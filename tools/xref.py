"""Find Thumb BL/BLX call sites that target an address.  Usage: python tools/xref.py 0x4A0406F8"""
import struct, sys, os, json
here = os.path.dirname(os.path.abspath(__file__)); out = os.path.join(here, 'out')
B = json.load(open(os.path.join(out, 'calls.json')))['base']; code = open(os.path.join(out, 'code.bin'), 'rb').read()
tgt = int(sys.argv[1], 16)
for o in range(0, len(code) - 4, 2):
    h1, h2 = struct.unpack_from('<HH', code, o)
    if (h1 & 0xF800) == 0xF000 and (h2 & 0xE800) == 0xE800:
        off = ((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
        if off & 0x400000: off -= 0x800000
        dest = B + o + 4 + off
        if (h2 & 0xF800) == 0xE800: dest &= ~3
        if dest == tgt: print(f'{B + o:08x}')
