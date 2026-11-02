#!/usr/bin/env python3
"""Compare screenshots of this emulator with MAME snapshots, pixel by pixel.

  python3 compare_frames.py mine_dir_or_pattern mame_snapshot_dir frame1 frame2 ...

Own screenshots are expected as <mine>/<frame>.png, MAME's as 0000.png, 0001.png ... in
the order of the frames listed (see mame_shot.lua). Requires Pillow.
"""
import sys
from PIL import Image, ImageChops

mine, mame, frames = sys.argv[1], sys.argv[2], sys.argv[3:]
for i, f in enumerate(frames):
    a = Image.open(f"{mine}/{f}.png").convert("RGB")
    b = Image.open(f"{mame}/{i:04d}.png").convert("RGB")
    d = ImageChops.difference(a, b)
    n = sum(1 for p in d.getdata() if p != (0, 0, 0))
    print(f"frame {f}: {'IDENTICAL' if n == 0 else f'{n} pixels differ, bbox {d.getbbox()}'}")
