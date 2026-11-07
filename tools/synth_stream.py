#!/usr/bin/env python3
"""Generate a synthetic HD60 S stream (same framing the card sends) for testing.

Usage: synth_stream.py WxH[i] frames out.bin

Each active pixel's luma encodes its frame row: Y = 16 + (row % 200), chroma
is a horizontal ramp. For interlaced ('i' suffix) the height is the full frame
height; field 1 (F=0) carries even rows, field 2 (F=1) odd rows.
Audio blocks (ff 00 ff 02 + 8 bytes) are inserted before every 3rd line.
"""
import sys


def line_bytes(w, row):
    y = 16 + (row % 200)
    out = bytearray(w * 2)
    for x in range(0, w, 2):
        c = 16 + (x * 224 // w)
        out[x * 2:x * 2 + 4] = bytes((y, c, y, 240 - c + 16 if 240 - c + 16 < 240 else 239))
    return bytes(out)


def main():
    mode, frames, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    inter = mode.endswith("i")
    w, h = (int(v) for v in mode.rstrip("i").split("x"))
    blank = bytes(w * 2)
    audio = b"\xff\x00\xff\x02" + bytes((1, 0, 1, 0, 1, 0, 1, 0))
    cache = {}
    n = 0
    with open(path, "wb") as f:
        def emit_line(code, data):
            nonlocal n
            if n % 3 == 2:
                f.write(audio)
            f.write(b"\xff\x00\x00" + bytes((code,)) + data)
            n += 1

        for _ in range(frames):
            fields = [(0, range(0, h, 2)), (1, range(1, h, 2))] if inter else [(0, range(h))]
            for fbit, rows in fields:
                vb = 0xEC if fbit else 0xAB
                act = 0xC7 if fbit else 0x80
                for _ in range(22 if inter else 45):
                    emit_line(vb, blank)
                for r in rows:
                    if r not in cache:
                        cache[r] = line_bytes(w, r)
                    emit_line(act, cache[r])
            # trailing blanking so the last frame/field is terminated
        for _ in range(5):
            emit_line(0xAB, blank)


if __name__ == "__main__":
    main()
