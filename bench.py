#!/usr/bin/env python3
"""ETC1 encoder benchmark on 1920x1080 opaque PNGs.

For each test image and each encoder:
  1. Encode source PNG -> ETC1 file
  2. Decode ETC1 file -> reconstructed PNG
  3. Compare original vs reconstructed using neutral skimage PSNR/SSIM (RGB)

Encoders:
  - etcpak           (heuristic, fastest)
  - ETCPACK -s slow  (Ericsson reference, exhaustive)
  - etc2comp -effort 100 (Google block-archetype)
  - rg_etc1 cHighQuality (lattice scan + iter refine)
  - basisu_full_etc1 (cluster fit, both flips x diff/individual, Uber)
"""

import os, sys, subprocess, time, shutil, tempfile, json
from pathlib import Path
import numpy as np
from PIL import Image
from skimage.metrics import structural_similarity as ssim_fn

ROOT = Path("/home/romain/etc1_bench")
BUILD = ROOT / "build"
TEST = ROOT / "test_images"
OUT = ROOT / "decoded"
OUT.mkdir(exist_ok=True)
RESULTS = ROOT / "results"
RESULTS.mkdir(exist_ok=True)

ENCODERS = {
    # ETCPACK (Ericsson reference) intentionally omitted: too slow for the
    # quality it delivers, and licensed under a non-OSI Ericsson license that
    # complicates downstream use. Both rg_etc1 cHighQuality and basisu cluster
    # fit match or beat its quality at >50x the throughput.
    "etcpak":      str(BUILD / "etcpak/etcpak"),
    "etc2comp_e100": str(BUILD / "etc2comp/EtcTool/EtcTool"),
    "rg_etc1_hq":  str(BUILD / "rg_etc1_tool"),
    "basisu_full": str(BUILD / "basisu_etc1_tool"),
    "basisu_full_perc": str(BUILD / "basisu_etc1_tool_v2"),
    "basisu_v3_corners": str(BUILD / "basisu_etc1_tool_v3"),
    "basisu_v3_corners_perc": str(BUILD / "basisu_etc1_tool_v3_perc"),
    # research-branch v4: adaptive per-block effort + (optional) YCoCg + (optional) dither
    "v4_default":        str(BUILD / "basisu_etc1_tool_v4"),
    "v4_dither":         str(BUILD / "basisu_etc1_tool_v4"),
    "v4_ycocg":          str(BUILD / "basisu_etc1_tool_v4_ycocg"),
    "v4_ycocg_dither":   str(BUILD / "basisu_etc1_tool_v4_ycocg"),
    "v4_no_adaptive":    str(BUILD / "basisu_etc1_tool_v4"),
    # research-branch v5: idea #4, importance-driven worst-N re-encoding.
    # Pass 1 = v3_corners_perc; pass 2 widens corner cube to 125 around LS
    # centers for the worst 5% of blocks by reconstructed Y-MSE.
    "v5_idea4":          str(BUILD / "basisu_etc1_tool_v5"),
    # research-branch v6: idea #2 (MS-SSIM top-K rerank) stacked on v5_idea4.
    # Same pass 1; pass 2 generates 8 candidates per worst block × (flip,diff,
    # radius∈{1,2}) plus the pass-1 result, reranks by MS-SSIM_Y on a 12×12
    # (3×3 block) window using already-pass-1-encoded neighbors as context.
    "v6_idea2":          str(BUILD / "basisu_etc1_tool_v6"),
    # research-branch v6_v2: REAL top-K capture inside the optimizer
    # (g_uberetc1_topk_capture/buffer wired up in evaluate_solution_slow).
    # Per worst block: pull top-K=8 (color, inten, selectors) per sub-block
    # for each (flip, diff), combine into K×K full-block candidates, pool
    # global top-K=8 by perceptual error, MS-SSIM-rerank K+1 (K + pass-1).
    "v6_idea2_v2":       str(BUILD / "basisu_etc1_tool_v6_v2"),
    # research-branch v7: v6_v2 + chroma sanity floor (guard rail) +
    # idea #5 multi-table sub-block refinement on the picked candidate.
    "v7_guarded_idea5":  str(BUILD / "basisu_etc1_tool_v7_guarded_idea5"),
}

def run(cmd, timeout=600):
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r, time.time() - t0

def encode_decode(name, src_png, work_dir):
    """Run encoder, return path to decoded PNG and encoding wall time (seconds)."""
    work_dir = Path(work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    dec_png = work_dir / "decoded.png"

    if name == "etcpak":
        pvr = work_dir / "out.pvr"
        _, t_enc = run([ENCODERS["etcpak"], "-c", "etc1", str(src_png), str(pvr)])
        # Decode using etcpak itself
        run([ENCODERS["etcpak"], "-v", str(pvr), str(dec_png)])
        return dec_png, t_enc

    if name == "etc2comp_e100":
        ktx = work_dir / "out.ktx"
        _, t_enc = run([ENCODERS["etc2comp_e100"], str(src_png),
                        "-format", "ETC1", "-effort", "100",
                        "-errormetric", "numeric", "-j", "8",
                        "-output", str(ktx)])
        # Decode using etcpak's -v on the KTX
        run([ENCODERS["etcpak"], "-v", str(ktx), str(dec_png)])
        return dec_png, t_enc

    if name == "rg_etc1_hq":
        bin_path = work_dir / "out.bin"
        _, t_enc = run([ENCODERS["rg_etc1_hq"], "encode", str(src_png), str(bin_path)])
        run([ENCODERS["rg_etc1_hq"], "decode", str(bin_path), str(dec_png)])
        return dec_png, t_enc

    if name == "basisu_full":
        bin_path = work_dir / "out.bin"
        _, t_enc = run([ENCODERS["basisu_full"], "encode", str(src_png), str(bin_path)])
        run([ENCODERS["basisu_full"], "decode", str(bin_path), str(dec_png)])
        return dec_png, t_enc

    if name == "basisu_full_perc":
        bin_path = work_dir / "out.bin"
        _, t_enc = run([ENCODERS["basisu_full_perc"], "encode", str(src_png), str(bin_path)])
        run([ENCODERS["basisu_full_perc"], "decode", str(bin_path), str(dec_png)])
        return dec_png, t_enc

    if name == "basisu_v3_corners":
        bin_path = work_dir / "out.bin"
        _, t_enc = run([ENCODERS["basisu_v3_corners"], "encode", str(src_png), str(bin_path)])
        run([ENCODERS["basisu_v3_corners"], "decode", str(bin_path), str(dec_png)])
        return dec_png, t_enc

    if name == "basisu_v3_corners_perc":
        bin_path = work_dir / "out.bin"
        _, t_enc = run([ENCODERS["basisu_v3_corners_perc"], "encode", str(src_png), str(bin_path)])
        run([ENCODERS["basisu_v3_corners_perc"], "decode", str(bin_path), str(dec_png)])
        return dec_png, t_enc

    # ---- v4 research-branch variants (adaptive + YCoCg/YCbCr + dither) ----
    v4_envs = {
        "v4_default":      {"UBERETC1_PERCEPTUAL": "1", "UBERETC1_ADAPTIVE": "1", "UBERETC1_DITHER": "0"},
        "v4_dither":       {"UBERETC1_PERCEPTUAL": "1", "UBERETC1_ADAPTIVE": "1", "UBERETC1_DITHER": "1"},
        "v4_ycocg":        {"UBERETC1_PERCEPTUAL": "1", "UBERETC1_ADAPTIVE": "1", "UBERETC1_DITHER": "0"},
        "v4_ycocg_dither": {"UBERETC1_PERCEPTUAL": "1", "UBERETC1_ADAPTIVE": "1", "UBERETC1_DITHER": "1"},
        "v4_no_adaptive":  {"UBERETC1_PERCEPTUAL": "1", "UBERETC1_ADAPTIVE": "0", "UBERETC1_DITHER": "0"},
    }
    if name in v4_envs:
        bin_path = work_dir / "out.bin"
        env = {**os.environ, **v4_envs[name]}
        t0 = time.time()
        subprocess.run([ENCODERS[name], "encode", str(src_png), str(bin_path)],
                       env=env, capture_output=True, timeout=600)
        t_enc = time.time() - t0
        subprocess.run([ENCODERS[name], "decode", str(bin_path), str(dec_png)],
                       env=env, capture_output=True, timeout=120)
        return dec_png, t_enc

    if name == "v5_idea4":
        bin_path = work_dir / "out.bin"
        env = {**os.environ,
               "UBERETC1_WORST_PCT": "5.0",
               "UBERETC1_WORST_RADIUS": "2"}
        t0 = time.time()
        subprocess.run([ENCODERS[name], "encode", str(src_png), str(bin_path)],
                       env=env, capture_output=True, timeout=900)
        t_enc = time.time() - t0
        subprocess.run([ENCODERS[name], "decode", str(bin_path), str(dec_png)],
                       env=env, capture_output=True, timeout=120)
        return dec_png, t_enc

    if name == "v6_idea2":
        bin_path = work_dir / "out.bin"
        env = {**os.environ,
               "UBERETC1_WORST_PCT": "5.0",
               "UBERETC1_WORST_RADIUS": "2"}
        t0 = time.time()
        subprocess.run([ENCODERS[name], "encode", str(src_png), str(bin_path)],
                       env=env, capture_output=True, timeout=900)
        t_enc = time.time() - t0
        subprocess.run([ENCODERS[name], "decode", str(bin_path), str(dec_png)],
                       env=env, capture_output=True, timeout=120)
        return dec_png, t_enc

    if name == "v6_idea2_v2":
        bin_path = work_dir / "out.bin"
        env = {**os.environ,
               "UBERETC1_WORST_PCT": "5.0",
               "UBERETC1_WORST_RADIUS": "2",
               "UBERETC1_TOPK": "8"}
        t0 = time.time()
        subprocess.run([ENCODERS[name], "encode", str(src_png), str(bin_path)],
                       env=env, capture_output=True, timeout=1800)
        t_enc = time.time() - t0
        subprocess.run([ENCODERS[name], "decode", str(bin_path), str(dec_png)],
                       env=env, capture_output=True, timeout=120)
        return dec_png, t_enc

    if name == "v7_guarded_idea5":
        bin_path = work_dir / "out.bin"
        env = {**os.environ,
               "UBERETC1_WORST_PCT": "5.0",
               "UBERETC1_WORST_RADIUS": "2",
               "UBERETC1_TOPK": "8",
               "UBERETC1_CHROMA_FLOOR_DB": "1.5",
               "UBERETC1_REFINE_ITERS": "4"}
        t0 = time.time()
        # Capture stderr to a log so we can extract per-image stats.
        log_path = work_dir / "encode.log"
        with open(log_path, "wb") as lf:
            subprocess.run([ENCODERS[name], "encode", str(src_png), str(bin_path)],
                           env=env, stdout=subprocess.DEVNULL, stderr=lf, timeout=1800)
        t_enc = time.time() - t0
        subprocess.run([ENCODERS[name], "decode", str(bin_path), str(dec_png)],
                       env=env, capture_output=True, timeout=120)
        return dec_png, t_enc

    raise ValueError(name)


def metric(orig_png, dec_png):
    a = np.asarray(Image.open(orig_png).convert("RGB"), dtype=np.float64)
    b = np.asarray(Image.open(dec_png).convert("RGB"), dtype=np.float64)
    if a.shape != b.shape:
        return None
    mse = ((a - b)**2).mean()
    psnr = 10 * np.log10(255*255 / mse) if mse > 0 else float("inf")
    # Y-PSNR (BT.601 luma)
    ay = 0.299*a[...,0] + 0.587*a[...,1] + 0.114*a[...,2]
    by = 0.299*b[...,0] + 0.587*b[...,1] + 0.114*b[...,2]
    mse_y = ((ay - by)**2).mean()
    psnr_y = 10 * np.log10(255*255 / mse_y) if mse_y > 0 else float("inf")
    # SSIM on luma
    ssim = ssim_fn(ay, by, data_range=255.0)
    return {"psnr_rgb": psnr, "psnr_y": psnr_y, "ssim_y": ssim, "mse_rgb": mse}


def main():
    images = sorted(TEST.glob("*.png"))
    if not images:
        sys.exit("no test images")

    results = []
    encoders_to_run = list(ENCODERS.keys())
    if len(sys.argv) > 1:
        encoders_to_run = sys.argv[1:]

    for img in images:
        for enc in encoders_to_run:
            tag = f"{img.stem}__{enc}"
            wd = OUT / tag
            print(f"[+] {tag}", flush=True)
            try:
                dec, t_enc = encode_decode(enc, img, wd)
                if not dec.exists():
                    print(f"    MISSING decoded output", flush=True)
                    continue
                m = metric(img, dec)
                if m is None:
                    print(f"    SHAPE MISMATCH", flush=True)
                    continue
                row = {
                    "image": img.name, "encoder": enc, "encode_seconds": round(t_enc, 3),
                    **{k: round(v, 4) for k, v in m.items()}
                }
                results.append(row)
                print(f"    PSNR_RGB={m['psnr_rgb']:.3f} PSNR_Y={m['psnr_y']:.3f} "
                      f"SSIM_Y={m['ssim_y']:.4f} t={t_enc:.2f}s", flush=True)
            except Exception as e:
                print(f"    ERROR: {e}", flush=True)

    out_json = RESULTS / "results.json"
    out_json.write_text(json.dumps(results, indent=2))
    print(f"\nSaved {out_json}")

    # Print compact table
    print("\n=== Summary (mean across images) ===")
    enc_set = sorted({r["encoder"] for r in results})
    for enc in enc_set:
        rows = [r for r in results if r["encoder"] == enc]
        if not rows: continue
        avg = lambda k: sum(r[k] for r in rows) / len(rows)
        print(f"  {enc:20s} PSNR_RGB={avg('psnr_rgb'):.3f}  PSNR_Y={avg('psnr_y'):.3f}  "
              f"SSIM_Y={avg('ssim_y'):.4f}  t={avg('encode_seconds'):.2f}s  (n={len(rows)})")

if __name__ == "__main__":
    main()
