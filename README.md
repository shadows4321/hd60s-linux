# hd60s-linux

A Linux driver for the original **Elgato Game Capture HD60 S** (USB ID `0fd9:0076`).

Elgato only supports this card on Windows and macOS, and unlike the HD60 S+ it is
not a standard UVC webcam device, so Linux has never been able to use it. This
project is a small userspace driver, built from a reverse-engineered protocol,
that makes it show up in OBS (or any V4L2 app) as a normal camera plus an audio
input.

> Not affiliated with or endorsed by Elgato / Corsair. "Elgato" and "HD60 S" are
> their trademarks. This project contains no Elgato code or firmware. It was
> written from USB traffic captures, for interoperability.

## Status

| | |
|---|---|
| 1080p60 video | ✅ tested on hardware: steady 60 fps, 260 MB/s, no dropped frames |
| Audio (48 kHz stereo) | ✅ tested on hardware; no drift over 10 min (needs an OBS sync offset, see below) |
| HDMI unplug / replug | ✅ camera stays open and shows black while there's no signal |
| 720p and other progressive modes | ⚠️ scaled to the output size; only tested with synthetic streams |
| 1080i | ⚠️ field weaving implemented; only tested with synthetic streams |
| HDCP-protected sources | ❌ blank, same as on Windows (turn HDCP off on your console) |
| HD60 S+ / HD60 X / Cam Link | Not needed: those are UVC devices that already work on Linux |

Reports from other sources and modes (consoles, 720p, 1080i) are very welcome.

## Requirements

- USB 3.0 port
- `libusb-1.0` development files (Fedora `libusb1-devel`, Debian/Ubuntu `libusb-1.0-0-dev`, Arch `libusb`)
- [`v4l2loopback`](https://github.com/umlaeute/v4l2loopback) kernel module
- PipeWire with its PulseAudio server (`pactl`) for audio
- gcc, make, pkg-config; Python 3 for `make test`

## Build, test and install

```sh
cd driver
make
make test                 # offline decoder self-test, no hardware needed
sudo make install         # /usr/local/bin, udev rule, systemd user unit
sudo udevadm control --reload
systemctl --user daemon-reload
systemctl --user enable hd60s.service
```

The udev rule gives the logged-in user access to the card and starts
`hd60s.service` when it's plugged in. The service stops when it's unplugged.

### v4l2loopback

The driver writes into a v4l2loopback device labelled **"HD60 S"**. See
[`driver/v4l2loopback.conf.example`](driver/v4l2loopback.conf.example). Copy
it to `/etc/modprobe.d/v4l2loopback.conf`, or merge it into your existing
v4l2loopback options, and make sure `v4l2loopback` is listed in
`/etc/modules-load.d/`. Then reboot, or reload the module.

## Usage in OBS

1. Plug in the HD60 S (USB 3.0) and your HDMI source.
2. **Video:** Sources → + → *Video Capture Device (V4L2)* → Device: **HD60 S**.
   **Uncheck "Use buffering"** in its properties. With it on, OBS holds video back
   by roughly 0.4 s.
3. **Audio:** Sources → + → *Audio Input Capture (PulseAudio)* → Device: **HD60 S Audio**
4. **A/V sync:** Edit → Advanced Audio Properties → HD60 S Audio → **Sync Offset ≈ -160 ms**.
   Measured with a flash/click test (`tools/avsync.py`). Audio arrives about 160 ms
   after the matching video, and stays stable over 10 minutes (no drift). Your value
   may differ slightly, so check with your own source.

### Running by hand

```sh
hd60s -s                                   # print the input signal status and exit
hd60s-obs                                  # camera + audio input, until Ctrl+C
hd60s -o - | ffplay -f rawvideo -pixel_format yuyv422 -video_size 1920x1080 -framerate 60 -
```

`hd60s -h` lists all options: output size (`-W`), raw audio output (`-a`),
offline decoding of stream dumps (`-f`), and more.

## How it works

The card sends uncompressed YUYV 4:2:2 over an isochronous endpoint. Each row
starts with a BT.656-style timing code, and audio samples are mixed in between
rows. Setup and status go through vendor control requests to chips on the
card's internal I2C bus. The full write-up is in [PROTOCOL.md](PROTOCOL.md).

- `driver/hd60s.c`: the driver (libusb, V4L2 output, bilinear scaler, field weaving)
- `driver/hd60s-obs`: launcher that creates the PipeWire input and runs the driver
- `tools/`: scripts used for the reverse engineering (usbmon pcap parsing, stream
  decoding, synthetic stream generator and self-test)

The driver never writes to the card's EDID EEPROM.

## Contributing captures

To support a new mode, capture the Windows driver with USB passthrough to a VM and
`usbmon` on the Linux host. That's how this project was made:

```sh
sudo modprobe usbmon
dumpcap -i usbmonN -w capture.pcapng   # N = the card's USB bus number
python3 tools/ctrl_transcript.py capture.pcapng
python3 tools/isoextract.py capture.pcapng stream.bin
hd60s -f stream.bin -g 1920x1080 -o frames.yuv
```

Raw captures contain whatever was on screen. Check them before sharing.

## License

MIT, see [LICENSE](LICENSE).
