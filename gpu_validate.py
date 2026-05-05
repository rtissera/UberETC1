#!/usr/bin/env python3
"""Validate every encoder's bitstream by GPU-decoding via gl_decode and
comparing PSNR vs original. Prints SW PSNR vs GPU PSNR side by side.

For encoders that produce KTX/PVR (etcpak, etc2comp), uses extract_blocks.py
to extract raw blocks first.
"""
import subprocess, sys, json
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path("/home/romain/etc1_bench")
TEST = ROOT / "test_images"
DEC = ROOT / "decoded"
GL_BIN = ROOT / "build/gl_decode"
EXTRACT = ROOT / "extract_blocks.py"

# Mapping of encoder -> name of bitstream file inside decoded/<image>__<encoder>/
BITSTREAM = {
    "etcpak": "out.pvr",
    "etc2comp_e100": "out.ktx",
    "rg_etc1_hq": "out.bin",          # already raw
    "basisu_full": "out.bin",
    "basisu_full_perc": "out.bin",
    "basisu_v3_corners": "out.bin",
    "basisu_v3_corners_perc": "out.bin",
    "v5_idea4": "out.bin",
    "v6_idea2": "out.bin",
    "v6_idea2_v2": "out.bin",
}
RAW_OK = {"rg_etc1_hq", "basisu_full", "basisu_full_perc", "basisu_v3_corners",
          "basisu_v3_corners_perc", "v5_idea4", "v6_idea2", "v6_idea2_v2"}


def psnr(a, b):
    mse = ((a.astype(np.float64) - b.astype(np.float64)) ** 2).mean()
    return 10 * np.log10(255*255 / mse) if mse > 0 else float("inf")


def gpu_decode(bitstream_path, image_w, image_h, raw_ok):
    if raw_ok:
        bin_for_gl = bitstream_path
    else:
        # Convert via extract_blocks.py
        bin_for_gl = bitstream_path.with_suffix(".gl.bin")
        r = subprocess.run(["python3", str(EXTRACT), str(bitstream_path), str(bin_for_gl)],
                           capture_output=True, text=True)
        if r.returncode:
            return None
    out_png = bitstream_path.with_name("decoded_gpu.png")
    r = subprocess.run([str(GL_BIN), str(bin_for_gl), str(out_png)],
                       capture_output=True, text=True)
    if r.returncode:
        print(f"  GPU decode failed: {r.stderr[:200]}", file=sys.stderr)
        return None
    return out_png


def main():
    results = []
    for img in sorted(TEST.glob("*.png")):
        orig = np.asarray(Image.open(img).convert("RGB"))
        for enc, fname in BITSTREAM.items():
            tag = f"{img.stem}__{enc}"
            wd = DEC / tag
            bitstream = wd / fname
            if not bitstream.exists():
                continue
            sw_png = wd / "decoded.png"
            if not sw_png.exists():
                continue
            sw = np.asarray(Image.open(sw_png).convert("RGB"))
            gpu_png = gpu_decode(bitstream, orig.shape[1], orig.shape[0], enc in RAW_OK)
            if gpu_png is None:
                continue
            gpu = np.asarray(Image.open(gpu_png).convert("RGB"))
            sw_p = psnr(orig, sw)
            gpu_p = psnr(orig, gpu)
            mad = np.abs(sw.astype(np.int16) - gpu.astype(np.int16)).mean()
            row = {"image": img.name, "encoder": enc, "sw_psnr": round(sw_p, 4),
                   "gpu_psnr": round(gpu_p, 4), "sw_vs_gpu_meandiff": round(float(mad), 6)}
            results.append(row)
            print(f"{tag:60s} SW={sw_p:7.3f}  GPU={gpu_p:7.3f}  Δ={mad:.6f}")

    Path(ROOT / "results/gpu_validation.json").write_text(json.dumps(results, indent=2))
    # summary
    print("\n=== GPU vs SW max abs mean-diff per encoder ===")
    encs = sorted({r["encoder"] for r in results})
    for e in encs:
        mads = [r["sw_vs_gpu_meandiff"] for r in results if r["encoder"] == e]
        if mads:
            print(f"  {e:25s} max={max(mads):.6f}  mean={sum(mads)/len(mads):.6f}")

if __name__ == "__main__":
    main()
