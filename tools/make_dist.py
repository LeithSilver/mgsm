"""Builds the distributable copy of the native port in output/ (run from the repo root after a successful build).

    python tools/make_dist.py [output_dir]

Contents: mgs_mobile.exe, unicorn.dll, SDL3.dll, the VC++ runtime DLLs (app-local), README.txt and an empty ROM\\
folder. No game data is shipped: on first run the exe's setup window installs it from the player's own
Metal.Gear.Solid.n-gage (dropped on the window, browsed for, or found in ROM\\) into game\\ and extracts the
textures to textures\\ (mgs_native/installer.cpp, README.md section 6.19).
Not copied: mgs_mobile.ini (each install starts with defaults, so debug mode starts off: without an ini the exe
defaults it on only in the repo layout).
"""
import getpass
import glob
import os
import shutil
import struct
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VS_REDIST = r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC'
CRT_DLLS = ['msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll']

README = """Metal Gear Solid Mobile - native PC port
=========================================

First run
  You need your own copy of the N-Gage 2.0 game package, Metal.Gear.Solid.n-gage.
  Run mgs_mobile.exe and drop the file onto the setup window, click Browse to pick it,
  or put it in the ROM folder first and click "Install from ROM...". Setup takes a few
  seconds: the game files go to the game folder and the textures to the textures folder.
  Your .n-gage file isn't changed and can be removed afterwards.

Keyboard (default; remap in the F1 menu)
  Arrow keys          Move
  Z                   A  (action / OK)
  C                   B  (first-person view)
  A                   Left soft key  (pause menu)
  X / Esc / K / F2    Right soft key  (equipment)
  Enter / Space / J   Centre / OK
  Num *               Camera
  Tab / L             #
  Backspace           Clear
  0-9 / numpad        Number keys
  Page Up / Down      Volume

Controller (default; remap in the F1 menu)
  D-pad / left stick  Move
  A / Cross           A
  Y / Triangle        B
  B / Circle, RB / R1 Right soft key  (equipment)
  Start, X / Square, LB / L1   Left soft key  (pause menu)
  Back / Select, R3   Settings menu (in the menu: left stick / d-pad move the cursor,
                      A / R1 / R2 click, right stick scrolls)

Other keys
  F1                  Settings menu (display, anti-aliasing, FOV, soft-key labels, custom textures,
                      key and controller remapping, pause on focus loss). The game pauses while it is open.
  F11 / Alt+Enter     Fullscreen
  F9                  Texture filtering on/off
  F12                 Screenshot (saved in the screenshots folder next to the exe)

Custom textures
  The textures folder holds every texture of the game as PNG. Edit one in place, or put a PNG
  with the same name anywhere in that folder; any size works. Changes load when the game starts.
  See textures\\README.txt.

Files
  mgs_mobile.ini      Settings (created on first run, next to the exe)
  game\\data\\savedata*.i3d   Save games
  game\\               Installed game files (delete it to run setup again)

mgs_mobile.log (next to the exe, rewritten each run) is the log. If something goes wrong, include it in your report.
"""

ROM_README = """Optional: put Metal.Gear.Solid.n-gage in this folder before the first run, and the setup
window offers to install from it directly. You can also drop the file onto the setup window.
"""


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'output'))
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, 'ROM'))

    for f in ('mgs_mobile.exe', 'unicorn.dll', 'SDL3.dll'):
        shutil.copy2(os.path.join(REPO, f), out)

    # VC++ runtime, app-local (newest redist; it is backward compatible with older toolsets)
    crt_dirs = sorted(glob.glob(os.path.join(VS_REDIST, '14.*', 'x64', 'Microsoft.VC14*.CRT')))
    if not crt_dirs:
        sys.exit('VC++ redist not found under ' + VS_REDIST)
    for f in CRT_DLLS:
        shutil.copy2(os.path.join(crt_dirs[-1], f), out)

    # Release build: the same exe marked as a Windows (GUI) program, so no console window opens; main.cpp then
    # writes the log to mgs_mobile.log. The PE optional header's Subsystem field: 2 = GUI, 3 = console.
    exe = os.path.join(out, 'mgs_mobile.exe')
    with open(exe, 'r+b') as f:
        pe = struct.unpack('<I', f.read(0x40)[0x3C:0x40])[0]
        f.seek(pe + 0x18 + 0x44)
        assert struct.unpack('<H', f.read(2))[0] in (2, 3)
        f.seek(pe + 0x18 + 0x44)
        f.write(struct.pack('<H', 2))

    shutil.copytree(os.path.join(REPO, 'licenses'), os.path.join(out, 'licenses'))

    with open(os.path.join(out, 'README.txt'), 'w', newline='\r\n') as f:
        f.write(README)
    with open(os.path.join(out, 'ROM', 'README.txt'), 'w', newline='\r\n') as f:
        f.write(ROM_README)

    # Nothing from the game may ship: players install it from their own copy. Players also start with debug
    # mode off, which the exe picks from the layout only while no ini says otherwise.
    shipped = [os.path.relpath(os.path.join(d, n), out) for d, _, ns in os.walk(out) for n in ns]
    game_like = [p for p in shipped if p.lower().split(os.sep)[0] in ('game', 'textures')
                 or os.path.splitext(p)[1].lower() in ('.dz', '.luc', '.bin', '.n-gage', '.s3e', '.png', '.ini')]
    assert not game_like, game_like

    shipped = sorted(os.path.relpath(os.path.join(d, n), out) for d, _, ns in os.walk(out) for n in ns)

    # No traces of the build machine: its user name, home folder, repo path or computer name must not appear in
    # any shipped file (ASCII or UTF-16).
    needles = {getpass.getuser(), os.path.expanduser('~'), REPO, os.environ.get('COMPUTERNAME', '')}
    needles = [n.lower() for n in needles if n and len(n) >= 4]
    for rel in shipped:
        data = open(os.path.join(out, rel), 'rb').read().lower()
        for n in needles:
            for enc in ('utf-8', 'utf-16-le'):
                assert n.encode(enc) not in data, 'build machine detail %r in %s' % (n, rel)

    # The release zip: fixed timestamps and plain attributes, so it carries no build time or file ownership.
    zip_path = os.path.join(os.path.dirname(out), 'MGS_Mobile_PC.zip')
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in shipped:
            info = zipfile.ZipInfo('MGS_Mobile_PC/' + rel.replace(os.sep, '/'), date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            info.create_system = 0
            with open(os.path.join(out, rel), 'rb') as f:
                z.writestr(info, f.read())
        z.writestr(zipfile.ZipInfo('MGS_Mobile_PC/ROM/', date_time=(2020, 1, 1, 0, 0, 0)), b'')

    total = sum(os.path.getsize(os.path.join(out, p)) for p in shipped)
    print('Wrote %s (%d files, %.1f MB); CRT from %s' % (out, len(shipped), total / 1e6, crt_dirs[-1]))
    print('Wrote %s (%.1f MB)' % (zip_path, os.path.getsize(zip_path) / 1e6))


if __name__ == '__main__':
    main()
