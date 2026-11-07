#!/usr/bin/env python3
"""Extract the raw isochronous payload stream from a usbmon pcapng capture.

Usage: isoextract.py capture.pcapng out.bin [endpoint=0x83]

Writes the concatenated ISO packet payloads (in order) to out.bin and prints
per-packet stats (lengths, statuses, truncation) to stderr.
"""
import struct
import sys

LINKTYPE_USB_LINUX_MMAPPED = 220


def blocks(f):
    endian = "<"
    while True:
        hdr = f.read(8)
        if len(hdr) < 8:
            return
        btype, blen = struct.unpack(endian + "II", hdr)
        if btype == 0x0A0D0D0A:  # section header: detect endianness
            bom = f.read(4)
            endian = "<" if bom == b"\x4d\x3c\x2b\x1a" else ">"
            blen = struct.unpack(endian + "I", hdr[4:])[0]
            body = bom + f.read(blen - 12)
        else:
            body = f.read(blen - 8)
        yield btype, body[:-4], endian


def main():
    src, dst = sys.argv[1], sys.argv[2]
    ep_want = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x83
    linktypes = []
    n_urbs = n_pkts = n_trunc = 0
    sizes = {}
    with open(src, "rb") as f, open(dst, "wb") as out:
        for btype, body, e in blocks(f):
            if btype == 1:  # IDB
                linktypes.append(struct.unpack(e + "H", body[:2])[0])
                continue
            if btype != 6:  # EPB only
                continue
            if_id, _th, _tl, caplen, _origlen = struct.unpack(e + "IIIII", body[:20])
            if linktypes[if_id] != LINKTYPE_USB_LINUX_MMAPPED:
                continue
            pkt = body[20:20 + caplen]
            # usbmon mmapped header (64 bytes, host endian = little)
            (urb_id, typ, xfer, epnum, devnum, busnum, flag_setup, flag_data,
             ts_sec, ts_usec, status, length, len_cap) = struct.unpack(
                "<QBBBBHccqiiII", pkt[:40])
            if xfer != 0 or epnum != ep_want or typ != ord("C"):
                continue
            err_count, numdesc = struct.unpack("<ii", pkt[40:48])
            n_urbs += 1
            data = pkt[64:]
            descs = data[:numdesc * 16]
            payload = data[numdesc * 16:]
            for i in range(numdesc):
                st, off, ln, _pad = struct.unpack("<iIII", descs[i * 16:i * 16 + 16])
                n_pkts += 1
                chunk = payload[off:off + ln]
                if len(chunk) < ln:
                    n_trunc += 1
                sizes[(st, ln)] = sizes.get((st, ln), 0) + 1
                # Mark truncation with zero fill so frame strides stay correct
                out.write(chunk + b"\x00" * (ln - len(chunk)))
    print(f"URBs: {n_urbs}  ISO packets: {n_pkts}  truncated(zero-filled): {n_trunc}",
          file=sys.stderr)
    for (st, ln), c in sorted(sizes.items(), key=lambda x: -x[1])[:10]:
        print(f"  status={st} len={ln}: {c}", file=sys.stderr)


if __name__ == "__main__":
    main()
