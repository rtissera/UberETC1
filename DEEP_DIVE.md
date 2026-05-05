# Deep dive on the three remaining ideas + new candidates

This document goes below the surface on the three ideas suggested at the end of `RESULTS_RESEARCH.md`, with numbers from our actual test set, and proposes additional ideas grounded in fresh measurements.

## Empirical baselines (basisu_v3_corners_perc on 6 images @ 1920×1080)

I measured three properties of the *current SOTA encoder's residual error* to grade each candidate idea against reality, not theory.

### B1. Block-boundary error vs interior error

| Image | total MSE_Y | border-pixel MSE_Y | interior-pixel MSE_Y | ratio |
|---|---:|---:|---:|---:|
| MIX1 | 2.51 | 2.53 | 2.43 | 1.04× |
| MIX5 | 1.99 | 2.02 | 1.91 | 1.06× |
| all | 40.66 | 41.08 | 39.39 | 1.04× |
| gamelist_doom | 1.42 | 1.46 | 1.31 | 1.12× |
| gamelist_ffvii | 1.59 | 1.63 | 1.50 | 1.08× |
| gamelist_sonic2 | 1.29 | 1.31 | 1.23 | 1.07× |

(Border pixels = 75% of total because every block's outer ring counts. The numbers are pixel-area-weighted MSE, so the ratio measures error *per pixel* — not per area.)

**Takeaway:** border-pixel error is at most 12% higher than interior error. Block-boundary artifacts are NOT a dominant error source on our test set. The "block-boundary aware encoding" hypothesis loses most of its appeal.

### B2. Inter-block error correlation

| Image | h_corr | v_corr | P(both adjacent in top 10%) | random baseline |
|---|---:|---:|---:|---:|
| MIX1 | 0.62 | 0.59 | 6.2% | 1.0% |
| MIX5 | 0.59 | 0.59 | 6.3% | 1.0% |
| all | 0.42 | 0.40 | 2.7% | 1.0% |
| gamelist_doom | 0.42 | 0.43 | 5.1% | 1.0% |
| gamelist_ffvii | 0.40 | 0.45 | 6.4% | 0.9% |
| gamelist_sonic2 | 0.39 | 0.39 | 6.1% | 1.0% |

**Takeaway:** Neighbor blocks DO fail together (5–6× more often than random chance). But this is because hard *regions* (text, edges) span multiple blocks, not because per-block decisions are uncoordinated. Joint encoding can't trivially fix this — the blocks are individually hard.

### B3. Error concentration (the big one)

| Image | top 1% blocks | top 5% | top 10% | top 25% |
|---|---:|---:|---:|---:|
| MIX1 | 15.9% | 43.8% | 62.4% | 87.8% of total error |
| MIX5 | 17.7% | 42.7% | 60.5% | 86.3% |
| all | 6.1% | 22.2% | 37.2% | 67.3% |
| gamelist_doom | 36.0% | 54.6% | 62.7% | 78.7% |
| gamelist_ffvii | 42.6% | 69.3% | 76.8% | 88.2% |
| **gamelist_sonic2** | **44.0%** | **78.1%** | **87.0%** | **96.4%** |

**Takeaway:** This is the biggest signal in the data. On 5 of 6 images, the **top 10% of blocks contribute 60–87% of total error**. On `gamelist_sonic2`, **the top 1% of blocks (about 1,300 blocks out of 130k) produce 44% of all the error**.

Implication: improvements that target the *worst-N* blocks at extreme cost are far more leveraged than uniform improvements across all blocks. The cluster fit at Uber spends the same effort on every block, including the 90% that already encode well.

---

## Idea #1 — Block-boundary aware encoding (joint pair scoring)

**Original hypothesis:** Score adjacent block pairs jointly, penalize luma jumps across block edges. Estimated +0.3–0.5 dB SSIM_Y on smooth content.

### Verdict after deep dive: **expected gain revised down to +0.05–0.15 dB SSIM_Y**

### Why the gain is smaller than I claimed

- **B1 shows border error is only 4–12% higher than interior error.** If border-only error were eliminated entirely (an upper bound no algorithm achieves), total MSE_Y drops by ~3–5%, which is **+0.13 to +0.22 dB**. Realistic algorithms capture maybe a third of that, so **+0.04 to +0.08 dB**.
- **B2 shows the correlation isn't artifactual.** Adjacent blocks fail together because the underlying content is hard, not because the encoder is making locally bad decisions. Joint encoding can't fix content that's intrinsically hard.
- The cleanest version (Lloyd-style alternating optimization on adjacent pairs) costs ~4× encode time for ~0.05–0.1 dB. It's not free.

### What it would actually look like in code

```cpp
// After single-pass cluster fit:
// For each pair of horizontally-adjacent blocks (B[y][x], B[y][x+1]):
//   1. Compute current "edge cost" = sum over the 4 boundary-column pixel pairs
//      of |reconstructed_left[3, y'] - reconstructed_right[0, y']|.
//   2. Try perturbations: re-encode B[y][x] with a base color shifted toward
//      B[y][x+1]'s base color (and vice versa); accept the perturbation only
//      if (block_MSE_increase) < lambda * (edge_cost_decrease).
//   3. Iterate until no swap helps (Gauss-Seidel sweep over the image).
// Same for vertical.
```

This requires lambda tuning (rate of edge-cost ↔ block-MSE trade-off). Bad lambda → smoothing artifacts ("posterized" gradients across whole regions). Good lambda → small SSIM gain.

### Decision: **deprioritize**. The empirical ceiling (B1) doesn't justify it.

---

## Idea #2 — MS-SSIM final selection between top-K candidates

**Original hypothesis:** Have the optimizer return top-K (e.g. K=8) candidate encodings per block instead of the single MSE-best, then re-rank under MS-SSIM evaluated on a 3×3-block window including already-encoded neighbors. Estimated +0.1–0.3 dB SSIM_Y. ~1 day of work.

### Verdict after deep dive: **expected gain +0.10 to +0.25 dB SSIM_Y, but mostly on the worst 10% of blocks**

### Why it's plausible

- **B3 shows the worst 10% of blocks dominate total error.** A re-ranker that picks better among the top-K candidates for the 13k worst blocks per image (10% of 130k) and leaves the other 117k alone is a very leveraged change.
- MS-SSIM on a 3×3-block window (12×12 pixels) is computable in ~5μs/block using running window means. For the worst-10% subset, that's ~13k blocks × 8 candidates × 5μs ≈ 0.5s of overhead per image. Cheap.
- Cluster fit's `evaluate_solution_slow()` already maintains a `m_best_solution` and a `m_trial_solution`. Extending to keep a `top_k_solutions[K]` heap is a localized change in `basisu_etc.cpp` (about 50 LOC).

### Why I'm uncertain about the upper bound

- Top-K candidates from cluster fit are very similar in MSE (by construction — they're all near-optimal). Whether MS-SSIM ranking differs *meaningfully* from MSE ranking on those near-tied candidates is empirical.
- Initial back-of-envelope: among top-K=8 cluster-fit candidates, the MSE spread is typically <2%. If MS-SSIM picks a different winner, the SSIM gain is at most ~1% of the SSIM-error budget, which translates to roughly **+0.05 to +0.15 dB SSIM_Y** on the worst blocks specifically.
- We have to be careful about ordering effects: if MS-SSIM's evaluation depends on already-encoded neighbors, the encoding becomes order-dependent and we lose strict per-block parallelism. Workaround: do a first pass with MSE, then a second sweep with MS-SSIM that uses the first-pass neighbors. ~2× total encode time.

### What it would actually look like in code

```cpp
// In etc1_optimizer::compute() — keep a heap of K=8 best solutions.
struct potential_solution_topk {
    potential_solution sols[K];
    void try_insert(const potential_solution& s) {
        // standard "keep top K by MSE" heap
    }
};

// In encode_block_full_etc1() — after both sub-blocks optimized,
// produce K candidates per (flip, diff) by reading the top-K heap;
// total candidates = 4 × K.

// Second pass over the image, after pass-1 produced one candidate per block:
//   for each block b:
//     compute MS-SSIM(reconstructed[b], original[b]) using the existing
//     pass-1 reconstructions of b's 8 neighbors as context
//     pick the top-K candidate that maximizes MS-SSIM
```

### Decision: **highest-priority of the three**. ~1 day of work, leverages the B3 finding.

---

## Idea #3 — Differentiable post-pass

**Original hypothesis:** Take encoder output, relax selectors via Gumbel-softmax + STE, gradient descent on continuous base color under LPIPS or MS-SSIM, re-quantize. Estimated +0.3–0.5 dB Y-PSNR, 3–5 days of work.

### Verdict after deep dive: **expected gain +0.0 to +0.2 dB Y-PSNR, with high implementation risk**

### What the literature actually shows

Reading the NTBC paper (https://arxiv.org/html/2407.09543v2) carefully:

- They use STE on the argmax over 4 possible selector indices for BC1 (same idea translates to ETC1's 4 selectors).
- Their formulation: `∂w_n/∂d_n = (1/T) · σ(d_n) · (n − w_n_hat)` with temperature T=0.01. This is *not* Gumbel-softmax — it's a softmax-weighted STE variant.
- Their loss is L2 on endpoints + L2 on decoded colors. They don't use LPIPS or MS-SSIM.
- **Their reported result is *worse* than traditional BC1**: ~15% PSNR degradation, ~5% SSIM degradation. The whole *point* of NTBC is supercompression (40% smaller files), not quality.

So the literature does NOT support "differentiable post-pass beats traditional cluster fit at fixed bitrate." The differentiable encoders shown to date are at best comparable, and trade quality for compression.

### Why a *post-pass* might still help (vs full neural encoder)

- We start from cluster-fit output (already near-optimal under MSE). The differentiable pass only needs to find the small remaining improvement under a *different* metric (LPIPS or MS-SSIM).
- The continuous-base-color manifold is small (RGB444 = 12 bits, so 4096 grid points; gradient descent can perturb by ±1 quantization step per channel = 8 candidates in parallel).
- The new metric (LPIPS/MS-SSIM) genuinely differs from MSE in what it values, so even if the metric tells the encoder "no improvement possible under MSE", it can still find improvement under perception.

### Why the implementation risk is high

- Need a Python ↔ C++ pipeline: PyTorch for autograd, C++ basisu for the discrete encoder. 3–5 days minimum, possibly more.
- LPIPS requires a pretrained CNN (~10 MB weights, ImageNet domain mismatch on game UI textures).
- The gradient signal through STE is high-variance — convergence may need careful temperature scheduling, multiple restarts.
- Inside a 4×4 block, the LPIPS receptive field is too small to be meaningful; you have to evaluate over 16×16 or larger windows, which couples the optimization across blocks (back to ordering issues).

### Decision: **deprioritize until idea #2 is exhausted**. The expected gain is small and the engineering cost is high. Worth doing as a research vignette, not as a production pipeline change.

---

# New ideas not in the previous list

These came out of the empirical analysis (B1–B3) and the literature reading.

## Idea #4 — Per-block extra effort on the worst-N candidates (importance-driven encoding)

**Motivation:** B3 shows the top 10% of blocks dominate total error. Currently every block gets the same effort.

**Mechanism:**
1. First pass: encode every block with current cluster fit + Uber. Record per-block error.
2. Identify the worst-N blocks (e.g. N = top 5% of error tail).
3. Second pass: re-encode each worst block with **explicitly more search effort**: enumerate all 165 distributions × {-2, -1, 0, +1, +2}^3 = 165 × 125 = 20,625 candidates per (flip, diff, table) instead of the default Uber's effective ~165. Pick the lowest-error result.

**Expected gain:** if we improve the worst 5% blocks by 30% in MSE (plausible — they're under-fit), and they contribute 60% of total error, total MSE drops by 0.05 × 0.30 × (1/MSE) ≈ ... actually let's be concrete:
- Current MSE_Y on MIX1 = 2.51; top-5%-block contribution ≈ 0.44 × 2.51 = 1.10.
- If 30% of *that* is recoverable: new total MSE_Y ≈ 2.51 − 0.33 = 2.18.
- That's PSNR_Y from 44.13 to 44.74 — **+0.61 dB on MIX1**.

**Why this is plausible:** at Uber, basisu does cluster fit's 165 distributions, but each distribution's evaluation only tries the LS-optimal base color (with try-all-corners trying ±1 in each channel). On a block where the LS solution is wrong by more than 1 quantization step, we never find the better base color. The hard blocks are exactly the ones where LS is wrong.

**Implementation cost:** ~half a day. Add a 2nd-pass option to the v4 driver that re-encodes blocks above an error threshold with an explicit base-color search radius.

**Risk:** Encoding time on hard blocks increases ~125×, but for only 5% of blocks, so total time grows ~7×. Acceptable since quality is the only goal.

## Idea #5 — Per-block multi-table search (escape local optima of intensity table choice)

**Motivation:** Currently cluster fit picks the best table per (flip, diff) and refines. If two tables give near-identical MSE, the encoder picks one and never revisits. But the perceptual / SSIM optimum may be the *other* table (different distortion pattern).

**Mechanism:** For top-N blocks (per #4 above), enumerate all 8 tables × all 165 distributions × ±2 base color cube = 8 × 165 × 125 = 165,000 candidates. Pick best under SSIM-on-3x3-window (per #2).

**Expected gain:** stacks with #4 — a further +0.05–0.15 dB.

## Idea #6 — Selector-bit RDO across the image

**Motivation:** ETC1's 16×2-bit selector field per block is 32 bits = 4 bytes. If we can *steer* the per-block selector pattern to match a frequently-occurring pattern, the LZ compressor downstream gets shorter codes (this is what Geldreich's `bc7enc_rdo` does for BC1/BC7).

**Caveat for our use case:** if we're not LZ-compressing the .ktx file (e.g. we're shipping raw GPU textures), this idea has zero benefit. Worth checking with the team whether the deployment path includes LZ. If yes (typical for Android `.ktx2` payload + Zstd), this is the highest-leverage idea: bc7enc_rdo claims **20–50% file-size reduction at near-equal quality** for BC formats — likely similar for ETC1.

**Implementation cost:** large. Either port bc7enc_rdo's RDO machinery to ETC1 (2–4 weeks) or use basis_universal's existing ETC1S RDO (which we already have, but it's the *constrained* ETC1S subset, not full ETC1).

**Decision:** **separate question**. Ask the team if shipping path is LZ-compressed.

## Idea #7 — Importance-map weighted MSE (saliency-driven)

**Motivation:** Not all pixels matter equally. UI text and faces should be encoded more accurately than background.

**Mechanism:** Pre-compute a per-pixel importance weight (Sobel/Laplacian magnitude as proxy, or run a lightweight saliency CNN). Modify cluster fit's `color_distance()` to multiply each pixel's contribution by `weight[pixel]`.

**Expected gain:** *negative* on overall PSNR (we deliberately under-fit unimportant regions). *Positive* on perceived quality — essentially the same idea as JPEG's quantization tables for low-frequency vs high-frequency DCT coefficients.

**Decision:** interesting but hard to evaluate quantitatively. Would need a perceptual study. Park for now.

## Idea #8 — Variable-precision base color via 2-pass refinement

**Motivation:** ETC1 allows individual mode (RGB444, color4) and differential mode (RGB555 + ±3-bit delta, color5). Currently cluster fit picks one and commits. But the optimal mode might *change* if the search radius differs per mode.

**Mechanism:** Run cluster fit twice per (flip, diff): once with base-color-search-radius = 0 (current), once with radius = 2 (broader, slower). Pick best of all 4 (flip, diff) × 2 (radius) = 8 candidates.

**Expected gain:** small (+0.05 dB) but stacks with #4. Already 50% covered by `try-all-corners` patch.

**Decision:** Skip — `try-all-corners` measurement showed this category of "search broader" doesn't help at Uber.

## Idea #9 — Two-stage encode with content-aware preprocessing

**Motivation:** The `all.png` failure mode is structural — it's a system grid with sub-pixel text and icon edges. ETC1 fundamentally can't represent that at native resolution.

**Mechanism:** Detect such regions (high Laplacian magnitude). For those regions only, apply a **gentle blur or super-resolution-aware downsample** *before* encoding. The encoded ETC1 + GPU bilinear upsample at sample time may visually outperform encoding the original.

**Expected gain on `all.png`-like content:** +2 to +4 dB (we'd be encoding a representable signal instead of fighting unrepresentable HF).

**Risk:** It changes the original image. User has to opt in. Not a "drop-in encoder improvement" — it's an authoring pipeline change.

**Decision:** worth offering as an `--auto-preprocess` flag. ~1 day to implement.

## Idea #10 — Joint sub-block iteration (Lloyd-style fixed point)

**Already in audit, already deprioritized.** Per-block analysis showed diff/individual-mode choice contributes <0.05 dB. Skip.

---

# Revised priority list

Ranked by `expected_quality_gain / implementation_cost` (quality-only, time irrelevant):

| Rank | Idea | Expected Y-PSNR / SSIM gain | Cost | Notes |
|---:|---|---|---|---|
| **1** | **#4 Importance-driven worst-N re-encoding** | **+0.4 to +0.7 dB** | 0.5 day | Direct exploitation of B3 finding |
| **2** | **#2 MS-SSIM final selection between top-K** | **+0.10 to +0.25 dB** | 1 day | Stacks with #1 |
| 3 | #5 Per-block multi-table search | +0.05 to +0.15 dB | 0.5 day on top of #4 | Stacks with #1, #2 |
| 4 | #9 Auto preprocessing for HF regions | +2 to +4 dB on grid-like images, 0 elsewhere | 1 day | Authoring-pipeline change |
| 5 | #1 Block-boundary aware joint encoding | +0.05 to +0.15 dB SSIM_Y | 2 days | Lower payoff than I claimed earlier |
| 6 | #3 Differentiable post-pass | +0.0 to +0.2 dB | 3–5 days | High risk, unclear payoff |
| 7 | #6 Selector RDO across image | 0% (quality), 20–50% file size | 2–4 weeks | Only if LZ-compressed shipping |
| skip | #7 Saliency-weighted MSE | unmeasured | unclear | Park |
| skip | #8 Variable-precision search | already covered by try-all-corners | done | |
| skip | #10 Joint sub-block iteration | <0.05 dB | already in audit | |

# Recommended action

**Implement #4 first.** It's the most leveraged idea on this test set (B3 showed top 10% of blocks own 60–87% of total error). Half a day of work. If we get the projected +0.4 to +0.7 dB on MIX1, MIX5, the gamelist images, we've moved the needle materially.

Then layer #2 on top. The two together give roughly +0.5 to +0.9 dB realistically, which is the kind of move that's worth shipping.

**Don't do #1 (block-boundary aware) or #3 (differentiable) until #4 and #2 are exhausted.** The expected gain is too small relative to engineering cost given what B1/B3 measured.
