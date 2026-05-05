#!/usr/bin/env python3
"""Parse the per-encoder bench logs and produce a unified results table."""
import re, sys, json
from pathlib import Path

LOGS = {
    "etcpak":            "/tmp/etcpak.log",
    "rg_etc1_hq":        "/tmp/rg.log",
    "basisu_full":       "/tmp/basisu.log",
    "basisu_full_perc":  "/tmp/basisu_perc.log",
    "etc2comp_e100":     "/tmp/etc2c.log",
    "v3_corners":        "/tmp/v3rgb.log",
    "v3_corners_perc":   "/tmp/v3perc.log",
    "v4_default":        "/tmp/v4d.log",
    "v4_dither":         "/tmp/v4dith.log",
    "v4_ycocg":          "/tmp/v4yc.log",
    "v4_ycocg_dither":   "/tmp/v4ycd.log",
    "v4_no_adaptive":    "/tmp/v4na.log",
}
# also include the original etcpak run output
EXTRA = "/home/romain/etc1_bench/results/results.json"

PAT_TAG = re.compile(r"^\[\+\] (\S+)__(\S+)$")
PAT_METRIC = re.compile(r"PSNR_RGB=(\S+)\s+PSNR_Y=(\S+)\s+SSIM_Y=(\S+)\s+t=(\S+)s")

rows = []

# Recover etcpak rows from JSON file (it was the first run)
import json
try:
    j = json.loads(Path(EXTRA).read_text())
    for r in j:
        rows.append(r)
except Exception:
    pass

for enc, path in LOGS.items():
    if not Path(path).exists(): continue
    cur_image = None
    cur_enc = None
    matched_for_cur = False
    for line in Path(path).read_text().splitlines():
        m = PAT_TAG.match(line)
        if m:
            cur_image = m.group(1) + ".png"
            cur_enc = m.group(2)
            matched_for_cur = False
            continue
        if matched_for_cur:
            continue
        m = PAT_METRIC.search(line)
        if m and cur_enc:
            rows.append({
                "image": cur_image,
                "encoder": cur_enc,
                "psnr_rgb": float(m.group(1)),
                "psnr_y": float(m.group(2)),
                "ssim_y": float(m.group(3)),
                "encode_seconds": float(m.group(4).rstrip('s')),
            })
            matched_for_cur = True

# Dedupe: keep last occurrence per (image, encoder)
seen = {}
for r in rows:
    seen[(r["image"], r["encoder"])] = r

merged = list(seen.values())

# Print table by image
images = sorted({r["image"] for r in merged})
encoders = sorted({r["encoder"] for r in merged})
print(f"{'image':40s} {'encoder':18s} {'PSNR_RGB':>9s} {'PSNR_Y':>9s} {'SSIM_Y':>8s} {'t_enc(s)':>10s}")
for img in images:
    for enc in encoders:
        r = next((r for r in merged if r["image"] == img and r["encoder"] == enc), None)
        if r:
            print(f"{img:40s} {enc:18s} {r['psnr_rgb']:9.3f} {r['psnr_y']:9.3f} {r['ssim_y']:8.4f} {r['encode_seconds']:10.2f}")

print()
print("=== Mean across all images ===")
print(f"{'encoder':18s} {'PSNR_RGB':>9s} {'PSNR_Y':>9s} {'SSIM_Y':>8s} {'t_enc(s)':>10s} {'n':>4s}")
for enc in encoders:
    rows_e = [r for r in merged if r["encoder"] == enc]
    if not rows_e: continue
    avg = lambda k: sum(r[k] for r in rows_e) / len(rows_e)
    print(f"{enc:18s} {avg('psnr_rgb'):9.3f} {avg('psnr_y'):9.3f} {avg('ssim_y'):8.4f} {avg('encode_seconds'):10.2f} {len(rows_e):4d}")

Path("/home/romain/etc1_bench/results/merged.json").write_text(json.dumps(merged, indent=2))
