#!/usr/bin/env python3
"""Print ISO packet length sequence and bytes between short packets."""
import struct, sys
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from isoextract import blocks
lens = []
linktypes = []
with open(sys.argv[1], "rb") as f:
    for bt, body, e in blocks(f):
        if bt == 1:
            linktypes.append(struct.unpack(e + "H", body[:2])[0]); continue
        if bt != 6: continue
        caplen = struct.unpack(e + "I", body[12:16])[0]
        pkt = body[20:20 + caplen]
        typ, xfer, ep = pkt[8], pkt[9], pkt[10]
        if xfer != 0 or ep != 0x83 or typ != ord("C"): continue
        nd = struct.unpack("<i", pkt[44:48])[0]
        for i in range(nd):
            st, off, ln, _ = struct.unpack("<iIII", pkt[64 + i*16:80 + i*16])
            lens.append(ln)
# runs between short packets
acc = 0; runs = []
for i, l in enumerate(lens):
    acc += l
    if l < 32768:
        runs.append((i, l, acc)); acc = 0
for r in runs[:40]:
    print("pkt#%d short=%d bytes_since_last_short=%d" % r)
from collections import Counter
print(Counter(r[2] for r in runs).most_common(8))
