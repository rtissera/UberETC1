#!/usr/bin/env python3
"""A/B visual comparison for v9a floor sweep.

Layout per image (6 images):
  results/visual_floor/<image>_strip_1x.png
      Full-res 7-panel strip (each panel 1920x1080):
        original | v6_v2 | v8b_worst50 | v9a_floor05 | v9a_floor15 | v9a_floor25 | etcpak
  results/visual_floor/<image>_2x.png   3 regions x 8 panels at 2x  (480x270 native)
  results/visual_floor/<image>_4x.png   3 regions x 8 panels at 4x  (240x135 native)
  results/visual_floor/<image>_8x.png   3 regions x 8 panels at 8x  (120x67  native)
  results/visual_floor/<image>_errors_y.png   6 panels: encoder Y-error heatmaps
  results/visual_floor/<image>_errors_rgb.png 6 panels: max-channel RGB-error heatmaps

Region picker: high-Laplacian (detail), low-Laplacian (smooth),
median-Laplacian-with-high-variance (edge/text). Non-overlapping 480x270.

Diff-vs-orig column in 2x/4x/8x grids tracks the "new SOTA candidate"
(default = v9a_floor15; CLI override --diff-enc=...).

Nearest-neighbor zoom (preserves block artifacts).
"""
import os, sys, json, argparse
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from skimage.metrics import structural_similarity as ssim_fn
import matplotlib
matplotlib.use("Agg")
from matplotlib import cm

ROOT = Path("/home/romain/etc1_bench")
TEST = ROOT / "test_images"
DEC = ROOT / "decoded"
OUT = ROOT / "results" / "visual_floor"
OUT.mkdir(parents=True, exist_ok=True)

# Strip: 6 encoders + original.
ENCODERS_STRIP = [
    ("v6_idea2_v2",   "v6_v2"),
    ("v8b_worst50",   "worst50"),
    ("v9a_floor05",   "floor05"),
    ("v9a_floor15",   "floor15"),
    ("v9a_floor25",   "floor25"),
    ("etcpak",        "etcpak"),
]
# Crops: same 6 encoders + diff column for the new candidate.
ENCODERS_CROP = ENCODERS_STRIP[:]
# Errors: same 6 encoders.
ENCODERS_ERR = ENCODERS_STRIP[:]

LABEL_H = 36
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

def font(sz):
    try: return ImageFont.truetype(FONT_PATH, sz)
    except Exception: return ImageFont.load_default()

def load_rgb(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.uint8)

def y_channel(rgb):
    r, g, b = rgb[..., 0].astype(np.float32), rgb[..., 1].astype(np.float32), rgb[..., 2].astype(np.float32)
    return 0.299 * r + 0.587 * g + 0.114 * b

def psnr(a, b, peak=255.0):
    a = a.astype(np.float64); b = b.astype(np.float64)
    mse = float(np.mean((a - b) ** 2))
    if mse <= 0: return float("inf")
    return 10.0 * np.log10(peak * peak / mse)

def metrics(orig_rgb, dec_rgb):
    yo = y_channel(orig_rgb)
    yd = y_channel(dec_rgb)
    py = psnr(yo, yd)
    prgb = psnr(orig_rgb, dec_rgb)
    s = ssim_fn(yo, yd, data_range=255.0)
    return py, prgb, s

def upsample_nn(arr_rgb, factor):
    return np.repeat(np.repeat(arr_rgb, factor, axis=0), factor, axis=1)

def compose_row(panels_rgb, labels, label_h=LABEL_H, fsz=20):
    h, w = panels_rgb[0].shape[:2]
    n = len(panels_rgb)
    img = Image.new("RGB", (w * n, h + label_h), (24, 24, 28))
    d = ImageDraw.Draw(img)
    f = font(fsz)
    for i, (p, lbl) in enumerate(zip(panels_rgb, labels)):
        img.paste(Image.fromarray(p), (i * w, label_h))
        d.text((i * w + 6, 4), lbl, fill=(255, 255, 255), font=f)
    return img

def compose_grid(rows_panels, rows_labels, label_h=LABEL_H, fsz=18):
    h, w = rows_panels[0][0].shape[:2]
    n_cols = len(rows_panels[0])
    n_rows = len(rows_panels)
    img = Image.new("RGB", (w * n_cols, (h + label_h) * n_rows), (24, 24, 28))
    d = ImageDraw.Draw(img)
    f = font(fsz)
    for r, (panels, labels) in enumerate(zip(rows_panels, rows_labels)):
        y0 = r * (h + label_h)
        for c, (p, lbl) in enumerate(zip(panels, labels)):
            img.paste(Image.fromarray(p), (c * w, y0 + label_h))
            d.text((c * w + 6, y0 + 4), lbl, fill=(255, 255, 255), font=f)
    return img

def laplacian_abs(y):
    from scipy.signal import convolve2d
    k = np.array([[0, -1, 0], [-1, 4, -1], [0, -1, 0]], dtype=np.float32)
    return np.abs(convolve2d(y.astype(np.float32), k, mode="same", boundary="symm"))

def pick_regions(orig_rgb, win_w=480, win_h=270, stride=120):
    H, W, _ = orig_rgb.shape
    y_full = y_channel(orig_rgb)
    lap = laplacian_abs(y_full)
    candidates = []
    for y in range(0, H - win_h + 1, stride):
        for x in range(0, W - win_w + 1, stride):
            patch_lap = lap[y:y+win_h, x:x+win_w]
            patch_y = y_full[y:y+win_h, x:x+win_w]
            candidates.append((x, y, float(patch_lap.mean()), float(patch_y.var())))
    if not candidates:
        return {"detail": (0,0), "smooth": (win_w, 0), "edge": (0, win_h)}
    by_lap = sorted(candidates, key=lambda c: c[2])
    smooth = by_lap[0]
    detail = by_lap[-1]
    hi_var = sorted(candidates, key=lambda c: c[3], reverse=True)[: max(1, len(candidates)//4)]
    hi_var_sorted_by_lap = sorted(hi_var, key=lambda c: c[2])
    edge_candidate = hi_var_sorted_by_lap[len(hi_var_sorted_by_lap)//2]
    picks = {"detail": detail, "smooth": smooth, "edge": edge_candidate}
    def overlaps(a, b):
        ax, ay = a[0], a[1]; bx, by = b[0], b[1]
        return not (ax + win_w <= bx or bx + win_w <= ax or ay + win_h <= by or by + win_h <= ay)
    for key in ["edge"]:
        cur = picks[key]
        others = [v for k, v in picks.items() if k != key]
        if any(overlaps(cur, o) for o in others):
            for cand in hi_var_sorted_by_lap:
                if not any(overlaps(cand, o) for o in others):
                    picks[key] = cand
                    break
    return {k: (v[0], v[1]) for k, v in picks.items()}

def crop(arr, x, y, w, h):
    return arr[y:y+h, x:x+w].copy()

def diff_amplified(a_rgb, b_rgb, amp=8):
    d = np.clip(np.abs(a_rgb.astype(np.int16) - b_rgb.astype(np.int16)) * amp, 0, 255).astype(np.uint8)
    return d

def viridis_heatmap(err_2d, vmax=None):
    if vmax is None:
        vmax = max(1.0, float(np.percentile(err_2d, 99.5)))
    norm = np.clip(err_2d / vmax, 0, 1)
    rgba = (cm.viridis(norm) * 255).astype(np.uint8)
    return rgba[..., :3]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--diff-enc", default="v9a_floor25",
                    help="Encoder used for the |dec - orig| x8 column in zoom grids.")
    args = ap.parse_args()
    diff_enc = args.diff_enc

    images = sorted(TEST.glob("*.png"))
    summary = []
    total_bytes = 0
    out_files = []

    for img_path in images:
        name = img_path.stem
        print(f"== {name} ==")
        orig = load_rgb(img_path)

        all_encs = list({e for e, _ in ENCODERS_STRIP} | {e for e, _ in ENCODERS_CROP} | {e for e, _ in ENCODERS_ERR} | {diff_enc})
        decoded = {}
        mvals = {}
        for enc in all_encs:
            p = DEC / f"{name}__{enc}" / "decoded.png"
            if not p.exists():
                print(f"  MISSING {p}"); continue
            d = load_rgb(p)
            decoded[enc] = d
            mvals[enc] = metrics(orig, d)
        summary.append({"image": name, "metrics": {k: dict(zip(["psnr_y","psnr_rgb","ssim_y"], v)) for k,v in mvals.items()}})

        # --- 1) Full-res strip ---
        panels = [orig]
        labels = [f"original (1920x1080)"]
        for enc, short in ENCODERS_STRIP:
            if enc not in decoded: continue
            panels.append(decoded[enc])
            py, pr, ss = mvals[enc]
            labels.append(f"{short}  PSNR_Y={py:.2f}  SSIM_Y={ss:.4f}  PSNR_RGB={pr:.2f}")
        strip = compose_row(panels, labels, label_h=40, fsz=22)
        outp = OUT / f"{name}_strip_1x.png"
        strip.save(outp, optimize=True)
        total_bytes += outp.stat().st_size
        out_files.append(outp)
        print(f"  wrote {outp.name} ({strip.size})")

        # --- 2/3/4) Zoom grids: 2x, 4x, 8x ---
        regions = pick_regions(orig)
        # zoom=2: native 480x270 -> 960x540
        # zoom=4: native 240x135 -> 960x540
        # zoom=8: native 120x67  -> 960x536
        for zoom, native_w, native_h in [(2, 480, 270), (4, 240, 135), (8, 120, 67)]:
            rows_p, rows_l = [], []
            for region_name, (x0, y0) in regions.items():
                # Center the smaller crops inside the chosen 480x270 region.
                cx = x0 + (480 - native_w) // 2
                cy = y0 + (270 - native_h) // 2
                row_panels = []
                row_labels = []
                orig_c = crop(orig, cx, cy, native_w, native_h)
                row_panels.append(upsample_nn(orig_c, zoom))
                row_labels.append(f"{region_name}: orig ({cx},{cy}) {native_w}x{native_h}@{zoom}x")
                for enc, short in ENCODERS_CROP:
                    if enc not in decoded: continue
                    dc = crop(decoded[enc], cx, cy, native_w, native_h)
                    row_panels.append(upsample_nn(dc, zoom))
                    py, pr, ss = mvals[enc]
                    row_labels.append(f"{short}  Y={py:.2f}  S={ss:.4f}")
                # diff column
                if diff_enc in decoded:
                    diff_c = crop(decoded[diff_enc], cx, cy, native_w, native_h)
                    d_c = diff_amplified(orig_c, diff_c, amp=8)
                    row_panels.append(upsample_nn(d_c, zoom))
                    row_labels.append(f"|{diff_enc} - orig| x8")
                rows_p.append(row_panels)
                rows_l.append(row_labels)
            grid = compose_grid(rows_p, rows_l, label_h=32, fsz=16)
            outp = OUT / f"{name}_{zoom}x.png"
            grid.save(outp, optimize=True)
            total_bytes += outp.stat().st_size
            out_files.append(outp)
            print(f"  wrote {outp.name} ({grid.size})")

        # --- 5) Y-error heatmaps ---
        yo = y_channel(orig)
        y_errs = {}
        for enc, _ in ENCODERS_ERR:
            if enc not in decoded: continue
            y_errs[enc] = np.abs(yo - y_channel(decoded[enc]))
        if y_errs:
            ymax = max(float(np.percentile(e, 99.5)) for e in y_errs.values())
            ymax = max(ymax, 4.0)
            panels_y, labels_y = [], []
            for enc, short in ENCODERS_ERR:
                if enc not in y_errs: continue
                hm = viridis_heatmap(y_errs[enc], vmax=ymax)
                panels_y.append(hm)
                py, pr, ss = mvals[enc]
                labels_y.append(f"{short} |dY| vmax={ymax:.1f} Y={py:.2f}")
            comp = compose_row(panels_y, labels_y, label_h=40, fsz=22)
            outp = OUT / f"{name}_errors_y.png"
            comp.save(outp, optimize=True)
            total_bytes += outp.stat().st_size
            out_files.append(outp)
            print(f"  wrote {outp.name}")

        # --- 6) RGB-max error heatmaps ---
        rgb_errs = {}
        for enc, _ in ENCODERS_ERR:
            if enc not in decoded: continue
            rgb_errs[enc] = np.max(np.abs(orig.astype(np.int16) - decoded[enc].astype(np.int16)), axis=2).astype(np.float32)
        if rgb_errs:
            rmax = max(float(np.percentile(e, 99.5)) for e in rgb_errs.values())
            rmax = max(rmax, 8.0)
            panels_r, labels_r = [], []
            for enc, short in ENCODERS_ERR:
                if enc not in rgb_errs: continue
                hm = viridis_heatmap(rgb_errs[enc], vmax=rmax)
                panels_r.append(hm)
                py, pr, ss = mvals[enc]
                labels_r.append(f"{short} max|dRGB| vmax={rmax:.1f} RGB={pr:.2f}")
            comp = compose_row(panels_r, labels_r, label_h=40, fsz=22)
            outp = OUT / f"{name}_errors_rgb.png"
            comp.save(outp, optimize=True)
            total_bytes += outp.stat().st_size
            out_files.append(outp)
            print(f"  wrote {outp.name}")

    sj = OUT / "metrics.json"
    sj.write_text(json.dumps(summary, indent=2))
    print(f"\nTotal bytes: {total_bytes:,}")
    print(f"Files (sorted):")
    for p in sorted(OUT.glob("*")):
        print(f"  {p}  ({p.stat().st_size:,} B)")

if __name__ == "__main__":
    main()
