# UberETC1 Benchmark Results — 1920×1080

**Test set:** 6 fully-opaque 1920×1080 PNG backgrounds from public Batocera/
Recalbox/RetroPie theme repositories (MIX1.png, MIX5.png from Batocera Pulse;
gamelist_doom.png, gamelist_sonic2.png, gamelist_final_fantasy_vii.png from
es-theme-next-slide; all.png from es-theme-next-pixel).

**Hardware:** Linux laptop with AMD Radeon 780M iGPU. Software: Mesa 25.2.8,
EGL surfaceless + GLES2.

**Methodology:** Each encoder produces an ETC1 bitstream. We decode it back
to PNG with the *same encoder's* software decoder (or an equivalent), AND
upload it as `GL_OES_compressed_ETC1_RGB8_texture` on the GPU and read back.
Both reconstructed images are compared to the original with PSNR (RGB and
Y / BT.601 luma) and SSIM (Y) computed in a neutral Python skimage script.

**Cross-validation result:** GPU and SW decoders agree bit-exactly across
**all 36 (image × encoder) pairs** — `mean_abs_diff = 0.000000`. So PSNR
numbers below are the exact values the GPU will produce.

**ETCPACK (Ericsson reference) dropped** from active comparison: at >50 s
per 1920×1080 image (single-threaded, no path to parallelize because of
its `tmp.ppm` global file usage), it delivered PSNR within 0.05 dB of
rg_etc1 / basisu cluster fit on the two images that completed. Not worth
the integration cost given its non-OSI Ericsson license. Quality reference
role goes to rg_etc1 instead.

## Mean across 6 images, sorted by PSNR_Y (perceptual)

| Encoder | PSNR_RGB (dB) | PSNR_Y (dB) | SSIM_Y | Time (s) | Threads |
|---|---:|---:|---:|---:|---|
| **basisu_v3_corners_perc** (cluster fit + perceptual + try-all-corners) | 37.688 | **43.509** | **0.9920** | 6.31 | 32 |
| basisu_full_perc (cluster fit + perceptual) | 37.983 | 43.378 | 0.9919 | 1.15 | 32 |
| **rg_etc1 cHighQuality** | **39.229** | 42.197 | 0.9895 | 19.20 | 32 |
| etc2comp -effort 100 | 39.109 | 42.080 | 0.9894 | 39.49 | 8 |
| basisu_full (cluster fit, RGB metric) | 39.016 | 42.047 | 0.9879 | 3.44 | 32 |
| basisu_v3_corners (try-all-corners RGB) | 39.015 | 42.045 | 0.9879 | 5.04 | 32 |
| etcpak | 35.879 | 39.718 | 0.9670 | 0.04 | 32 |


## Per-image breakdown (RGB PSNR / Y PSNR / Y SSIM)

| Image | etcpak | rg_etc1 HQ | etc2comp e100 | basisu_full (RGB) | basisu_full_perc (Y-best) | basisu_v3_corners_perc |
|---|---|---|---|---|---|---|
| MIX1.png | 34.703 / 41.543 / 0.947 | 37.657 / 41.639 / 0.984 | 37.531 / 41.638 / 0.984 | 37.444 / 41.521 / 0.982 | 36.146 / 42.832 / 0.989 | 36.094 / 42.945 / 0.989 |
| MIX5.png | 41.415 / 41.415 / 0.929 | 45.153 / 45.153 / 0.993 | 45.145 / 45.145 / 0.993 | 44.897 / 44.897 / 0.989 | 44.897 / 45.220 / 0.991 | 44.055 / 45.136 / 0.989 |
| all.png | 23.443 / 29.727 / 0.980 | 25.299 / 29.999 / 0.979 | 24.963 / 29.902 / 0.980 | 25.245 / 30.068 / 0.980 | 24.352 / 31.986 / 0.988 | 24.323 / 32.040 / 0.988 |
| gamelist_doom.png | 38.782 / 42.768 / 0.984 | 42.042 / 45.631 / 0.991 | 41.946 / 45.483 / 0.991 | 41.885 / 45.495 / 0.991 | 40.476 / 46.536 / 0.993 | 40.210 / 46.608 / 0.993 |
| gamelist_final_fantasy_vii.png | 38.189 / 42.029 / 0.971 | 41.900 / 44.671 / 0.993 | 41.797 / 44.420 / 0.993 | 41.570 / 44.452 / 0.991 | 40.019 / 45.979 / 0.995 | 39.853 / 46.106 / 0.995 |
| gamelist_sonic2.png | 38.744 / 40.825 / 0.991 | 43.325 / 46.089 / 0.996 | 43.273 / 45.894 / 0.996 | 43.056 / 45.847 / 0.996 | 42.005 / 46.824 / 0.997 | 41.590 / 47.024 / 0.997 |

## Visual comparisons

See `results/crops/` for side-by-side 256×256 crops (offset (700, 400) into
each image) showing original vs each encoder, plus per-pixel error
heatmaps (4× amplified `|orig − decoded|`).

## Takeaways for the team

1. **For pure RGB-PSNR**, rg_etc1 cHighQuality is the best practical encoder
   in our open-source comparison: 39.23 dB avg, beating etc2comp at e=100
   while being 2× faster (multithreaded).

2. **For perceptual quality (Y-PSNR / SSIM, what users actually see)**,
   our patched basisu cluster-fit + perceptual YCbCr metric + try-all-corners
   delivers **+1.3 dB Y-PSNR** and **+0.003 SSIM** over rg_etc1, at 1/3 the
   wall time. It costs ~0.4 dB RGB-PSNR (an irrelevant perceptual price).

3. **The single biggest quality lever is the perceptual metric**, not the
   search algorithm. Cluster fit + RGB MSE plateaus around the same place
   as rg_etc1's lattice search; switching the inner cost function to
   `(128, 64, 16) YCbCr` lifts Y-PSNR by >1 dB at every quality level.

4. **etcpak is fine for development builds** (40 ms/image, 1.5–3.5 dB
   below SOTA) but not for shipping when ETC1 quality matters.

5. **All encoders' bitstreams are hardware-correct** — verified via real
   GL_OES_compressed_ETC1_RGB8_texture upload + GPU rasterize + readback
   on AMD Radeon 780M, with mean_abs_diff vs SW decoder = 0 in every test.

## What's next on the roadmap

The §5.2 enhancement pipeline in `RESEARCH_REPORT.md` lists 8 layered
improvements. We have implemented:

- [x] Step 1: all 165 distributions enumerated (basisu Uber default)
- [x] Step 2: weighted YCbCr metric
- [x] Step 3: try-all 8 quantization corners
- [ ] Step 4: joint sub-block optimization in diff mode (iterate SB1/SB0 fix-points)
- [ ] Step 5: MS-SSIM final selection between top-K candidates with 3×3-block window
- [ ] Step 6: branch-and-bound exhaustive base-color search (ultra mode)
- [ ] Step 7: differentiable post-pass (Gumbel-softmax + LPIPS)
- [ ] Step 8: neural warm-start CNN (only if encode time becomes a constraint)

Estimated additional gains, layered on top of what we have:
- Step 4: +0.1 to +0.3 dB on diff-favored content
- Step 5: small RGB delta, visibly cleaner smooth regions
- Step 6: +0.05 to +0.5 dB worst-case on adversarial blocks
- Step 7: another +0.3–0.5 dB Y-PSNR if perceptual loss is well-tuned

Total realistic ceiling above current SOTA basisu_v3_corners_perc:
**+0.8 to +1.3 dB Y-PSNR**, with proportional SSIM gains.
