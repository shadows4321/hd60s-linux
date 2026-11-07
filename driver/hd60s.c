// hd60s — userspace capture driver for the Elgato Game Capture HD60 S (0fd9:0076)
//
// Protocol reverse-engineered from a usbmon capture of the Windows driver; see
// ../PROTOCOL.md. This program never writes to the EDID EEPROM (I2C 0xa0).
//
// Build:  make
// Usage:  hd60s [options]
//   -v DEV      write frames to a v4l2loopback device. The device keeps a fixed
//               format (see -W); other input sizes are scaled, and black frames
//               are sent while there is no HDMI signal.
//   -W WxH      output size for -v (default 1920x1080)
//   -o FILE     write raw YUYV frames to FILE ("-" = stdout), at input size
//               unless -W is given
//   -a FILE     write raw audio (s16le, 48 kHz, stereo) to FILE/FIFO ("-" = stdout)
//   -n N        stop after N frames
//   -s          print the status block and exit (no streaming)
//   -A ALT      interface 0 alt setting for streaming (default 2)
//   -f FILE     offline test: decode a raw stream dump instead of the USB device
//   -g WxH      line width / progressive height for -f (default 1920x1080)
//   -q          quieter (no per-second stats)
//
// Exit codes: 0 normal stop, 1 error, 4 card not present / unplugged.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libusb-1.0/libusb.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VID 0x0fd9
#define PID 0x0076
#define EP_STREAM 0x83

#define REQ_I2C 192
#define REQ_BRIDGE 193
#define REQ_INIT_C2 194
#define REQ_C6 198
#define REQ_C7 199
#define REQ_EC 236

#define I2C_MAIN 0x0064
#define I2C_EDID 0x00a0 // never written by this program

#define NUM_XFERS 14 // 14 x 32 x 32 KiB = 14 MiB, under the 16 MiB usbfs default
#define PKTS_PER_XFER 32
#define CTRL_TIMEOUT 1000

#define MAXW 1920
#define MAXH 1200
#define FRAME_MAX ((size_t)MAXW * MAXH * 2)

static volatile sig_atomic_t g_stop;
static int g_quiet;
static int g_gone; // device disappeared

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "hd60s: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void msleep(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int parse_wxh(const char *s, int *w, int *h) {
    return sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0 && *w <= MAXW && *h <= MAXH &&
                   *w % 2 == 0
               ? 0
               : -1;
}

/* ---------------------------------------------------------------- control */

static libusb_device_handle *g_h;

static void note_err(int r) {
    if (r == LIBUSB_ERROR_NO_DEVICE)
        g_gone = 1;
}

static int vout(uint8_t req, uint16_t wv, uint16_t wi, const void *data, uint16_t len) {
    if (wv == I2C_EDID && req == REQ_I2C) {
        logf_("refusing to write EDID EEPROM");
        return -1;
    }
    int r = libusb_control_transfer(g_h, 0x40, req, wv, wi, (unsigned char *)data, len,
                                    CTRL_TIMEOUT);
    if (r < 0) {
        note_err(r);
        if (!g_gone)
            logf_("OUT req=%u wV=%04x wI=%04x failed: %s", req, wv, wi, libusb_error_name(r));
    }
    return r;
}

static int vin(uint8_t req, uint16_t wv, uint16_t wi, void *data, uint16_t len) {
    int r = libusb_control_transfer(g_h, 0xc0, req, wv, wi, data, len, CTRL_TIMEOUT);
    if (r < 0) {
        note_err(r);
        if (!g_gone)
            logf_("IN  req=%u wV=%04x wI=%04x failed: %s", req, wv, wi, libusb_error_name(r));
    }
    return r;
}

static int main_write(uint8_t reg, const void *data, uint16_t len) {
    vout(REQ_C6, 0, 0x0100, NULL, 0);
    return vout(REQ_I2C, I2C_MAIN, reg, data, len);
}

static int main_write1(uint8_t reg, uint8_t val) { return main_write(reg, &val, 1); }

struct status {
    uint16_t total_lines, total_width, height, width;
    uint8_t hz, flags, streaming;
    uint8_t raw[32];
};

static int read_status(struct status *st) {
    uint8_t b[32];
    int r = vin(REQ_I2C, I2C_MAIN, 0x00, b, sizeof b);
    if (r != (int)sizeof b)
        return -1;
    memcpy(st->raw, b, 32);
    st->total_lines = b[4] | b[5] << 8;
    st->total_width = b[6] | b[7] << 8;
    st->height = b[8] | b[9] << 8;
    st->width = b[10] | b[11] << 8;
    st->hz = b[12];
    st->flags = b[13];
    st->streaming = b[16];
    return 0;
}

static int has_signal(const struct status *st) {
    return st->width >= 320 && st->height >= 200 && st->width <= MAXW && st->height <= MAXH &&
           st->width % 2 == 0;
}

static void print_status(const struct status *st) {
    fprintf(stderr, "hd60s: status:");
    for (int i = 0; i < 32; i++)
        fprintf(stderr, " %02x", st->raw[i]);
    fprintf(stderr, "\nhd60s: signal: %s", st->width ? "yes" : "no");
    if (st->width)
        fprintf(stderr, "  %ux%u @ %u Hz  (raster %ux%u, flags %02x)", st->width, st->height,
                st->hz, st->total_width, st->total_lines, st->flags);
    fprintf(stderr, "  streaming=%u\n", st->streaming);
}

static int device_init(void) {
    uint8_t b[16];
    static const uint8_t ec[10] = {0xb8, 0x22, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x50, 0xca};
    static const uint8_t q1[5] = {0xab, 0x03, 0x12, 0x34, 0x57};
    static const uint8_t q2[5] = {0xab, 0x03, 0x12, 0x34, 0x58};
    static const uint8_t pic[4] = {0x80, 0x80, 0x80, 0x80};

    if (vout(REQ_EC, 0, 0, ec, sizeof ec) < 0)
        return -1;
    // fails with PIPE when the card was already initialised since power-up; harmless
    libusb_control_transfer(g_h, 0x40, REQ_BRIDGE, 0xc039, 0, NULL, 0, CTRL_TIMEOUT);
    vout(REQ_BRIDGE, 0x4134, 0, NULL, 0);
    if (vin(REQ_BRIDGE, 0x0039, 0, b, 1) == 1 && !g_quiet)
        logf_("bridge reg 0x39 = %02x", b[0]);

    vout(REQ_I2C, 0x5066, 0, q1, sizeof q1);
    vin(REQ_I2C, 0x5066, 0, b, 3);
    msleep(100);
    vin(REQ_I2C, 0x5066, 0, b, 3);
    msleep(100);
    vout(REQ_I2C, 0x5066, 0, q2, sizeof q2);
    vin(REQ_I2C, 0x5066, 0, b, 3);
    msleep(100);
    if (vin(REQ_I2C, 0x5066, 0, b, 3) == 3 && !g_quiet)
        logf_("controller id: %02x %02x %02x", b[0], b[1], b[2]);

    vout(REQ_INIT_C2, 0, 0, NULL, 0);
    vout(REQ_C7, 0x0064, 0, NULL, 0);
    vout(REQ_C6, 0, 0x0100, NULL, 0);
    main_write(0x13, pic, sizeof pic);
    main_write1(0x3a, 0x00);
    main_write1(0x3b, 0x80);
    return g_gone ? -1 : 0;
}

static void stream_start(int alt) {
    static const uint8_t c6[4] = {0x00, 0x40, 0xff, 0x32};
    vout(REQ_C6, 0x0032, 0x0101, c6, sizeof c6);
    main_write1(0x12, 0x00);
    main_write1(0x3a, 0x00);
    main_write1(0x3b, 0x80);
    main_write1(0x10, 0x01);
    int r = libusb_set_interface_alt_setting(g_h, 0, alt);
    if (r < 0) {
        note_err(r);
        logf_("set alt %d failed: %s", alt, libusb_error_name(r));
    }
}

static void stream_stop(void) {
    if (g_gone)
        return;
    libusb_set_interface_alt_setting(g_h, 0, 0);
    main_write1(0x10, 0x00);
}

/* ----------------------------------------------------------------- frames */

struct frame {
    uint8_t *data; // FRAME_MAX bytes
    int w, h;
};

static struct frame *frame_new(void) {
    struct frame *f = calloc(1, sizeof *f);
    if (f)
        f->data = malloc(FRAME_MAX);
    if (!f || !f->data) {
        logf_("out of memory");
        exit(1);
    }
    return f;
}

static void fill_black(struct frame *f, int w, int h) {
    uint32_t px = 0x80108010u; // bytes: 10 80 10 80 (Y=16, U=V=128)
    uint32_t *p = (uint32_t *)f->data;
    for (size_t i = 0, n = (size_t)w * h / 2; i < n; i++)
        p[i] = px;
    f->w = w;
    f->h = h;
}

/* Bilinear YUYV -> YUYV scaler (fixed point, 8-bit fractions). */
struct scaler {
    int sw, sh, dw, dh;
    int *lx0, *lx1, *lwx; // luma columns (dw)
    int *cx0, *cx1, *cwx; // chroma pairs (dw/2)
    int *y0, *y1, *wy;    // rows (dh)
};

static void map_axis(int src, int dst, int *i0, int *i1, int *w) {
    for (int d = 0; d < dst; d++) {
        // centre-aligned: s = (d + 0.5) * src / dst - 0.5, in 1/256 units
        long s = ((long)(2 * d + 1) * src * 256) / (2L * dst) - 128;
        if (s < 0)
            s = 0;
        int a = (int)(s >> 8);
        if (a >= src - 1) {
            i0[d] = i1[d] = src - 1;
            w[d] = 0;
        } else {
            i0[d] = a;
            i1[d] = a + 1;
            w[d] = (int)(s & 255);
        }
    }
}

static void scaler_setup(struct scaler *s, int sw, int sh, int dw, int dh) {
    if (s->sw == sw && s->sh == sh && s->dw == dw && s->dh == dh)
        return;
    free(s->lx0); // all arrays live in one allocation starting at lx0
    int *m = malloc(sizeof(int) * (size_t)(3 * dw + 3 * (dw / 2) + 3 * dh));
    s->lx0 = m;
    s->lx1 = m + dw;
    s->lwx = m + 2 * dw;
    s->cx0 = m + 3 * dw;
    s->cx1 = s->cx0 + dw / 2;
    s->cwx = s->cx1 + dw / 2;
    s->y0 = s->cwx + dw / 2;
    s->y1 = s->y0 + dh;
    s->wy = s->y1 + dh;
    map_axis(sw, dw, s->lx0, s->lx1, s->lwx);
    map_axis(sw / 2, dw / 2, s->cx0, s->cx1, s->cwx);
    map_axis(sh, dh, s->y0, s->y1, s->wy);
    s->sw = sw;
    s->sh = sh;
    s->dw = dw;
    s->dh = dh;
}

static inline int lerp8(int a, int b, int w) { return a + (((b - a) * w + 128) >> 8); }

static void scale_yuyv(struct scaler *s, const struct frame *src, struct frame *dst, int dw,
                       int dh) {
    scaler_setup(s, src->w, src->h, dw, dh);
    const size_t sstride = (size_t)src->w * 2;
    for (int y = 0; y < dh; y++) {
        const uint8_t *r0 = src->data + s->y0[y] * sstride;
        const uint8_t *r1 = src->data + s->y1[y] * sstride;
        int wy = s->wy[y];
        uint8_t *o = dst->data + (size_t)y * dw * 2;
        for (int x = 0; x < dw; x++) {
            int a = s->lx0[x] * 2, b = s->lx1[x] * 2, wx = s->lwx[x];
            int t = lerp8(r0[a], r0[b], wx), u = lerp8(r1[a], r1[b], wx);
            o[x * 2] = (uint8_t)lerp8(t, u, wy);
        }
        for (int c = 0; c < dw / 2; c++) {
            int a = s->cx0[c] * 4, b = s->cx1[c] * 4, wx = s->cwx[c];
            for (int k = 1; k <= 3; k += 2) { // U at +1, V at +3
                int t = lerp8(r0[a + k], r0[b + k], wx), u = lerp8(r1[a + k], r1[b + k], wx);
                o[c * 4 + k] = (uint8_t)lerp8(t, u, wy);
            }
        }
    }
    dst->w = dw;
    dst->h = dh;
}

/* ----------------------------------------------------------------- output */

struct out {
    int video_fd; // -1 if none
    int is_v4l2;
    int ow, oh; // fixed output size (0 = follow input; file output only)
    int audio_fd;
    // latest-wins hand-off to the writer thread
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct frame *ready;
    int has_ready;
    int blocking; // offline mode: wait for the writer instead of dropping
    uint64_t written, dropped_slow;
};

static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static int v4l2_setup(int fd, int w, int h) {
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    f.fmt.pix.width = w;
    f.fmt.pix.height = h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    f.fmt.pix.bytesperline = w * 2;
    f.fmt.pix.sizeimage = (uint32_t)w * h * 2;
    f.fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
    if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) {
        logf_("VIDIOC_S_FMT %dx%d failed: %s", w, h, strerror(errno));
        return -1;
    }
    return 0;
}

// Hand a filled frame to the writer; returns an empty frame to keep using.
static struct frame *post_frame(struct out *o, struct frame *f) {
    pthread_mutex_lock(&o->mu);
    while (o->blocking && o->has_ready && !g_stop)
        pthread_cond_wait(&o->cv, &o->mu);
    if (o->has_ready)
        o->dropped_slow++;
    struct frame *back = o->ready;
    o->ready = f;
    o->has_ready = 1;
    pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->mu);
    return back;
}

// Send one black frame at the output size (keeps a v4l2 reader alive).
static void post_black(struct out *o) {
    struct frame *f = frame_new();
    fill_black(f, o->ow, o->oh);
    struct frame *back = post_frame(o, f); // pool size stays constant
    free(back->data);
    free(back);
}

static void *writer_thread(void *arg) {
    struct out *o = arg;
    struct frame *mine = frame_new();
    struct frame *scaled = frame_new();
    struct scaler sc = {0};
    int warned = 0;
    for (;;) {
        pthread_mutex_lock(&o->mu);
        while (!o->has_ready && !g_stop)
            pthread_cond_wait(&o->cv, &o->mu);
        if (g_stop && !o->has_ready) {
            pthread_mutex_unlock(&o->mu);
            break;
        }
        struct frame *t = o->ready;
        o->ready = mine;
        mine = t;
        o->has_ready = 0;
        pthread_cond_broadcast(&o->cv); // wake a blocking post_frame
        pthread_mutex_unlock(&o->mu);

        const struct frame *f = mine;
        if (o->ow && (mine->w != o->ow || mine->h != o->oh)) {
            if (!warned++ && !g_quiet)
                logf_("scaling %dx%d -> %dx%d", mine->w, mine->h, o->ow, o->oh);
            scale_yuyv(&sc, mine, scaled, o->ow, o->oh);
            f = scaled;
        } else {
            warned = 0;
        }
        if (o->video_fd >= 0 && write_all(o->video_fd, f->data, (size_t)f->w * f->h * 2) < 0) {
            logf_("video write failed: %s", strerror(errno));
            g_stop = 1;
            break;
        }
        o->written++;
    }
    return NULL;
}

/* ----------------------------------------------------------------- parser */

enum { S_SCAN, S_LINE, S_AUDIO };

struct parser {
    int width, line_bytes;
    int exp_h;       // expected progressive height (from status)
    int state;
    uint32_t code;
    int in_active;   // inside a run of active lines
    int cur_f;       // field bit of the current run
    int run_lines;   // active lines seen in the current run
    int f0_lines;    // lines in the last field-1 run (interlaced)
    int interlaced;  // sticky once F=1 lines are seen
    int runs_seen;   // completed runs this session (the first is usually cut off)
    int row;         // destination row of the line being copied
    int pos, skip;
    uint8_t audio[8];
    int apos;
    struct frame *frame;
    struct out *out;
    uint64_t frames, partial, audio_samples;
    uint8_t abuf[65536];
    size_t alen;
};

static uint64_t g_audio_dropped;

// Audio fd is non-blocking: a stalled reader must never stall the USB thread.
// Writes of <= PIPE_BUF bytes to a pipe are atomic, so chunks stay sample-aligned.
static void flush_audio(struct parser *p) {
    size_t off = 0;
    while (p->out->audio_fd >= 0 && off < p->alen) {
        size_t n = p->alen - off > 4096 ? 4096 : p->alen - off;
        ssize_t w = write(p->out->audio_fd, p->abuf + off, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            g_audio_dropped += p->alen - off;
            break;
        }
        off += (size_t)w;
    }
    p->alen = 0;
}

static void parser_reset(struct parser *p, int width, int exp_h) {
    p->width = width;
    p->line_bytes = width * 2;
    p->exp_h = exp_h;
    p->state = S_SCAN;
    p->code = 0;
    p->in_active = 0;
    p->run_lines = p->f0_lines = 0;
    p->interlaced = 0;
    p->runs_seen = 0;
}

static void emit(struct parser *p, int h) {
    p->frame->w = p->width;
    p->frame->h = h;
    p->frame = post_frame(p->out, p->frame);
    p->frames++;
}

static void end_of_run(struct parser *p) {
    if (p->runs_seen++ == 0) { // first run: started mid-frame, and we don't yet
        p->in_active = 0;      // know whether the input is interlaced
        p->partial++;
        return;
    }
    if (!p->interlaced) {
        if (p->run_lines == p->exp_h)
            emit(p, p->run_lines);
        else
            p->partial++;
    } else if (p->cur_f == 0) {
        p->f0_lines = p->run_lines;
    } else {
        int h = p->f0_lines + p->run_lines;
        int d = p->f0_lines - p->run_lines;
        if (p->f0_lines > 0 && d >= -1 && d <= 1 && h <= MAXH)
            emit(p, h);
        else
            p->partial++;
        p->f0_lines = 0;
    }
    p->in_active = 0;
}

// BT.656 timing reference: ff 00 00 XY, XY = 1 F V H P3 P2 P1 P0
static void on_trs(struct parser *p, uint8_t xy) {
    if (!(xy & 0x80) || (xy & 0x10)) // not a code / EAV: ignore
        return;
    int f = !!(xy & 0x40), v = !!(xy & 0x20);
    if (v) {
        if (p->in_active)
            end_of_run(p);
        return;
    }
    if (!p->in_active || f != p->cur_f) {
        if (p->in_active)
            end_of_run(p);
        p->in_active = 1;
        p->cur_f = f;
        p->run_lines = 0;
        if (f && !p->interlaced) {
            p->interlaced = 1;
            p->f0_lines = 0; // the field before this was stored progressively; drop it
            if (!g_quiet)
                logf_("interlaced input detected");
        }
    }
    int row = p->interlaced ? p->run_lines * 2 + f : p->run_lines;
    p->run_lines++;
    p->skip = row >= MAXH || (!p->interlaced && row >= p->exp_h);
    p->row = row;
    p->state = S_LINE;
    p->pos = 0;
}

static void parse(struct parser *p, const uint8_t *d, int n) {
    int i = 0;
    while (i < n) {
        switch (p->state) {
        case S_LINE: {
            int want = p->line_bytes - p->pos;
            int take = n - i < want ? n - i : want;
            if (!p->skip)
                memcpy(p->frame->data + (size_t)p->row * p->line_bytes + p->pos, d + i, take);
            p->pos += take;
            i += take;
            if (p->pos == p->line_bytes) {
                p->state = S_SCAN;
                p->code = 0;
            }
            break;
        }
        case S_AUDIO:
            p->audio[p->apos++] = d[i++];
            if (p->apos == 8) {
                if (p->out->audio_fd >= 0) {
                    if (p->alen + 8 > sizeof p->abuf)
                        flush_audio(p);
                    memcpy(p->abuf + p->alen, p->audio, 8);
                    p->alen += 8;
                }
                p->audio_samples += 2;
                p->state = S_SCAN;
                p->code = 0;
            }
            break;
        default: // S_SCAN
            p->code = (p->code << 8) | d[i++];
            if ((p->code & 0xffffff00u) == 0xff000000u) {
                uint8_t xy = p->code & 0xff;
                p->code = 0;
                on_trs(p, xy);
            } else if (p->code == 0xff00ff02u) {
                p->state = S_AUDIO;
                p->apos = 0;
                p->code = 0;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------- iso xfers */

static uint64_t g_iso_err, g_iso_bytes;
static int g_inflight;
static int g_session_stop; // cancel the current streaming session

static void LIBUSB_CALL iso_cb(struct libusb_transfer *x) {
    struct parser *p = x->user_data;
    if (x->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int k = 0; k < x->num_iso_packets; k++) {
            struct libusb_iso_packet_descriptor *pd = &x->iso_packet_desc[k];
            if (pd->status != LIBUSB_TRANSFER_COMPLETED) {
                g_iso_err++;
                continue;
            }
            if (pd->actual_length) {
                parse(p, libusb_get_iso_packet_buffer_simple(x, k), pd->actual_length);
                g_iso_bytes += pd->actual_length;
            }
        }
        flush_audio(p);
    } else if (x->status == LIBUSB_TRANSFER_NO_DEVICE) {
        g_gone = 1;
    }
    if (!g_stop && !g_session_stop && !g_gone && x->status != LIBUSB_TRANSFER_CANCELLED &&
        libusb_submit_transfer(x) == 0)
        return;
    g_inflight--;
}

static void pump_events(int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    libusb_handle_events_timeout_completed(NULL, &tv, NULL);
}

/* One streaming session at a fixed input geometry. Returns when the signal is
 * lost or changes, the device goes away, or we are asked to stop. */
static void run_session(struct parser *p, struct out *o, const struct status *st, int alt,
                        long max_frames) {
    int pkt = libusb_get_max_alt_packet_size(libusb_get_device(g_h), 0, alt, EP_STREAM);
    if (pkt <= 0) {
        logf_("cannot get packet size for alt %d: %s", alt, libusb_error_name(pkt));
        g_stop = 1;
        return;
    }
    parser_reset(p, st->width, st->height);
    g_session_stop = 0;
    stream_start(alt);
    if (g_gone)
        return;
    logf_("streaming %ux%u @ %u Hz", st->width, st->height, st->hz);

    static struct libusb_transfer *xf[NUM_XFERS];
    static uint8_t *bufs[NUM_XFERS];
    for (int k = 0; k < NUM_XFERS; k++) {
        if (!xf[k]) {
            xf[k] = libusb_alloc_transfer(PKTS_PER_XFER);
            bufs[k] = malloc((size_t)pkt * PKTS_PER_XFER);
        }
        libusb_fill_iso_transfer(xf[k], g_h, EP_STREAM, bufs[k], pkt * PKTS_PER_XFER,
                                 PKTS_PER_XFER, iso_cb, p, 1000);
        libusb_set_iso_packet_lengths(xf[k], pkt);
        if (libusb_submit_transfer(xf[k]) == 0)
            g_inflight++;
        else
            logf_("submit transfer %d failed", k);
    }

    double t_stat = now_s(), t_poll = t_stat, t_frame = t_stat;
    uint64_t last_bytes = g_iso_bytes, last_frames = p->frames, seen_frames = p->frames;
    while (!g_stop && !g_gone && g_inflight > 0) {
        pump_events(100);
        if (max_frames && (long)p->frames >= max_frames)
            g_stop = 1;
        double t = now_s();
        if (p->frames != seen_frames) {
            seen_frames = p->frames;
            t_frame = t;
        }
        if (t - t_poll >= 0.5) {
            struct status s2;
            t_poll = t;
            if (read_status(&s2) == 0) {
                if (!has_signal(&s2)) {
                    logf_("HDMI signal lost");
                    break;
                }
                if (s2.width != st->width || s2.height != st->height) {
                    logf_("input changed to %ux%u @ %u Hz", s2.width, s2.height, s2.hz);
                    break;
                }
            }
        }
        if (o->is_v4l2 && t - t_frame > 0.5) { // signal present but no frames decoded
            post_black(o);
            t_frame = t;
        }
        if (t - t_stat >= 1.0 && !g_quiet) {
            logf_("%.1f fps  %.0f MB/s  frames=%llu partial=%llu slow-drop=%llu iso-err=%llu "
                  "audio=%llu",
                  (p->frames - last_frames) / (t - t_stat),
                  (g_iso_bytes - last_bytes) / (t - t_stat) / 1e6,
                  (unsigned long long)p->frames, (unsigned long long)p->partial,
                  (unsigned long long)o->dropped_slow, (unsigned long long)g_iso_err,
                  (unsigned long long)p->audio_samples);
            last_frames = p->frames;
            last_bytes = g_iso_bytes;
            t_stat = t;
        }
    }
    if (g_inflight == 0 && !g_stop)
        g_gone = 1; // all transfers died without us cancelling them

    g_session_stop = 1;
    for (int k = 0; k < NUM_XFERS; k++)
        libusb_cancel_transfer(xf[k]);
    double t_end = now_s() + 2;
    while (g_inflight > 0 && now_s() < t_end)
        pump_events(100);
    stream_stop();
}

/* ---------------------------------------------------------- offline test */

static int run_offline(const char *path, int w, int h, struct parser *p) {
    FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
    if (!f) {
        logf_("open %s: %s", path, strerror(errno));
        return 1;
    }
    parser_reset(p, w, h);
    static uint8_t buf[32768];
    size_t n;
    while (!g_stop && (n = fread(buf, 1, sizeof buf, f)) > 0) {
        parse(p, buf, (int)n);
        flush_audio(p);
    }
    if (f != stdin)
        fclose(f);
    logf_("offline: %llu frames, %llu partial, %llu audio samples, interlaced=%d",
          (unsigned long long)p->frames, (unsigned long long)p->partial,
          (unsigned long long)p->audio_samples, p->interlaced);
    return 0;
}

/* ------------------------------------------------------------------- main */

static void on_signal(int s) {
    (void)s;
    g_stop = 1;
}

static int open_out(const char *path, int flags) {
    if (!strcmp(path, "-"))
        return STDOUT_FILENO;
    int fd = open(path, flags, 0644);
    if (fd < 0)
        logf_("open %s: %s", path, strerror(errno));
    return fd;
}

int main(int argc, char **argv) {
    const char *out_path = NULL, *v4l2_path = NULL, *audio_path = NULL, *offline = NULL;
    long max_frames = 0;
    int status_only = 0, alt = 2, opt;
    int ow = 0, oh = 0, gw = 1920, gh = 1080;
    while ((opt = getopt(argc, argv, "o:v:a:n:sA:W:f:g:qh")) != -1) {
        switch (opt) {
        case 'o': out_path = optarg; break;
        case 'v': v4l2_path = optarg; break;
        case 'a': audio_path = optarg; break;
        case 'n': max_frames = atol(optarg); break;
        case 's': status_only = 1; break;
        case 'A': alt = atoi(optarg); break;
        case 'f': offline = optarg; break;
        case 'q': g_quiet = 1; break;
        case 'W':
            if (parse_wxh(optarg, &ow, &oh)) {
                logf_("bad -W %s", optarg);
                return 2;
            }
            break;
        case 'g':
            if (parse_wxh(optarg, &gw, &gh)) {
                logf_("bad -g %s", optarg);
                return 2;
            }
            break;
        default:
            fprintf(stderr,
                    "usage: %s [-v /dev/videoN] [-W WxH] [-o FILE|-] [-a FILE] [-n N] [-s] "
                    "[-A ALT] [-f DUMP -g WxH] [-q]\n",
                    argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (v4l2_path && !ow) {
        ow = 1920;
        oh = 1080;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    struct out o = {.video_fd = -1, .audio_fd = -1, .ow = ow, .oh = oh};
    pthread_mutex_init(&o.mu, NULL);
    pthread_cond_init(&o.cv, NULL);
    o.ready = frame_new();

    if (v4l2_path) {
        o.video_fd = open(v4l2_path, O_WRONLY);
        if (o.video_fd < 0 || v4l2_setup(o.video_fd, ow, oh) < 0) {
            logf_("cannot use %s: %s", v4l2_path, strerror(errno));
            return 1;
        }
        o.is_v4l2 = 1;
    } else if (out_path) {
        if ((o.video_fd = open_out(out_path, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
            return 1;
    }
    if (audio_path) {
        if ((o.audio_fd = open_out(audio_path, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
            return 1;
        fcntl(o.audio_fd, F_SETFL, fcntl(o.audio_fd, F_GETFL) | O_NONBLOCK);
        // A FIFO's default 64 KiB buffer is ~340 ms of audio; once full, every
        // reader hears that much lag. Keep it at 8 KiB (~43 ms).
        struct stat sb;
        if (fstat(o.audio_fd, &sb) == 0 && S_ISFIFO(sb.st_mode) &&
            fcntl(o.audio_fd, F_SETPIPE_SZ, 8192) < 0)
            logf_("could not shrink audio pipe: %s", strerror(errno));
    }

    struct parser p = {0};
    p.out = &o;
    p.frame = frame_new();
    pthread_t wt;
    pthread_create(&wt, NULL, writer_thread, &o);

    int rc = 0;
    if (offline) {
        o.blocking = 1;
        rc = run_offline(offline, gw, gh, &p);
        goto out_writer;
    }

    if (libusb_init_context(NULL, NULL, 0) < 0) {
        rc = 1;
        goto out_writer;
    }
    g_h = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!g_h) {
        logf_("HD60 S (%04x:%04x) not found or no permission (see 60-hd60s.rules)", VID, PID);
        rc = 4;
        goto out_exit;
    }
    libusb_set_auto_detach_kernel_driver(g_h, 1);
    for (int ifc = 0; ifc < 2; ifc++) {
        int r = libusb_claim_interface(g_h, ifc);
        if (r < 0) {
            logf_("claim interface %d: %s", ifc, libusb_error_name(r));
            rc = 1;
            goto out_close;
        }
    }
    libusb_set_interface_alt_setting(g_h, 0, 0);
    if (device_init() < 0) {
        rc = g_gone ? 4 : 1;
        goto out_release;
    }

    struct status st;
    if (status_only) {
        msleep(300);
        if (read_status(&st) == 0)
            print_status(&st);
        goto out_release;
    }

    // Main loop: stream while there is a signal; show black while there isn't.
    int said_wait = 0;
    double t_black = 0;
    while (!g_stop && !g_gone) {
        if (read_status(&st) == 0 && has_signal(&st)) {
            said_wait = 0;
            if (!g_quiet)
                print_status(&st);
            run_session(&p, &o, &st, alt, max_frames);
            continue;
        }
        if (!said_wait) {
            logf_("waiting for HDMI signal...");
            said_wait = 1;
        }
        double t = now_s();
        if (o.is_v4l2 && t - t_black >= 0.1) { // keep the camera alive at 10 fps
            post_black(&o);
            t_black = t;
        }
        pump_events(100);
    }
    if (g_gone) {
        logf_("card unplugged");
        rc = 4;
    }
    if (o.is_v4l2)
        post_black(&o); // leave OBS showing black rather than a frozen frame
    logf_("done: %llu frames, %llu partial, %llu written, %llu audio bytes dropped",
          (unsigned long long)p.frames, (unsigned long long)p.partial,
          (unsigned long long)o.written, (unsigned long long)g_audio_dropped);

out_release:
    if (!g_gone) {
        libusb_release_interface(g_h, 1);
        libusb_release_interface(g_h, 0);
    }
out_close:
    libusb_close(g_h);
out_exit:
    libusb_exit(NULL);
out_writer:
    g_stop = 1;
    pthread_mutex_lock(&o.mu);
    pthread_cond_broadcast(&o.cv);
    pthread_mutex_unlock(&o.mu);
    pthread_join(wt, NULL);
    return rc;
}
