#!/usr/bin/env python3
"""Produce side-by-side crops (256x256) of each encoder's reconstruction
for each test image, plus per-pixel error heatmap."""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

ROOT = Path("/home/romain/etc1_bench")
TEST = ROOT / "test_images"
DEC = ROOT / "decoded"
OUT = ROOT / "results" / "crops"
OUT.mkdir(parents=True, exist_ok=True)

# Crop region — pick something with detail. (x, y, w, h)
# 1920x1080 -> roughly center where detail typically lies.
CROP = (700, 400, 256, 256)

ENCODERS = ["etcpak", "etc2comp_e100", "rg_etc1_hq", "basisu_full", "etcpack_slow"]

LABEL_H = 24

for img in sorted(TEST.glob("*.png")):
    orig = Image.open(img).convert("RGB")
    panels = []
    labels = ["original"]
    panels.append(orig.crop((CROP[0], CROP[1], CROP[0]+CROP[2], CROP[1]+CROP[3])))
    for enc in ENCODERS:
        dec_png = DEC / f"{img.stem}__{enc}" / "decoded.png"
        if not dec_png.exists():
            continue
        dec = Image.open(dec_png).convert("RGB")
        panels.append(dec.crop((CROP[0], CROP[1], CROP[0]+CROP[2], CROP[1]+CROP[3])))
        labels.append(enc)

    # Make composite: panels in a row, with label band above each.
    n = len(panels)
    W = CROP[2]
    H = CROP[3]
    comp = Image.new("RGB", (W*n, H + LABEL_H), (32, 32, 32))
    draw = ImageDraw.Draw(comp)
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 14)
    except Exception:
        font = ImageFont.load_default()
    for i, (p, lbl) in enumerate(zip(panels, labels)):
        comp.paste(p, (i*W, LABEL_H))
        draw.text((i*W + 4, 4), lbl, fill=(255, 255, 255), font=font)

    out_path = OUT / f"{img.stem}_crops.png"
    comp.save(out_path)
    print(f"wrote {out_path}")

    # Diff heatmaps (abs error scaled x4)
    diff_imgs = [Image.new("RGB", (W, H), (0, 0, 0))]  # placeholder for original
    a = np.asarray(orig.crop((CROP[0], CROP[1], CROP[0]+CROP[2], CROP[1]+CROP[3])), dtype=np.int16)
    for enc in ENCODERS:
        dec_png = DEC / f"{img.stem}__{enc}" / "decoded.png"
        if not dec_png.exists(): continue
        b = np.asarray(Image.open(dec_png).convert("RGB").crop((CROP[0], CROP[1], CROP[0]+CROP[2], CROP[1]+CROP[3])), dtype=np.int16)
        d = np.clip(np.abs(a - b) * 4, 0, 255).astype(np.uint8)
        diff_imgs.append(Image.fromarray(d, mode="RGB"))
    comp2 = Image.new("RGB", (W*len(diff_imgs), H + LABEL_H), (32, 32, 32))
    draw2 = ImageDraw.Draw(comp2)
    for i, (p, lbl) in enumerate(zip(diff_imgs, labels)):
        comp2.paste(p, (i*W, LABEL_H))
        draw2.text((i*W + 4, 4), f"|err|x4 {lbl}", fill=(255, 255, 255), font=font)
    comp2.save(OUT / f"{img.stem}_errors.png")
