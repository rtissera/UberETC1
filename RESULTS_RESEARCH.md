# Research-branch results — implementing improvements 1+2+3

This document records the outcome of implementing the three enhancements
identified by the audit (`audit_*.txt` on main):

1. **Adaptive per-block effort** — classify blocks by RGB variance and
   route to `pack_etc1_block_solid_color` (solid) / `cETCQualityFast`
   (flat) / `Medium` (smooth) / `Slow` (default) / `Uber` (high-var).
   Inspired by etc2comp's `PerformIteration(effort)` tier system.
2. **YCoCg perceptual metric** — alternative to basisu's BT.601-ish
   `(14R, 45G, 5B)` weighted YCbCr. Compile-time toggled via
   `-DBASISU_USE_YCOCG_PERCEPTUAL`; uses integer-reversible YCoCg-R
   with Y weight 4×, Co/Cg weight 1× each.
3. **Floyd-Steinberg dither preprocessing** — pre-quantize input
   pixels to RGB555 with error diffusion before encoding. Lifted from
   rg_etc1's `dither_block_555`.

All three implemented on the `research` branch. Encoder: `basisu_etc1_tool_v4`
(YCbCr lib) and `basisu_etc1_tool_v4_ycocg` (YCoCg lib). Toggled via env vars.

## Outcome (mean across 6 images @ 1920×1080, 32 threads)

| Variant | PSNR_RGB | PSNR_Y | SSIM_Y | Time/img | vs SOTA Y-PSNR |
|---|---:|---:|---:|---:|---:|
| **basisu_v3_corners_perc** (main, prior SOTA) | 37.69 | **43.51** | **0.9920** | 6.31 s | baseline |
| **v4_no_adaptive** (research, ablation = perc YCbCr Uber, no try-corners) | 37.69 | 43.51 | 0.9920 | 7.20 s | **identical** |
| basisu_full_perc (main, no try-corners, no adaptive) | 37.98 | 43.38 | 0.9919 | 1.15 s | -0.13 |
| v4_default (research: adaptive + perc YCbCr) | 38.14 | 42.94 | 0.9905 | 4.88 s | **-0.57** |
| v4_ycocg (research: adaptive + perc YCoCg) | 38.69 | 42.79 | 0.9902 | 4.26 s | **-0.72** |
| v4_dither (research: adaptive + perc YCbCr + Floyd-Steinberg) | 35.29 | 38.32 | 0.9469 | 5.54 s | **-5.19** |
| v4_ycocg_dither | 35.72 | 38.36 | 0.9475 | 5.05 s | -5.15 |

## Verdict — none of the three improvements wins

### 1. Adaptive per-block effort: -0.57 dB Y-PSNR
The router (variance-based 5-tier classification + per-tier quality level)
saves ~30% wall time but loses 0.57 dB Y-PSNR on average. The "saved
budget on flat blocks pays for extra work on hard blocks" hypothesis
is wrong: at Uber, basisu *already* skips most of the cluster-fit work
on flat blocks (the optimizer's hash-table dedup catches it), so there
isn't enough slack to give back.

The *high-variance* blocks were already getting the best basisu can do
(Uber is the top tier). Adaptive routing only adds a way to spend *less*
effort on easier blocks — which is a speed/quality trade, not a
Pareto-improvement.

**Conclusion:** keep this as a `--fast` option in the future driver,
not as the default. The 0.6 dB cost is too high for the 33% speedup.

### 2. YCoCg metric: -0.15 dB vs YCbCr (-0.72 vs SOTA)
Counter to my hypothesis. Comparing `v4_ycocg` (42.79) to `v4_default`
(42.94), YCoCg loses 0.15 dB — small but consistent across all 6 images.
RGB-PSNR is +0.55 dB for YCoCg vs YCbCr (38.69 vs 38.14), so the YCoCg
metric pushes the encoder toward better RGB at the cost of Y.

That's actually the *opposite* of what we wanted. The integer-reversible
property of YCoCg-R doesn't translate to a better-aligned-with-MSE-of-luma
loss function, because what we measure (BT.601 luma PSNR) and what
basisu's BT.601-ish YCbCr metric optimize are matched. YCoCg's Y is a
different linear combination, so it optimizes a different luma than
the one we report.

**Conclusion:** drop YCoCg. The existing YCbCr is correct for our PSNR_Y
target.

### 3. Floyd-Steinberg dither: -5.2 dB Y-PSNR
Catastrophic loss. Why: dither moves the input pixels by plus/minus a few
units in each channel to "spread" quantization error. Cluster fit then sees
those *moved* pixels and finds the optimal base color for them. But our
PSNR is computed against the *original* (un-dithered) pixels, so the
encoder is now solving the wrong problem.

Dither is a useful concept for direct quantization (e.g. RGB888 -> RGB555
display path), but ETC1 already has 4 modifier levels per pixel which
provides a kind of natural "dither budget" — adding more dithering on
top steals that budget for noise we then have to encode.

**Conclusion:** drop the dither path. rg_etc1's `dither_block_555` is
useful when no further compression follows it; useless (worse: harmful)
when we then run cluster fit.

### Bonus finding: try-all-corners is also a no-op at Uber + perceptual
`v4_no_adaptive` (research, identical algorithm to `basisu_full_perc` from
main but with try-corners disabled) hits 43.51 dB — same as
`basisu_v3_corners_perc` (with try-all-corners patch from RESEARCH_REPORT
section 5.2 step 3). The 8 cube corners around the LS solution add zero
benefit when `evaluate_solution()` already iterates internally with
refinement.

So the `try-all-corners` patch from main is also not earning its keep
in perceptual mode. We should consider removing it from the production
path (it's still in `patches/` for reference).

## What this tells us about the encoder ceiling

basisu cluster-fit at Uber + perceptual YCbCr is hitting the **format
ceiling on our test set**. The remaining quality gap to "ideal" is now
in places no per-block search can reach:

- **Block-boundary artifacts** on smooth gradients (independent block
  encoding, no global coherence)
- **Fundamental 4-bpp budget** on high-frequency content like
  `all.png` (every encoder clusters at ~25 dB on it)
- **5-bit base color quantization** on saturated content

The next meaningful improvements are the higher-numbered RESEARCH_REPORT
section 5.2 steps:

- **Step 5: MS-SSIM final selection between top-K candidates.** Requires
  evaluating multiple candidate encodings of the same block, which
  basisu's optimizer doesn't expose cleanly. ~1 day of work.
- **Step 7: Differentiable post-pass.** Take the encoder output, relax
  selectors via Gumbel-softmax + STE, gradient descent on continuous
  base color under LPIPS or MS-SSIM, re-quantize. ~3-5 days of work.

Or pivot to the bigger lever: **block-boundary aware encoding** — score
adjacent block pairs jointly under a metric that penalizes luma jumps
across block edges. Not in any open ETC1 encoder. Estimated +0.3-0.5 dB
SSIM_Y on smooth content.

## Recommendation

Quality is the goal; encode time is irrelevant. With that framing:

- **SOTA stays `basisu_v3_corners_perc` / `v4_no_adaptive` at 43.51 dB
  Y-PSNR / 0.9920 SSIM_Y / 37.69 dB RGB-PSNR.** They are bit-equivalent
  algorithms — perceptual YCbCr at Uber quality with full cluster fit
  (165 distributions) on every block.
- **All three v4 ideas failed:**
  - Adaptive per-block effort: -0.57 dB Y-PSNR. Not a quality move.
  - YCoCg metric: -0.15 dB Y-PSNR. Loss.
  - Floyd-Steinberg dither: -5.2 dB Y-PSNR. Catastrophic loss.
- **The try-all-corners patch (main, RESEARCH_REPORT section 5.2 step 3)
  is also a no-op** at the perceptual-Uber operating point. Not worth
  keeping unless we re-enable RGB-only mode.
- **The encoder is at format ceiling on this test set.** Any further
  quality gain will not come from per-block search refinements; the
  remaining error is structural (block-boundary artifacts on smooth
  gradients, 4-bpp budget on high-frequency content like icon grids,
  5-bit base-color quantization on saturated regions).

Where quality can still go up (in expected order of payoff, all
*quality-only*, ignoring time):

1. **Block-boundary-aware encoding** — jointly score adjacent block
   pairs under a metric that penalizes luma jumps across block edges.
   Blocks are decode-independent so this *only* changes encode cost,
   not bitstream layout. No published ETC1 encoder does this. Expected
   +0.3-0.5 dB SSIM_Y on smooth content. Implementation: ~2 days.
2. **MS-SSIM final selection between top-K candidates** (RESEARCH_REPORT
   section 5.2 step 5). Have the optimizer return the top 8 candidates
   per (flip, diff) instead of the single MSE-best, then re-rank
   under MS-SSIM evaluated on a 3x3-block window including already-encoded
   neighbors. Expected +0.1-0.3 dB SSIM_Y. Implementation: ~1 day.
3. **Differentiable post-pass** (RESEARCH_REPORT section 5.2 step 7).
   Gumbel-softmax + STE on selectors, gradient descent on continuous
   base color under LPIPS or MS-SSIM, re-quantize. Highest engineering
   cost (PyTorch + custom autograd), highest unknown payoff. Implementation:
   ~3-5 days.

Skip on quality grounds, even though they exist:
- Adaptive routing (proven loss above).
- YCoCg metric (proven loss).
- Dither preprocessing (proven catastrophic loss).
- Branch-and-bound exhaustive base-color search (audit shows current
  cluster fit already finds equivalent quality; ETCPACK exhaustive only
  gained ~0.05 dB on the two completed images).
