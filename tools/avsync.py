#!/usr/bin/env python3
"""Measure audio/video offset in a recording of a flash+click sync test.

Usage: avsync.py recording.mkv [crop=W:H:X:Y]

The crop selects the region that flashes (default: the red circle of the
soundbase.app "Audio & Video Sync" page at 1080p). Prints audio-minus-video
offset per click; negative = audio early (video late). Needs ffmpeg and numpy.
"""
import subprocess
import sys

import numpy as np


def ff(args):
    return subprocess.run(["ffmpeg", "-loglevel", "error", *args, "-"],
                          check=True, capture_output=True).stdout


def main():
    path = sys.argv[1]
    crop = sys.argv[2] if len(sys.argv) > 2 else "200:200:1120:548"
    fps = 60
    rgb = np.frombuffer(ff(["-i", path, "-map", "0:v", "-vf",
                            f"crop={crop},scale=20:20,fps={fps}", "-pix_fmt", "rgb24",
                            "-f", "rawvideo"]), np.uint8).reshape(-1, 20, 20, 3).astype(float)
    pcm = np.frombuffer(ff(["-i", path, "-map", "0:a", "-ac", "1", "-ar", "48000",
                            "-f", "s16le"]), np.int16).astype(float)

    red = rgb[..., 0].mean((1, 2)) - rgb[..., 2].mean((1, 2))
    thr = (red.max() + red.min()) / 2
    flashes = [i / fps for i in range(1, len(red)) if red[i] > thr >= red[i - 1]]

    env = np.abs(pcm[:len(pcm) // 48 * 48]).reshape(-1, 48).max(1)  # 1 ms
    athr = env.max() * 0.3
    clicks, last = [], -1000
    for i, e in enumerate(env):
        if e > athr and i - last > 300:
            clicks.append(i / 1000)
        if e > athr:
            last = i

    offs = []
    for t in clicks:
        v = min(flashes, key=lambda f: abs(f - t))
        if abs(t - v) < 0.5:
            offs.append((t - v) * 1000)
    if not offs:
        sys.exit("no click/flash pairs found")
    print(f"{len(offs)} click/flash pairs over {len(pcm) / 48000:.1f} s")
    print("audio minus video (ms):", [round(o) for o in offs])
    print(f"median {np.median(offs):+.0f} ms  (spread {min(offs):+.0f} .. {max(offs):+.0f})")
    if len(offs) > 4:
        slope = np.polyfit(range(len(offs)), offs, 1)[0]
        print(f"drift {slope:+.2f} ms per click")


if __name__ == "__main__":
    main()
