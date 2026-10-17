"""Builds the Windows icon from icon.png (repo root): mgs_native/mgs_mobile.ico, all the sizes Windows asks for
(16-48 for title bars / taskbar / Explorer lists, 256 for large Explorer views). Called by build.bat.

    python tools/make_icon.py [source.png] [out.ico]
"""
import os
import sys
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'icon.png')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, 'mgs_native', 'mgs_mobile.ico')
    if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(src):
        return   # up to date
    img = Image.open(src).convert('RGBA')
    # Pillow resamples from the source for each size (LANCZOS) when writing an .ico
    img.save(out, format='ICO', sizes=[(s, s) for s in SIZES])
    print('Wrote %s (%s)' % (out, ', '.join(str(s) for s in SIZES)))


if __name__ == '__main__':
    main()
