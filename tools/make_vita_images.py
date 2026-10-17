"""PS Vita bubble and LiveArea images (README.md §6.29): vita/*.jpg|png -> mgs_native/vita/sce_sys/ as 8-bit
palette PNGs (the Vita's installer wants indexed PNGs; full-colour ones can fail or show blank).

  vita/icon0.jpg        128x128  sce_sys/icon0.png                    the home screen bubble
  vita/pic0.jpg         960x544  sce_sys/pic0.png                     shown while the app starts
  vita/liveareabg.jpg   840x500  sce_sys/livearea/contents/bg.png     LiveArea background
  vita/startup.png      280x158  sce_sys/livearea/contents/startup.png  LiveArea Start button

Run after changing an image; the outputs are checked in, so a build needs no Pillow.
"""
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "vita")
OUT = os.path.join(ROOT, "mgs_native", "vita", "sce_sys")
IMAGES = [
    ("icon0.jpg", (128, 128), "icon0.png"),
    ("pic0.jpg", (960, 544), "pic0.png"),
    ("liveareabg.jpg", (840, 500), "livearea/contents/bg.png"),
    ("startup.png", (280, 158), "livearea/contents/startup.png"),
]


def main():
    for src, size, dst in IMAGES:
        im = Image.open(os.path.join(SRC, src)).convert("RGB")
        if im.size != size:
            sys.exit(f"{src}: {im.size[0]}x{im.size[1]}, expected {size[0]}x{size[1]}")
        out = os.path.join(OUT, dst)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(out, optimize=True)
        print(f"{dst}: {size[0]}x{size[1]}, {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main()
