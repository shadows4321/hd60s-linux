#!/usr/bin/env python3
"""Offline self-test: generate synthetic HD60 S streams, decode them with the
driver's -f mode, and verify every output row. No hardware needed.

Usage: check_synth.py path/to/hd60s
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd):
    subprocess.run(cmd, check=True, stderr=subprocess.DEVNULL)


def rows_luma(path, w, h):
    """Yield (frame, row, mean luma of the row's centre) for a YUYV file."""
    fb = w * h * 2
    with open(path, "rb") as f:
        n = 0
        while True:
            buf = f.read(fb)
            if len(buf) < fb:
                return
            for r in range(h):
                line = buf[r * w * 2:(r + 1) * w * 2]
                ys = line[40:-40:2]
                yield n, r, sum(ys) / len(ys)
            n += 1


def case(driver, tmp, mode, geom, out_wh, expect_frames):
    src = os.path.join(tmp, mode + ".bin")
    dst = os.path.join(tmp, mode + ".yuv")
    run([sys.executable, "-I", os.path.join(HERE, "synth_stream.py"), mode, "4", src])
    cmd = [driver, "-q", "-f", src, "-g", geom, "-o", dst]
    w, h = 1920, 1080
    if out_wh:
        cmd += ["-W", out_wh]
        w, h = (int(v) for v in out_wh.split("x"))
    run(cmd)
    frames = os.path.getsize(dst) // (w * h * 2)
    src_h = int(mode.rstrip("i").split("x")[1])
    worst = 0.0
    for _, r, y in rows_luma(dst, w, h):
        s = min(max(round((r + 0.5) * src_h / h - 0.5), 0), src_h - 1)
        if s % 200 in (0, 199):  # rows next to the pattern's wrap blend legitimately
            continue
        worst = max(worst, abs(y - (16 + s % 200)))
    ok = frames == expect_frames and worst <= 1.0
    print(f"{'PASS' if ok else 'FAIL'}  {mode:12s} -> {w}x{h}: {frames} frames, "
          f"max row error {worst:.2f}")
    return ok


def main():
    driver = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        results = [
            case(driver, tmp, "1920x1080", "1920x1080", None, 3),
            case(driver, tmp, "1920x1080i", "1920x540", None, 3),
            case(driver, tmp, "1280x720", "1280x720", "1920x1080", 3),
        ]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
