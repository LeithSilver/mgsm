"""Disassemble a guest address range with import calls and literal-pool strings annotated.
Usage: python tools/disasm.py 0x4A0406F8 0x4A040760 [thumb|arm]
Uses capstone if installed, else llvm-objdump on tools/out/code.elf. Run re_prep.py first."""
import sys, json, os, re, struct, subprocess
here = os.path.dirname(os.path.abspath(__file__)); out = os.path.join(here, 'out')
c = json.load(open(os.path.join(out, 'calls.json'))); B = c['base']; calls = {o: n for o, k, n in c['calls']}
code = open(os.path.join(out, 'code.bin'), 'rb').read()
start, end = int(sys.argv[1], 16), int(sys.argv[2], 16); mode = sys.argv[3] if len(sys.argv) > 3 else 'thumb'
def note(a, text):
    extra = ''; la = None
    m1 = re.search(r'@ 0x([0-9a-f]+)', text); m2 = re.search(r'\[pc, #(0x[0-9a-f]+|\d+)\]', text)
    if 'ldr' in text and m1: la = int(m1.group(1), 16); la = la - B if la >= B else la
    elif 'ldr' in text and m2: la = (((a + 4) & ~3) if mode == 'thumb' else a + 8) + int(m2.group(1), 0) - B
    if la is not None:
        if 0 <= la <= len(code) - 4:
            v = struct.unpack_from('<I', code, la)[0]; extra = f'  ; ={v:#x}'
            if B <= v < B + len(code):
                s = code[v - B:v - B + 80].split(b'\0')[0]
                if len(s) > 3 and all(32 <= ch < 127 for ch in s): extra += f' "{s.decode()}"'
    if a - B in calls: extra += '   <== ' + calls[a - B]
    return extra
try:
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if mode == 'thumb' else capstone.CS_MODE_ARM)
    md.skipdata = True
    for i in md.disasm(code[start - B:end - B], start):
        t = f'{i.mnemonic}\t{i.op_str}'; print(f'{i.address:08x}: {t}{note(i.address, t)}')
except ImportError:
    r = subprocess.run(['llvm-objdump', '-D', '--triple=' + ('thumbv5te' if mode == 'thumb' else 'armv5te'),
                        f'--start-address={start - B:#x}', f'--stop-address={end - B:#x}', os.path.join(out, 'code.elf')],
                       capture_output=True, text=True).stdout
    for l in r.splitlines():
        m = re.match(r'\s*([0-9a-f]+):(.*)', l)
        if not m: continue
        a = B + int(m.group(1), 16); t = re.sub(r'<_binary_\w+_start\+0x([0-9a-f]+)>', lambda x: f'<{B + int(x.group(1), 16):x}>', m.group(2))
        print(f'{a:08x}:{t}{note(a, t)}')
