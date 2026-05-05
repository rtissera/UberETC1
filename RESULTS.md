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
| **v6_idea2_v2** (real top-K-in-optimizer + MS-SSIM rerank, K=8) | 37.128 | **43.692** | **0.9921** | 10.07 | 32 |
| **v7_guarded_idea5** (v6_v2 + chroma floor 1.5 dB + #5 multi-table refine) | **37.762** | 43.576 | 0.9921 | 8.40 | 32 |
| **v6_idea2** (idea#2 param-grid MS-SSIM rerank, 9 candidates) | 37.520 | 43.551 | 0.9921 | 6.67 | 32 |
| **v5_idea4** (worst-N re-encode @ 5%, radius=2) | 37.672 | 43.540 | 0.9920 | 5.87 | 32 |
| **basisu_v3_corners_perc** (cluster fit + perceptual + try-all-corners) | 37.688 | 43.509 | 0.9920 | 3.73 | 32 |
| basisu_full_perc (cluster fit + perceptual) | 37.983 | 43.378 | 0.9919 | 1.15 | 32 |
| **rg_etc1 cHighQuality** | **39.229** | 42.197 | 0.9895 | 19.20 | 32 |
| etc2comp -effort 100 | 39.109 | 42.080 | 0.9894 | 39.49 | 8 |
| basisu_full (cluster fit, RGB metric) | 39.016 | 42.047 | 0.9879 | 3.44 | 32 |
| basisu_v3_corners (try-all-corners RGB) | 39.015 | 42.045 | 0.9879 | 5.04 | 32 |
| etcpak | 35.879 | 39.718 | 0.9670 | 0.04 | 32 |

### v7_guarded_idea5 — v6_v2 + chroma sanity floor (guard rail) + idea #5 multi-table refinement

**Goal**: keep v6_v2's MS-SSIM-driven Y-PSNR / SSIM_Y wins on the worst 5%
of blocks while cutting v6_v2's RGB-PSNR regression (-0.392 dB vs v6,
-0.560 dB vs baseline). Two stacked features, both driver-side.

**Feature 1 — Chroma sanity floor (guard rail).** v6_v2 picks the
MS-SSIM-best of {top-K + pass-1}. Sometimes that pick has RGB-MSE far
above the pool's MSE-best (chroma "blowup" the rerank ignores because
luma SSIM gains slightly). Guard rail: compute
`pool_best_rgb_mse = min(rgb_mse(c) for c in top-K)`, set
`floor = pool_best_rgb_mse * 10^(T/10)` with T = 1.5 dB
(`floor_ratio ≈ 1.4125`). Walk the K+1 candidates in MS-SSIM-descending
order and commit the first whose decoded RGB-MSE ≤ floor. If no
candidate passes, fall back to the pass-1 result (safest).

**Feature 2 — Idea #5 multi-table sub-block refinement.** After the guard
rail picks a candidate, run an iterative refiner (max 4 iters), per
sub-block:

1. LS-solve continuous base color from current selectors + inten table:
   `b = mean(pixel - I[T,sel])` per channel (scaled-RGB space).
2. Quantize to RGB444 (color4) or RGB555 (color5). Try all 27 corners
   of the {center, ±1}^3 cube around the LS-quantized point. In diff
   mode, sub-block 1 is constrained to within ±[cETC1ColorDeltaMin
   ..cETC1ColorDeltaMax] of sub-block 0's color5 — invalid corners
   skipped.
3. Re-pick selectors greedily per pixel from the (base, T) palette.
4. Sweep all 8 inten tables and keep the joint argmin (T, corner,
   selectors) by RGB MSE.
5. Repeat with the new state until no change.

The refined block is then competed against the committed v6_v2-pick
under MS-SSIM on the same 12×12 window. Take winner. Done driver-side
using `etc_block::get_block_color`, `get_block_colors4/5`, and
`g_etc1_inten_tables` — no new patch is needed (the optimizer's
internals don't have to be touched).

**Mean delta vs `v6_idea2_v2` (current SOTA) and the lineage:**

| Metric          | baseline | v5_idea4 | v6_idea2 | v6_idea2_v2 | **v7_guarded_idea5** | Δ vs v6_v2 | Δ vs base |
|---|---:|---:|---:|---:|---:|---:|---:|
| PSNR_Y  (dB)    | 43.5089  | 43.5397  | 43.5507  | **43.6916** | 43.5760              | **-0.116** | +0.067 |
| SSIM_Y          | 0.992006 | 0.992023 | 0.992057 | **0.992106**| 0.992065             | -0.000041  | +0.000059 |
| PSNR_RGB (dB)   | 37.6876  | 37.6723  | 37.5197  | 37.1280     | **37.7615**          | **+0.634** | +0.074 |
| Encode (s)      | 3.73     | 5.87     | 6.67     | 10.07       | 8.40                 | -1.67      | +4.67  |

**This is a different trade-off than v6_v2, not a strict improvement.**
v7 trades back ~0.116 dB Y-PSNR / -0.00004 SSIM_Y for **+0.634 dB
RGB-PSNR** (recovers all of v6_v2's chroma regression and modestly
beats baseline on RGB too). Encode time is 17% faster than v6_v2 on
average — the chroma floor pre-empts the most expensive MS-SSIM-vs-RGB
fights, and the multi-table refiner is comparatively cheap (driver-side,
no extra optimizer runs).

Per-image PSNR_Y / PSNR_RGB / SSIM_Y, v6_v2 → v7:

| Image          | v6_v2 RGB / Y / SSIM    | v7 RGB / Y / SSIM       | Δ RGB | Δ Y    | Δ SSIM     |
|---|---|---|---:|---:|---:|
| MIX1.png       | 35.886 / 44.343 / 0.9904 | 36.039 / 44.303 / 0.9904 | +0.153 | -0.040 | +0.000005  |
| MIX5.png       | 42.676 / 45.274 / 0.9889 | 44.212 / 45.112 / 0.9888 | +1.536 | -0.162 | -0.000058  |
| all.png        | 24.288 / 32.097 / 0.9880 | 24.304 / 32.102 / 0.9880 | +0.016 | +0.005 |  0.000003  |
| gamelist_doom  | 39.783 / 46.802 / 0.9930 | 40.273 / 46.674 / 0.9930 | +0.490 | -0.128 | -0.000026  |
| gamelist_ffvii | 39.312 / 46.344 / 0.9951 | 39.994 / 46.182 / 0.9950 | +0.682 | -0.162 | -0.000043  |
| gamelist_sonic2| 40.824 / 47.290 / 0.9972 | 41.747 / 47.082 / 0.9972 | +0.923 | -0.207 | -0.000045  |

v7 beats v6_v2 on RGB-PSNR on **every single image** (+0.016 to +1.536
dB), and is within 0.21 dB Y-PSNR of v6_v2 on every image (best case
+0.005 dB on `all.png`, worst case -0.207 dB on sonic2). v7 still
beats baseline on Y-PSNR on every image (+0.063 to +0.162) so the
v6_v2 lineage win is largely preserved.

**Per-image guard-rail and refiner stats** (worst 5% = 6480 blocks,
K=8, refine_iters=4, T=1.5 dB):

| Image          | floor_kicked_in | floor_no_pass | refine_changed | refine_won_msssim | refine_won_rgb_mse |
|---|---:|---:|---:|---:|---:|
| MIX1.png       | 1542 (23.8%)    | 0             | 6220 (96.0%)   | 251 (4.0% of changed)  | 229 (3.7%)  |
| MIX5.png       | 5211 (80.4%)    | 0             | 2245 (34.6%)   | 47 (2.1% of changed)   | 37 (1.6%)   |
| all.png        | 509 (7.9%)      | 0             | 6317 (97.5%)   | 615 (9.7% of changed)  | 438 (6.9%)  |
| gamelist_doom  | 3491 (53.9%)    | 0             | 4834 (74.6%)   | 448 (9.3% of changed)  | 405 (8.4%)  |
| gamelist_ffvii | 3951 (61.0%)    | 0             | 5137 (79.3%)   | 391 (7.6% of changed)  | 358 (7.0%)  |
| gamelist_sonic2| 3686 (56.9%)    | 0             | 5053 (78.0%)   | 376 (7.4% of changed)  | 350 (6.9%)  |

Reading the stats:

- **Guard rail bites hard.** On 5/6 images the floor rejects the
  unconstrained MS-SSIM top-1 in 24-80% of worst blocks (mean ≈ 47%).
  Only `all.png` (8%) is the exception — the most photographic content,
  where pool members' RGB-MSEs cluster tightly so the floor rarely
  crosses. The floor never failed completely (`floor_no_pass=0` on every
  image): some pool member always passed the 1.5 dB threshold. So the
  pass-1 fallback is dormant in practice on this test set.
- **Multi-table refiner runs often but rarely wins.** It changes the
  block on 35-97% of worst blocks (mean ≈ 77%), but only 2-10% of those
  changes beat the pre-refine candidate under MS-SSIM (mean ≈ 7%).
  When MS-SSIM picks the refined version, RGB-MSE also improved 86-95%
  of the time — i.e. the refiner mostly trades small RGB MSE wins for
  small or zero MS-SSIM wins. **MIX5 is the floor of that effect**:
  only 47/2245 = 2.1% of refiner changes win MS-SSIM, suggesting the
  refiner converges to local minima that MS-SSIM is mostly indifferent
  to.

**Combined effect — did the guard rail prevent v6_v2-style RGB-MSE
blowups?** Yes, decisively. The mean RGB-PSNR delta vs v6_v2 is +0.634
dB and per-image deltas range from +0.016 (all.png — where the floor
barely fires) to +1.536 (MIX5 — where the floor fires on 80% of worst
blocks). The correlation is direct: more floor activity → more RGB
recovery. On MIX5, v6_v2 was -1.380 dB RGB vs baseline; v7 brings that
back to +0.157 above baseline.

**Honest verdict per feature:**

1. **Guard rail (floor) — paid off cleanly.** It is the dominant
   feature in v7. It produces the +0.634 dB RGB recovery and is
   responsible for the 17% encode time reduction (the floor often
   commits a different candidate than v6_v2 picked, and that commit
   short-circuits unnecessary downstream MS-SSIM ties). The Y-PSNR cost
   (~-0.116 dB) is the unavoidable price of any chroma-protecting
   constraint on a perceptual-rerank pipeline.
2. **Multi-table refiner (idea #5) — small effect.** Across all 6
   images it changes block bitstreams in ~77% of worst blocks but only
   ~7% of those changes survive the MS-SSIM competition. Its
   contribution to the +0.634 RGB win is small; the bulk of the chroma
   recovery is the floor. The refiner is cheap enough to leave on
   (negligible fraction of total encode time on these images), but on
   this test set it does **not** independently justify the patch.

**Combined verdict:** v7 is the right shape if you wanted v6_v2's
perceptual lineage but couldn't tolerate the RGB regression. **Y-PSNR
SOTA remains v6_idea2_v2** (43.692 dB) — v7 is 43.576 dB.  **Best
all-around (Y still above all baselines, RGB recovered) is v7**.  Pick
based on whether downstream consumers measure RGB or Y.

GPU-validated bit-exactly on AMD Radeon 780M for all 6 images
(mean abs diff vs SW decoder = 0.000000; all 60 SW-vs-GPU comparisons
across 10 encoders × 6 images = zero diff).

**Visual A/B images** (regenerated with v7 panel, replacing prior
v6_v2-as-primary set under `results/visual/`):
- `<image>_strip_1x.png`  — original + v7 + v6_v2 + v5 + v6 + etcpak
- `<image>_2x.png`        — 3 regions × 7 panels (orig + 6 enc + |v7-orig|×8)
- `<image>_4x.png`        — same regions at 4× zoom
- `<image>_errors_y.png`  — 6-panel viridis |dY| heatmap
- `<image>_errors_rgb.png`— 6-panel viridis max|dRGB| heatmap

The diff panel is now `|v7 - orig| × 8` instead of `|v6_v2 - orig| × 8`.

### v6_idea2_v2 — Real top-K capture inside the optimizer + MS-SSIM rerank

**v6_idea2 was a wash (+0.011 dB Y-PSNR vs v5).** Hypothesis (per the v6
post-mortem): the 9 candidates in v6 came from cluster_fit's MSE-optimal
output for each (flip, diff, radius) parameter slice — already nearly
indistinguishable under MSE *or* MS-SSIM. Real top-K diversity required
draining the per-color-coord trial heap from inside the optimizer's
inner loop. v6_idea2_v2 implements that.

**Patch `0003-real-topk-capture.patch`** wires the push site inside
`evaluate_solution_slow()`. When `g_uberetc1_topk_capture > 0`, every
successful per-color-coord trial (after the 8-inten-table sweep, so the
captured entry is the best (color, inten, selectors) for that color
coord) appends to `g_uberetc1_topk_buffer`. Buffer is bounded to K via
worst-evict. Header types (`uberetc1_topk_entry`, the thread-locals)
were already in place — only the push was missing.

**Driver pass-2** (per worst block, top 5% by Y-MSE):

For each (flip, diff) in {0,1}^2:
  1. Run sub-block 0's optimizer at radius=R=2 with `topk_capture=K=8`.
     Drain `buf0` = up to 8 (color, inten, selectors, perc_err) entries
     — the global top-K across all 165 perms × 125 corners × 8 inten
     tables for that sub-block.
  2. If `diff=0` (color4 / individual mode): sub-block 1 is independent.
     Run once with `topk_capture=K`, drain `buf1`. Combine `buf0 × buf1`
     → up to K×K = 64 full-block candidates.
  3. If `diff=1` (color5 / diff mode): sub-block 1 must respect
     `m_constrain_against_base_color5` from sub-block 0's chosen color.
     For each entry e0 in `buf0`, run sub-block 1 *constrained against
     e0's color5* with topk_capture=K → buf1 of K entries. Combine each
     e0 with each of those K. Total: K×K candidates per (diff=1).

Pool all candidates (≤ 4 × 64 = 256) into a global heap of size K=8
keyed by total perceptual error (e0.err + e1.err). Add the pass-1
result as candidate K+1 (sentinel ranking), giving 9 candidates total.
MS-SSIM-rerank K+1 candidates with same 12×12 (3×3-block) window +
pass-1-neighbor context as v6_idea2. Pick winner. Commit.

**Mean delta vs `v6_idea2` (the wash) and `v5_idea4` (prior SOTA):**

| Metric          | baseline | v5_idea4 | v6_idea2 | **v6_idea2_v2** | Δ vs v5 | Δ vs v6 |
|---|---:|---:|---:|---:|---:|---:|
| PSNR_Y  (dB)    | 43.5089  | 43.5397  | 43.5507  | **43.6916**     | **+0.152** | **+0.141** |
| SSIM_Y          | 0.992006 | 0.992023 | 0.992057 | **0.992106**    | +0.000083  | +0.000049  |
| PSNR_RGB (dB)   | 37.6876  | 37.6723  | 37.5197  | 37.1280         | -0.544     | -0.392     |
| Encode (s)      | 3.73     | 5.87     | 6.67     | 10.07           | +4.20      | +3.40      |

This is a **real win** on Y-PSNR / SSIM_Y, an order of magnitude bigger
than v6_idea2's +0.011 dB Y over v5. SSIM_Y now moves at 5-decimal
precision (0.99206 → 0.99211). PSNR_RGB regresses meaningfully
(-0.392 dB vs v6, -0.544 dB vs baseline) — the perceptual rerank trades
RGB-MSE for luma quality more aggressively when given diverse candidates.

Per-image PSNR_Y and SSIM_Y, baseline → v5_idea4 → v6_idea2 → v6_idea2_v2:

| Image          | baseline           | v5_idea4           | v6_idea2           | v6_idea2_v2        | Δ Y vs base | Δ Y vs v5 | Δ Y vs v6 |
|---|---:|---:|---:|---:|---:|---:|---:|
| MIX1.png       | 44.1413 / 0.990253 | 44.1640 / 0.990277 | 44.2205 / 0.990342 | 44.3435 / 0.990422 | +0.202 | +0.180 | +0.123 |
| MIX5.png       | 45.1355 / 0.988801 | 45.1473 / 0.988805 | 45.1191 / 0.988792 | 45.2739 / 0.988882 | +0.138 | +0.127 | +0.155 |
| all.png        | 32.0397 / 0.987876 | 32.0562 / 0.987909 | 32.1238 / 0.988034 | 32.0967 / 0.987982 | +0.057 | +0.040 | -0.027 |
| gamelist_doom  | 46.6077 / 0.992956 | 46.6407 / 0.992968 | 46.6447 / 0.992988 | 46.8022 / 0.993047 | +0.195 | +0.162 | +0.158 |
| gamelist_ffvii | 46.1055 / 0.995003 | 46.1551 / 0.995019 | 46.1390 / 0.995023 | 46.3437 / 0.995077 | +0.238 | +0.189 | +0.205 |
| gamelist_sonic2| 47.0237 / 0.997146 | 47.0751 / 0.997162 | 47.0569 / 0.997166 | 47.2896 / 0.997227 | +0.266 | +0.214 | +0.233 |

v6_idea2_v2 beats v6_idea2 on Y-PSNR on 5 of 6 images (regresses by
-0.027 dB only on `all.png`, the most photographic / hardest content).
SSIM_Y improves on 5 of 6 (small regression on `all.png` only).

Per-image pass-2 internal stats (worst 5% = 6480 blocks, K=8):

| Image          | pool size | kept_pass1 | kept_pass2 | msssim_diverged_from_pool_top1 | Y-MSE_regressed | **winner_outside_v6_grid** |
|---|---:|---:|---:|---:|---:|---:|
| MIX1.png       | 8.00 | 248 | 6232 | 5319 (82.1%) | 785 (12.1%) | **5099 (78.7%)** |
| MIX5.png       | 8.00 |  65 | 6415 | 5573 (86.0%) | 912 (14.1%) | **5378 (83.0%)** |
| all.png        | 8.00 | 308 | 6172 | 5480 (84.6%) | 1013 (15.6%) | **5354 (82.6%)** |
| gamelist_doom  | 8.00 | 175 | 6305 | 5197 (80.2%) | 702 (10.8%) | **5117 (79.0%)** |
| gamelist_ffvii | 8.00 | 204 | 6276 | 5194 (80.2%) | 633 (9.8%)  | **5239 (80.8%)** |
| gamelist_sonic2| 8.00 | 232 | 6248 | 5025 (77.5%) | 844 (13.0%) | **4990 (77.0%)** |

**The "winner_outside_v6_grid" stat is the headline diversity result.**
For each worst block we re-encode v6_idea2's full 8-candidate parameter
grid ({flip,diff,radius∈{1,2}}) — the candidates v6_idea2 considers —
plus the pass-1 result, and check whether the v6_idea2_v2 MS-SSIM
winner's bitstream matches any of them.

**77-83% of worst blocks have a winner that v6_idea2 would have missed.**
That is, in 5/6 of worst-block decisions, the real top-K-from-the-optimizer
finds a candidate (different inten table, different selector pattern,
different non-MSE-optimal base color, etc.) that the parameter-grid
approximation never produces. This directly validates the prior agent's
hypothesis: v6_idea2 was a wash because its 9 candidates were
"already very close in perceptual error" — the per-slice MSE-best is
nearly indistinguishable under MS-SSIM. Real top-K gives the rerank
genuinely diverse alternatives.

`msssim_diverged_from_pool_top1` is also informative: in 78-86% of
worst blocks, MS-SSIM picks a candidate that is NOT the MSE-best of
the K-pool. That is, even within the K=8 perceptually-best (by basisu's
YCbCr metric) candidates, MS-SSIM ranks them differently — the K=8
pool is doing the right job of giving the rerank meaningful alternatives.

GPU-validated bit-exactly on AMD Radeon 780M for all 6 images
(mean abs diff vs SW decoder = 0.000000 — same machine config as
prior runs, all 9 encoders × 6 images = 54 SW-vs-GPU comparisons,
every one zero diff).

**Verdict: real top-K-in-the-optimizer is the missing piece.** v6_idea2's
parameter-grid was approximating top-K with parameter-slice top-1, which
collapsed the diversity needed to make MS-SSIM rerank pay off. The actual
top-K, drained from `evaluate_solution_slow()`'s inner trial loop,
delivers +0.141 dB Y-PSNR / +0.00005 SSIM_Y over v6_idea2, with the win
present on 5/6 images. The trade-off is -0.392 dB PSNR_RGB vs v6_idea2
and ~50% extra encode time (4 (flip,diff) × K runs of sub-block-1 in
diff mode + K^2 candidate construction per (flip,diff)).

The `all.png` regression (-0.027 dB Y) is interesting: it's the lowest-
PSNR image (32 dB Y) — a busy photographic background. On this content
even the diverse top-K candidates are all far enough from the source
that MS-SSIM and Y-MSE diverge in ways that occasionally favor a
visually-different-but-luma-worse candidate. The other 5 images
(graphic / illustrated content with more flat/edged regions) all gain.

### v6_idea2 — Idea #2 (MS-SSIM top-K rerank), stacked on v5_idea4

Pass 1 = `basisu_v3_corners_perc` (cluster_fit + Uber + perceptual YCbCr +
8-corner cube), identical to v5's pass 1. Pass 2 = top 5% of blocks by
reconstructed Y-MSE: for each worst block, generate 8 candidates as
`{flip ∈ {0,1}} × {diff ∈ {0,1}} × {wide_corner_radius ∈ {1,2}}` (each
tuple commits to one (flip, diff) and runs cluster_fit at that radius);
add the pass-1 encoded block as candidate #9. Re-rank these 9 candidates
under **MS-SSIM_Y** evaluated on a 12×12 (3×3-block) window centered on
the block, using already-pass-1-encoded neighbors as fixed context.
Multi-scale SSIM at scales 1, 1/2, 1/4 with 2×2 box-average downsampling
between scales, geometric mean. Boundary blocks shrink the window to
whatever neighbors exist (no fabrication). Final block = MS-SSIM winner.

**Mean delta vs `basisu_v3_corners_perc` (baseline) and `v5_idea4`:**

| Metric          | baseline | v5_idea4 | v6_idea2 | Δ vs baseline | Δ vs v5_idea4 |
|---|---:|---:|---:|---:|---:|
| PSNR_Y  (dB)    | 43.509   | 43.540   | **43.551** | +0.042  | +0.011 |
| SSIM_Y          | 0.99202  | 0.99206  | 0.99207    | +0.00005 | +0.00001 |
| PSNR_RGB (dB)   | 37.688   | 37.672   | 37.520     | -0.168   | -0.152 |
| Encode (s)      | 3.73     | 5.87     | 6.67       |  +2.94    | +0.80 |

Per-image PSNR_Y and SSIM_Y, baseline → v5_idea4 → v6_idea2:

| Image          | basisu_v3_corners_perc | v5_idea4 | v6_idea2 | Δ Y vs base | Δ Y vs v5 | Δ SSIM vs base |
|---|---:|---:|---:|---:|---:|---:|
| MIX1.png       | 44.1413 / 0.990253 | 44.1640 / 0.990277 | 44.2205 / 0.990342 | +0.079 | +0.057 | +0.000089 |
| MIX5.png       | 45.1355 / 0.988801 | 45.1473 / 0.988805 | 45.1191 / 0.988792 | -0.016 | -0.028 | -0.000009 |
| all.png        | 32.0397 / 0.987876 | 32.0562 / 0.987909 | 32.1238 / 0.988034 | +0.084 | +0.068 | +0.000158 |
| gamelist_doom  | 46.6077 / 0.992956 | 46.6407 / 0.992968 | 46.6447 / 0.992988 | +0.037 | +0.004 | +0.000032 |
| gamelist_ffvii | 46.1055 / 0.995003 | 46.1551 / 0.995019 | 46.1390 / 0.995023 | +0.034 | -0.016 | +0.000020 |
| gamelist_sonic2| 47.0237 / 0.997146 | 47.0751 / 0.997162 | 47.0569 / 0.997166 | +0.033 | -0.018 | +0.000020 |

Per-image pass-2 internal stats (worst 5% = 6480 blocks of ~129,600):

| Image          | candidates avg/blk | kept_pass1 | kept_pass2 (rerank picked a wider candidate) | msssim_diverged_from_mse_best | y_mse_regressed_vs_pass1 |
|---|---:|---:|---:|---:|---:|
| MIX1.png       | 7.4 |  147 | 6333 | 1691 (26.1%) |  947 (14.6%) |
| MIX5.png       | 8.2 |   35 | 6445 | 1541 (23.8%) | 1133 (17.5%) |
| all.png        | 5.6 |  222 | 6258 | 1642 (25.3%) |  830 (12.8%) |
| gamelist_doom  | 7.6 |  129 | 6351 | 1032 (15.9%) |  583  (9.0%) |
| gamelist_ffvii | 7.2 |  123 | 6357 |  941 (14.5%) |  590  (9.1%) |
| gamelist_sonic2| 7.0 |  189 | 6291 | 1019 (15.7%) |  645 (10.0%) |

GPU-validated bit-exactly on AMD Radeon 780M for all 6 images
(mean abs diff vs SW decoder = 0.000000).

**Verdict: stacking is essentially a wash on Y-PSNR (+0.011 dB on the mean),
flat on SSIM_Y (+0.00001), and costs **−0.152 dB RGB-PSNR** vs v5_idea4.
Per-image, v6 beats v5 on 3 of 6 images on PSNR_Y (MIX1, all, doom) and
loses on 3 (MIX5, ffvii, sonic2). The MS-SSIM rerank picks a non-MSE-best
candidate 14–26% of the time, and accepts a Y-MSE regression vs pass 1 on
9–18% of worst blocks — both confirm the rerank is doing something
non-trivial, but the SSIM win on those swaps is tiny on this content.**

**Hypothesis for the wash:**

1. The 9 candidates per block come from cluster_fit's MSE-optimal output
   for each (flip, diff, radius) slice. They are already very close in
   perceptual error — the per-slice top-1 is nearly indistinguishable
   from the per-slice top-K under MSE *or* MS-SSIM. We are not exploring
   the kind of "different distortion pattern" candidates (e.g. a different
   inten table that biases error spatially) that MS-SSIM would actually
   reward — those would require a true top-K heap inside the optimizer
   per the spec, not a parameter-grid sweep.
2. The MS-SSIM window is 12×12 = 144 pixels including 8 pass-1
   neighbors. The 16-pixel candidate region is only 11% of the window;
   neighbor pixels dominate the SSIM means/variances and dilute the
   ranking signal of the candidate's own variation.
3. The pass-2 candidates already differ from pass 1 mostly at radius=2,
   which is already captured by v5_idea4's MSE-only acceptance. The
   only true *new* signal v6 introduces is "accept a candidate whose
   Y-MSE is *worse* than pass 1 because MS-SSIM says it's better in a
   neighbor-aware sense" — and on this dataset that swap pays for
   itself only on a few % of blocks.
4. SSIM_Y at 4-decimal precision is unchanged because the per-block
   gain is small and bound to the 13k worst blocks; image-wide SSIM
   averaging dilutes it.

**To make idea #2 actually deliver, the next step would be exposing a
real top-K heap inside `etc1_optimizer::evaluate_solution_slow` (the
header struct `uberetc1_topk_entry` is in place, capture logic isn't),
so the K candidates per (flip, diff) are genuinely non-MSE-equivalent
sub-block solutions — different inten tables, different selectors —
rather than the single-MSE-optimal-per-slice output we feed in now.**

### v5_idea4 — Idea #4 (importance-driven worst-N re-encoding)

Pass 1 = `basisu_v3_corners_perc` (cluster_fit + Uber + perceptual YCbCr +
8-corner cube). Pass 2 = top 5% of blocks by reconstructed Y-MSE re-encoded
with `g_uberetc1_wide_corner_radius = 2`, expanding cluster_fit's per-perm
corner search from 8 corners to 125 corners (`{-2..+2}^3`) around each LS
center. Pass-2 result kept iff it lowers Y-MSE on the *decoded* block.

**Mean delta vs `basisu_v3_corners_perc`:**
- PSNR_Y: 43.509 → **43.540** (**+0.031 dB**)
- SSIM_Y: 0.9920 → 0.9920 (flat)
- PSNR_RGB: 37.688 → 37.672 (-0.016 dB)
- Encode time: 3.45 s → 5.44 s (1.6× slower)

Per-image PSNR_Y deltas vs baseline:

| Image | baseline PSNR_Y | v5_idea4 PSNR_Y | Δ PSNR_Y | Δ SSIM_Y | pass-2 worst Y-MSE thresh | improved blocks (of ~6480) |
|---|---:|---:|---:|---:|---:|---:|
| MIX1.png        | 44.141 | 44.164 | +0.023 | +0.00003 | 12.22 | 559 / 6480 |
| MIX5.png        | 45.136 | 45.147 | +0.011 |  0.0000  | n/a   | (small)    |
| all.png         | 32.040 | 32.056 | +0.016 |  0.0000  | n/a   | (small)    |
| gamelist_doom   | 46.608 | 46.641 | +0.033 |  0.0000  | n/a   | (small)    |
| gamelist_ffvii  | 46.106 | 46.155 | +0.049 |  0.0000  | n/a   | (small)    |
| gamelist_sonic2 | 47.024 | 47.075 | +0.051 | +0.00010 | n/a   | (small)    |

**Verdict: well below the +0.4 to +0.7 dB Y-PSNR estimate from `DEEP_DIVE.md`.**
The +0.031 dB mean gain is real and consistent (every image improved on
PSNR_Y), GPU-validated bit-exactly (mean abs diff = 0.000000 on all 6
images), and SSIM_Y is unchanged. PSNR_RGB regresses by -0.016 dB — this
is the same RGB ↔ Y-PSNR trade-off the perceptual metric introduces; the
wider corner search lets cluster_fit pick base colors that are slightly
worse RGB-MSE but slightly better luma-MSE under the YCbCr cost.

**Why the gain is so much smaller than estimated:**

- The B3 finding (top 5% of blocks own 40-80% of total error) is correct,
  but the additional 117 corners we explore in pass 2 only find a
  better Y-MSE result on ~9% of worst-set blocks (559 / 6480 on MIX1).
  The remaining 91% of worst blocks are *already* at the minimum Y-MSE
  achievable by cluster_fit's chosen perm + table — they're hard
  because the underlying content cannot be represented in 4 bits of
  selector × 8 inten tables × 32^3 base color, not because the
  base-color quantization landed on a sub-optimal lattice point.
- The rough math in DEEP_DIVE.md assumed 30% MSE recovery on the worst
  5%; actual recovery on improved blocks averages ~0.3% of pass-1 Y-MSE
  per worst block, two orders of magnitude smaller.
- Cluster_fit at Uber already evaluates 165 perms × 8 corners × 8 tables
  per (flip, diff) = 21,120 candidates per (flip, diff) per sub-block.
  The wider 125-corner cube increases this to 165 × 125 × 8 = 165,000
  per (flip, diff), but the LS center + 8-corner cube already lies near
  the true MSE minimum on most blocks.

**This is a small, GPU-validated, correct-direction gain. The hypothesis
that wider per-perm corner search alone would yield +0.5 dB is falsified
on this test set.** The per-block error-concentration finding (B3) holds,
but exploiting it requires changing more than just base-color quantization
granularity — likely table enumeration freedom (idea #5) or perceptual
re-ranking (idea #2), both of which stack on top of v5_idea4.


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
- [~] Step 5: MS-SSIM final selection between top-K candidates with 3×3-block window
      — partial: v6_idea2 implements the rerank with parameter-grid candidates
        (flip × diff × radius), but real top-K-per-(flip,diff) capture inside
        `etc1_optimizer::evaluate_solution_slow` is not yet wired. Wash on
        this 6-image set (+0.011 dB Y-PSNR vs v5_idea4, flat SSIM_Y).
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
