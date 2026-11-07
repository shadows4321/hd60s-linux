# Elgato Game Capture HD60 S (0fd9:0076) — reverse-engineered protocol notes

Source: usbmon capture of the Windows driver (Elgato 4K Capture Utility) running in a
VirtualBox Windows 11 VM with USB passthrough, 2026-11-06. Raw captures are not
published (they contain screen content); `tools/` has the scripts used to analyse them.

## USB layout
- USB 3.0 SuperSpeed, vendor-specific class, 1 configuration, 2 interfaces.
- Interface 0 (video+audio stream), EP 0x83 IN:
  - alt 0: zero bandwidth
  - alt 1: isochronous, 16384 B/µframe
  - alt 2: isochronous, 32768 B/µframe (≈262 MB/s) ← Windows uses this for 1080p60
  - alt 3: isochronous, 39936 B/µframe
  - alt 4: bulk
- Interface 1, EP 0x81 IN interrupt (64 B): 1-byte event (`01`) on HDMI signal change.
- Firmware lives on the device (no host firmware upload).

## Vendor control requests (bmRequestType 0x40 OUT / 0xC0 IN)
| bRequest | wValue | wIndex | Meaning (best guess) |
|---|---|---|---|
| 192 (0xC0) | I2C addr (8-bit) | register | I2C write (OUT) / read (IN) through the bridge |
| 193 (0xC1) | (val<<8)\|reg | 0 | bridge register write (OUT); IN wValue=reg reads 1 byte |
| 194 (0xC2) | 0 | 0 | init step, no data |
| 198 (0xC6) | 0 | 0x0100 | sent before most I2C ops to chip 0x64 (bus select/lock?) |
| 198 (0xC6) | 0x0032 | 0x0101 | 4 bytes `00 40 ff 32`, sent once after signal is detected |
| 199 (0xC7) | 0x0064 | 0 | init step |
| 236 (0xEC) | 0 | 0 | 10 bytes `b8 22 00 00 00 c0 00 00 50 ca`, first command after attach |

wValue 0x5066 on req 192 = I2C addr 0x66 with flag 0x50: small challenge/response
or version query during init (`ab 03 12 34 57` → reads `33 44 55`, `51 10 27`;
`ab 03 12 34 58` → `51 10 27`, `14 09 18`). Replayed verbatim.

### I2C chip 0x64 (main controller)
- reg 0x00, read 32 bytes = status block (polled every ~100 ms):
  - [4..5]  u16le total lines   (0x0465 = 1125)
  - [6..7]  u16le total width   (0x0898 = 2200)
  - [8..9]  u16le active lines  (0x0438 = 1080)
  - [10..11] u16le active width (0x0780 = 1920)
  - [12]    refresh Hz (0x3c = 60)
  - [13]    flags? (0x10)
  - [14]    0x30 constant
  - [16]    1 while streaming (mirrors reg 0x10)
  - [19..22] mirrors reg 0x13..0x16 (`80 80 80 80`, likely picture controls)
  - all-zero geometry = no signal
- reg 0x10 ← 1 start stream, 0 stop
- reg 0x12 ← 0, reg 0x3a ← 0, reg 0x3b ← 0x80 (set before streaming)
- reg 0x13 ← `80 80 80 80` (init)

### I2C chip 0xa0 — EDID EEPROM (DO NOT WRITE)
Windows software reads and rewrites the EDID the card presents to the source.
Not needed for capture.

## Init sequence (as observed)
1. SET_INTERFACE(0, alt 0)
2. 236 OUT `b8 22 00 00 00 c0 00 00 50 ca`
3. 193 OUT wV=0xc039; 193 OUT wV=0x4134; 193 IN wV=0x0039 len 1 (→ 01)
4. 192 OUT wV=0x5066 `ab 03 12 34 57`; 192 IN wV=0x5066 len 3 (×2, ~100 ms apart)
5. 192 OUT wV=0x5066 `ab 03 12 34 58`; 192 IN wV=0x5066 len 3 (×2)
6. 194 OUT; 199 OUT wV=0x0064; 198 OUT wI=0x0100 (×2)
7. I2C 0x64: reg 0x13 ← `80 80 80 80`; reg 0x3a ← 00; reg 0x3b ← 80
8. poll status (0x64 reg 0, 32 B) until geometry non-zero
9. 198 OUT wV=0x0032 wI=0x0101 `00 40 ff 32`

## Start / stop streaming
Start: [198 wI=0x0100] reg 0x12←0; [198] reg 0x3a←0; reg 0x3b←0x80; [198] reg 0x10←1; SET_INTERFACE(0, alt 2)
Stop:  SET_INTERFACE(0, alt 0); [198] reg 0x10←0

## Stream format (EP 0x83)
Continuous byte stream (ISO packets carry no headers of their own) of lines:
```
[audio block: ff 00 ff 02 + 8 bytes = 2 stereo s16le samples]   (on ~1 in 3 lines)
ff 00 00 XY                                                      (BT.656-style SAV)
<width*2 bytes YUYV 4:2:2, BT.709 limited range>                 (only when XY=0x80)
```
- XY = 0x80: active line. XY = 0xab: vertical-blanking line (followed by zeros).
- A frame = the run of 0x80 lines between 0xab runs (~45 blanking lines @1080p).
- 1080p60: 1080 active lines, 4,329,300 bytes per frame period incl. blanking/audio.
- Audio ≈ 800 samples/frame at 60 fps → 48 kHz stereo 16-bit (unconfirmed by ear).

## Interlaced input (expected, not yet verified on hardware)
The line codes follow BT.656 (`XY = 1 F V H P3 P2 P1 P0`), so 1080i should arrive as
two fields: field 1 with `80` (active) / `ab` (blanking), field 2 with `c7` / `ec`.
The driver weaves field 1 into even rows and field 2 into odd rows. Captures from a
real 1080i source would confirm this, including what the status block reports as
height (540 or 1080).
