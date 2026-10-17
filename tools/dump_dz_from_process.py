"""Find decoded Derbh archives in a running process (e.g. EKA2L1 running the game) and save them.

    python tools/dump_dz_from_process.py <pid> <out_dir> [size]

Read-only: scans committed, readable memory for the DTRZ header of encrypted.dz (38 files, 3 dirs)
and writes <size> bytes (default 88212, the archive's plaintext length) from each hit.
"""
import ctypes, ctypes.wintypes as wt, os, sys, hashlib

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
READABLE = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}   # PAGE_READONLY/READWRITE/WRITECOPY/EXECUTE_READ/_READWRITE/_WRITECOPY

class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', wt.DWORD),
                ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_size_t), ('State', wt.DWORD),
                ('Protect', wt.DWORD), ('Type', wt.DWORD)]

def main():
    pid, out = int(sys.argv[1]), sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 88212
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
    k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h: raise SystemExit('OpenProcess failed: %d' % ctypes.get_last_error())
    os.makedirs(out, exist_ok=True)
    magic = b'DTRZ\x26\x00\x03\x00'
    addr, mbi, seen, scanned = 0, MBI(), set(), 0
    while k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base, rsize = mbi.BaseAddress or 0, mbi.RegionSize
        if mbi.State == MEM_COMMIT and (mbi.Protect & 0xff) in READABLE and not (mbi.Protect & 0x100):
            buf = ctypes.create_string_buffer(rsize); got = ctypes.c_size_t()
            if k32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, rsize, ctypes.byref(got)):
                data = buf.raw[:got.value]; scanned += len(data)
                i = data.find(magic)
                while i != -1:
                    blob = data[i:i + size]
                    if len(blob) < size:   # crosses the region end: read directly
                        b2 = ctypes.create_string_buffer(size); g2 = ctypes.c_size_t()
                        k32.ReadProcessMemory(h, ctypes.c_void_p(base + i), b2, size, ctypes.byref(g2))
                        blob = b2.raw[:g2.value]
                    md5 = hashlib.md5(blob).hexdigest()
                    names = blob[9:0x30c].split(b'\0')[:8]
                    print('hit at %#x  md5=%s  names=%s' % (base + i, md5, names))
                    if md5 not in seen:
                        seen.add(md5)
                        open(os.path.join(out, 'dz_%x.bin' % (base + i)), 'wb').write(blob)
                    i = data.find(magic, i + 1)
        addr = base + rsize
        if addr >= 0x7FFFFFFFFFFF: break
    print('scanned %.1f MB, %d distinct archive copies saved' % (scanned / 1e6, len(seen)))

if __name__ == '__main__':
    main()
