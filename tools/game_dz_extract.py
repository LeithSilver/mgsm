"""Extract a Derbh (.dz) archive with the game's own Derbh code, run in Unicorn.

    python tools/game_dz_extract.py <archive> <out_dir> [name ...]

Uses tools/out/code.bin + calls.json (run tools/re_prep.py first). Every import call site is
patched to an SVC that a Python handler services (s3eFile*, s3eMalloc*, ...). The archive is
mounted with CDerbhArchive (ctor 0x4A07D690, Attach 0x4A07D084), then entries are opened with
0x4A07D33C(archive, name, mode) and read with 0x4A07D470(archive, buf, size, count, handle):
the same calls the game's s3eFileAddUserFileSys callbacks (0x4A0D3E91 / 0x4A0D3ECD) make.
Without names, all entries listed in the archive's string table are extracted.
"""
import json, os, struct, sys
from unicorn import *
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = 0x4A000000
HEAP = 0x60000000; HEAP_SIZE = 0x04000000
STACK_TOP = 0x70100000
RET = 0x7F000000

class Game:
    def __init__(self):
        code = open(os.path.join(HERE, 'out', 'code.bin'), 'rb').read()
        calls = json.load(open(os.path.join(HERE, 'out', 'calls.json')))
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        uc.mem_map(BASE, 0x00400000)               # code + data + bss
        uc.mem_write(BASE, code)
        uc.mem_map(HEAP, HEAP_SIZE)
        uc.mem_map(STACK_TOP - 0x100000, 0x100000)
        uc.mem_map(RET, 0x1000)
        self.sites = {}
        for off, mode, name in calls['calls']:
            a = BASE + off
            if mode == 'thumb':
                uc.mem_write(a, struct.pack('<HH', 0xDF01, 0x46C0))   # svc #1; nop
                self.sites[a + 2] = (name, a + 4 + 1)
            else:
                uc.mem_write(a, struct.pack('<I', 0xEF000001))        # svc #1
                self.sites[a + 4] = (name, a + 4)
        # Derbh allocator hooks: the game registers alloc/free with 0x4A0D3E60 ([0x4A0EDB90+8/+0xC]);
        # without them it falls back to a heap that only exists in the running game.
        STUBS = 0x7E000000
        uc.mem_map(STUBS, 0x1000)
        uc.mem_write(STUBS, struct.pack('<HHHH', 0xDF02, 0x4770, 0xDF02, 0x4770))   # svc #2; bx lr  (x2)
        self.sites[STUBS + 2] = ('dzAlloc', None)
        self.sites[STUBS + 6] = ('dzFree', None)
        self.w32(0x4A0EDB90 + 8, (STUBS + 0) | 1)
        self.w32(0x4A0EDB90 + 12, (STUBS + 4) | 1)
        uc.hook_add(UC_HOOK_INTR, self.on_svc)
        self.heap = HEAP + 0x100
        self.files = {}; self.next_h = 1
        self.unhandled = {}

    def r32(self, a): return struct.unpack('<I', self.uc.mem_read(a, 4))[0]
    def w32(self, a, v): self.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def cstr(self, a):
        out = b''
        while True:
            c = bytes(self.uc.mem_read(a + len(out), 1))
            if c == b'\0': return out.decode('latin1')
            out += c
    def alloc(self, n):
        a = (self.heap + 15) & ~15; self.heap = a + max(n, 4) + 16
        self.uc.mem_write(a, b'\0' * max(n, 4)); return a
    def put_str(self, s):
        a = self.alloc(len(s) + 1); self.uc.mem_write(a, s.encode('latin1') + b'\0'); return a

    def on_svc(self, uc, intno, ud):
        pc = uc.reg_read(UC_ARM_REG_PC)
        site = self.sites.get(pc)
        if site is None:
            raise RuntimeError('svc at unknown site %x' % pc)
        name, ret = site
        r = [uc.reg_read(UC_ARM_REG_R0 + i) for i in range(4)]
        sp = uc.reg_read(UC_ARM_REG_SP)
        fn = getattr(self, 'i_' + name, None)
        if fn is None:
            self.unhandled[name] = self.unhandled.get(name, 0) + 1
            v = 0
        else:
            v = fn(r, sp)
        uc.reg_write(UC_ARM_REG_R0, (v or 0) & 0xffffffff)
        if ret is not None: uc.reg_write(UC_ARM_REG_PC, ret)   # None: stub continues with its own bx lr

    # --- imports ---
    def i_dzAlloc(self, r, sp): return self.alloc(r[0])
    def i_dzFree(self, r, sp): return 0
    def i_s3eMalloc(self, r, sp): return self.alloc(r[0])
    def i_s3eMallocBase(self, r, sp): return self.alloc(r[0])
    def i_s3eRealloc(self, r, sp):
        a = self.alloc(r[1])
        if r[0]: self.uc.mem_write(a, bytes(self.uc.mem_read(r[0], r[1])))
        return a
    i_s3eReallocBase = i_s3eRealloc
    def i_s3eFree(self, r, sp): return 0
    i_s3eFreeBase = i_s3eFree
    def i_s3eFileOpen(self, r, sp):
        name = self.cstr(r[0])
        path = self.open_map.get(name)
        if not path or not os.path.exists(path): return 0
        h = 0x1000 + self.next_h; self.next_h += 1
        self.files[h] = open(path, 'rb'); return h
    def i_s3eFileOpenFromMemory(self, r, sp):
        import io
        h = 0x1000 + self.next_h; self.next_h += 1
        self.files[h] = io.BytesIO(bytes(self.uc.mem_read(r[0], r[1]))); return h
    def i_s3eFileRead(self, r, sp):
        buf, size, count, h = r
        f = self.files.get(h)
        if f is None or size == 0: return 0
        data = f.read(size * count)
        self.uc.mem_write(buf, data)
        return len(data) // size
    def i_s3eFileSeek(self, r, sp):
        h, off, origin = r[0], struct.unpack('<i', struct.pack('<I', r[1]))[0], r[2]
        f = self.files.get(h)
        if f is None: return 1
        f.seek(off, origin if origin in (0, 1, 2) else 0); return 0   # S3E_FILESEEK_SET=0, CUR=1, END=2 (as vfs.cpp)
    def i_s3eFileTell(self, r, sp):
        f = self.files.get(r[0]); return f.tell() if f else -1
    def i_s3eFileGetSize(self, r, sp):
        f = self.files.get(r[0])
        if not f: return -1
        cur = f.tell(); f.seek(0, 2); n = f.tell(); f.seek(cur); return n
    def i_s3eFileClose(self, r, sp):
        f = self.files.pop(r[0], None)
        if f: f.close()
        return 0
    def i_s3eFileEOF(self, r, sp):
        f = self.files.get(r[0])
        if not f: return 1
        cur = f.tell(); f.seek(0, 2); n = f.tell(); f.seek(cur); return 1 if cur >= n else 0
    def i_s3eFileGetChar(self, r, sp):
        f = self.files.get(r[0]); c = f.read(1) if f else b''
        return c[0] if c else 0xFFFFFFFF   # EOF = -1
    def i_s3eFileCheckExists(self, r, sp): return 1 if self.cstr(r[0]) in self.open_map else 0

    def call(self, addr, args, limit=200_000_000):
        uc = self.uc
        sp = STACK_TOP - 0x100
        extra = args[4:]
        sp -= 4 * len(extra)
        for i, v in enumerate(extra): self.w32(sp + 4 * i, v)
        uc.reg_write(UC_ARM_REG_SP, sp)
        for i in range(4): uc.reg_write(UC_ARM_REG_R0 + i, args[i] if i < len(args) else 0)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        try:
            uc.emu_start(addr | 1, RET, count=limit)
        except UcError as e:
            regs = ' '.join('r%d=%x' % (i, uc.reg_read(UC_ARM_REG_R0 + i)) for i in range(13))
            raise RuntimeError('%s in call %x: pc=%x lr=%x sp=%x %s' % (e, addr, uc.reg_read(UC_ARM_REG_PC),
                               uc.reg_read(UC_ARM_REG_LR), uc.reg_read(UC_ARM_REG_SP), regs))
        return uc.reg_read(UC_ARM_REG_R0)

def entry_names(path):
    """File paths from the archive's string table + directory tree (DTRZ layout)."""
    d = open(path, 'rb').read()
    nf, nd = struct.unpack_from('<HH', d, 4); p = 9; strs = []
    for _ in range(nf + nd - 1):
        e = d.index(b'\0', p); strs.append(d[p:e].decode('latin1')); p = e + 1
    files, dirs = strs[:nf], [''] + strs[nf:]
    out = []
    for i in range(nf):
        ids = []
        while True:
            v = struct.unpack_from('<H', d, p)[0]; p += 2
            if v == 0xFFFF: break
            ids.append(v)
        dirn = dirs[ids[0]] if ids else ''
        out.append((dirn + '\\' if dirn else '') + files[i])
    return out

def main():
    arc, out_dir = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] or entry_names(arc)
    g = Game()
    g.open_map = {'archive.dz': arc}
    obj = g.alloc(0x44c)
    g.call(0x4A07D690, [obj])                                   # CDerbhArchive::CDerbhArchive
    if os.environ.get('DZ_MEM'):   # mount from memory, as the game does for encrypted.dz.ngdat (0x4A041D10)
        raw = open(arc, 'rb').read(); mem = g.alloc(len(raw)); g.uc.mem_write(mem, raw)
        rc = g.call(0x4A07D084, [obj, mem, 1, len(raw)])
    else:
        rc = g.call(0x4A07D084, [obj, g.put_str('archive.dz'), 0, 0])  # Attach(obj, name, is_mem=0, size=0)
    print('Attach ->', rc, ' unhandled imports:', g.unhandled)
    if rc != 0: return 1
    ok = 0
    for n in names:
        game_name = n.replace('\\', '/').lower()
        h = g.call(0x4A07D33C, [obj, g.put_str(game_name), g.put_str('rb')])
        if not h:
            print('  open failed:', n); continue
        CH = 0x10000
        buf = g.alloc(CH)
        data = b''
        while True:
            got = g.call(0x4A07D470, [obj, buf, 1, CH, h])
            if not got or got > CH: break
            data += bytes(g.uc.mem_read(buf, got))
            if got < CH: break
        size = got = len(data)
        g.call(0x4A07D32C, [obj, h])
        dst = os.path.join(out_dir, *n.split('\\'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(data)
        print('  %-55s size=%6d read=%6d  header=%s' % (n, size, got, data[:5]))
        ok += 1
    print('extracted %d/%d   unhandled imports: %s' % (ok, len(names), g.unhandled))

if __name__ == '__main__':
    sys.exit(main() or 0)
