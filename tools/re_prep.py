"""Prepare the game binary for disassembly.
Usage: python tools/re_prep.py [path to mgs_ngi]   (default: game/mgs_ngi as installed by setup, else extracted_mgs/mgs_ngi)
Writes tools/out/code.bin (code+data, loaded at 0x4A000000), tools/out/calls.json
(import names + every import call site) and, if llvm-objcopy is on PATH, tools/out/code.elf."""
import struct, sys, json, os, shutil, subprocess
here = os.path.dirname(os.path.abspath(__file__)); out = os.path.join(here, 'out'); os.makedirs(out, exist_ok=True)
default = next((p for p in (os.path.join(here, '..', 'game', 'mgs_ngi'), os.path.join(here, '..', 'extracted_mgs', 'mgs_ngi')) if os.path.exists(p)), None)
d = open(sys.argv[1] if len(sys.argv) > 1 else default, 'rb').read()
(ident, ver, flags, arch, fxo, fxs, co, cfs, cms, so, ss, eo, cfo, cfsz, base, xo, xs) = struct.unpack_from('<IIHHIIIIIIIIIIIII', d, 0)
seg = d[fxo:fxo + fxs]; p = seg.find(b's3eDebugErrorShow'); imps = []
while True:
    e = seg.index(b'\0', p); s = seg[p:e].decode('latin1')
    if not s or not s.startswith(('s3e', 'gl', 'egl', '_')): break
    imps.append(s); p = e + 1
code = bytes(d[co:co + cfs]) + bytes(cms - cfs); calls = []
for sig, kind in ((bytes([2, 0, 0, 0, 0x24, 3, 0, 0]), 'arm'), (bytes([3, 0, 0, 0, 0xd2, 0x12, 0, 0]), 'thumb')):
    k = d.find(sig, fxo, fxo + fxs); cnt = struct.unpack_from('<I', d, k + 8)[0]; r = k + 12
    for _ in range(cnt):
        rec = d[r:r + 6]; calls.append(((rec[0] << 16) | (rec[3] << 8) | rec[2], kind, imps[rec[4] | (rec[5] << 8)])); r += 6
open(os.path.join(out, 'code.bin'), 'wb').write(code)
json.dump({'base': base, 'imports': imps, 'calls': calls}, open(os.path.join(out, 'calls.json'), 'w'))
print(f'{len(imps)} imports, {len(calls)} call sites, base {base:#x}')
oc = shutil.which('llvm-objcopy')
if oc: subprocess.run([oc, '-I', 'binary', '-O', 'elf32-littlearm', os.path.join(out, 'code.bin'), os.path.join(out, 'code.elf')], check=True)
