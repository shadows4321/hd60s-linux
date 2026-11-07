#!/usr/bin/env python3
"""Decode HD60 S raw stream (concatenated ISO payload) into PNG frames + WAV audio.

Stream format (as reverse-engineered so far):
  repeated "lines":
    [optional audio block: ff 00 ff 02 + 8 bytes (2x stereo s16le)]
    SAV: ff 00 00 XY   (XY=0x80 active video, XY=0xab vertical blanking)
    3840 bytes YUYV 4:2:2 (1920 px)

Usage: decode_frames.py stream.bin outdir [max_frames]
"""
import os
import re
import sys
import wave

import numpy as np
from PIL import Image

W = 1920
LINE = W * 2


def yuyv_to_rgb(buf, h):
    a = np.frombuffer(buf, dtype=np.uint8).reshape(h, W // 2, 4).astype(np.float32)
    y0, u, y1, v = a[..., 0], a[..., 1] - 128, a[..., 2], a[..., 3] - 128
    y = np.stack([y0, y1], -1).reshape(h, W) - 16
    u = np.repeat(u, 2, axis=1)
    v = np.repeat(v, 2, axis=1)
    # BT.709 limited range
    r = 1.164 * y + 1.793 * v
    g = 1.164 * y - 0.213 * u - 0.533 * v
    b = 1.164 * y + 2.112 * u
    return np.clip(np.stack([r, g, b], -1), 0, 255).astype(np.uint8)


def main():
    src, outdir = sys.argv[1], sys.argv[2]
    max_frames = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    os.makedirs(outdir, exist_ok=True)
    d = open(src, "rb").read()

    audio = bytearray()
    for m in re.finditer(b"\xff\x00\xff\x02", d):
        audio += d[m.start() + 4:m.start() + 12]

    frames = []
    cur = []
    in_blank = True
    for m in re.finditer(b"\xff\x00\x00[\x80\xab]", d):
        xy = d[m.start() + 3]
        if xy == 0xAB:
            if cur:
                frames.append(cur)
                cur = []
            in_blank = True
            continue
        if in_blank and not frames and not cur:
            pass
        in_blank = False
        line = d[m.start() + 4:m.start() + 4 + LINE]
        # strip a trailing audio block that may sit inside the slice
        if len(line) == LINE:
            cur.append(line)

    # first group is partial (capture started mid-frame): skip it
    full = frames[1:1 + max_frames]
    for i, lines in enumerate(full):
        img = yuyv_to_rgb(b"".join(lines), len(lines))
        Image.fromarray(img).save(os.path.join(outdir, f"frame{i:02d}.png"))
        print(f"frame{i:02d}: {len(lines)} active lines")

    with wave.open(os.path.join(outdir, "audio.wav"), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(bytes(audio))
    print(f"audio: {len(audio) // 4} stereo samples "
          f"({len(audio) / 4 / 48000:.2f}s at 48 kHz) over {len(frames)} frames")


if __name__ == "__main__":
    main()
