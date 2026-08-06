#!/usr/bin/env python3
"""Render itch-replay text frames into an animated GIF for the README.

Usage: .venv/bin/python scripts/render_gif.py data/frames docs/assets/book.gif
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT_CANDIDATES = ["/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Monaco.ttf"]
BG = (13, 17, 23)
COLORS = {"A": (248, 113, 113), "B": (74, 222, 128), "-": (250, 204, 21),
          "l": (103, 232, 249)}  # asks red, bids green, spread yellow, tape cyan
DEFAULT = (230, 237, 243)


def main():
    frames_dir, out = Path(sys.argv[1]), Path(sys.argv[2])
    files = sorted(frames_dir.glob("frame_*.txt"))
    if not files:
        sys.exit(f"no frames in {frames_dir}")
    font = None
    for f in FONT_CANDIDATES:
        try:
            font = ImageFont.truetype(f, 15)
            break
        except OSError:
            continue
    if font is None:
        font = ImageFont.load_default()
    texts = [f.read_text().rstrip("\n").split("\n") for f in files]
    ncols = max(len(l) for t in texts for l in t)
    nrows = max(len(t) for t in texts)
    cw = font.getbbox("M")[2]
    ch = 19
    size = (cw * ncols + 24, ch * nrows + 24)
    imgs = []
    for t in texts:
        img = Image.new("RGB", size, BG)
        d = ImageDraw.Draw(img)
        for i, line in enumerate(t):
            d.text((12, 12 + i * ch), line, font=font,
                   fill=COLORS.get(line[:1], DEFAULT))
        imgs.append(img.quantize(colors=64))
    out.parent.mkdir(parents=True, exist_ok=True)
    imgs[0].save(out, save_all=True, append_images=imgs[1:], duration=80, loop=0,
                 optimize=True)
    print(f"wrote {out} ({out.stat().st_size / 1e6:.1f} MB, {len(imgs)} frames)")


if __name__ == "__main__":
    main()
