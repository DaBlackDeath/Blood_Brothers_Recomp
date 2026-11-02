#!/usr/bin/env python3
"""compare_render_states.py LIST_FILE MINE_DIR MAME_DIR
Compares bloodbro's rendering of saved states (MINE_DIR/<name>.png) with MAME's snapshots (MAME_DIR/0000.png ...,
in the order of LIST_FILE). Prints how many states are pixel-identical. Requires Pillow."""
import sys
from PIL import Image, ImageChops

lst, mine, mame = sys.argv[1:4]
names = [l.strip() for l in open(lst) if l.strip()]
bad = []
for i, n in enumerate(names):
    a = Image.open(f"{mine}/{n[:-4]}.png").convert("RGB")
    b = Image.open(f"{mame}/{i:04d}.png").convert("RGB")
    d = ImageChops.difference(a, b)
    cnt = sum(1 for p in d.getdata() if p != (0, 0, 0))
    if cnt:
        bad.append((n, cnt, d.getbbox()))
print(f"{len(names) - len(bad)} of {len(names)} states pixel-identical to MAME")
for n, cnt, bb in bad:
    print(f"  {n}: {cnt} pixels differ, region {bb}")
sys.exit(1 if bad else 0)
