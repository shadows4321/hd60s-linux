#!/usr/bin/env python3
"""Turn usbmon control traffic into a readable transcript.

Usage: ctrl_transcript.py capture.pcapng [--all]
By default prints only vendor requests (bmRequestType 0x40/0xc0) plus
SET_INTERFACE / SET_CONFIGURATION, and interrupt EP data.
Output: t(s)  dir  req  wValue  wIndex  wLen  status  data
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from isoextract import blocks  # noqa: E402


def main():
    src = sys.argv[1]
    show_all = "--all" in sys.argv
    pending = {}
    t0 = None
    with open(src, "rb") as f:
        for bt, body, e in blocks(f):
            if bt != 6:
                continue
            caplen = struct.unpack(e + "I", body[12:16])[0]
            pkt = body[20:20 + caplen]
            (urb_id, typ, xfer, ep, dev, bus, fsetup, fdata, sec, usec,
             status, length, lcap) = struct.unpack("<QBBBBHccqiiII", pkt[:40])
            t = sec + usec / 1e6
            if t0 is None:
                t0 = t
            data = pkt[64:64 + lcap]
            typ = chr(typ)
            if xfer == 1 and typ == "C" and data:  # interrupt IN
                print(f"{t - t0:10.6f}  INT  ep{ep & 0x7f}  len={len(data)}  {data.hex(' ')}")
                continue
            if xfer != 2:
                continue
            if typ == "S" and fsetup == b"\x00":
                bm, req, wv, wi, wl = struct.unpack("<BBHHH", pkt[40:48])
                pending[urb_id] = (t, bm, req, wv, wi, wl, data)
            elif typ == "C" and urb_id in pending:
                ts, bm, req, wv, wi, wl, odata = pending.pop(urb_id)
                vendor = (bm & 0x60) == 0x40
                interesting = vendor or (bm == 0x01 and req == 11) or (bm == 0x00 and req == 9)
                if not (interesting or show_all):
                    continue
                d = "IN " if bm & 0x80 else "OUT"
                payload = data if bm & 0x80 else odata
                print(f"{ts - t0:10.6f}  {d}  bm={bm:02x} req={req:3d} "
                      f"wV={wv:04x} wI={wi:04x} wL={wl:5d} st={status:3d}  "
                      f"{payload[:64].hex(' ')}{' …' if len(payload) > 64 else ''}")


if __name__ == "__main__":
    main()
