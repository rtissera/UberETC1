# UberETC1 — resumable plan

Status snapshot, decisions made, what's next. Picked up tomorrow on the
`research` branch.

## Where we are

**Production SOTA (main branch):** `basisu_v3_corners_perc`
- Patched `basis_universal` cluster fit + perceptual YCbCr metric (the
  existing one in basisu, NOT YCoCg) at Uber quality on every block.
- Mean across 6 1920×1080 themed-bg test images:
  - PSNR_RGB **37.69**
  - PSNR_Y **43.51**
  - SSIM_Y **0.9920**
  - 6.31 s/img on 32 threads
- Bitstream cross-validated bit-exactly against AMD Radeon 780M GPU decoder
  (mean_abs_diff = 0.0 across every (image, encoder) pair).

**Research branch:** active. Latest commit on top is the deep-dive analysis
that re-prioritized everything.

## What was ruled out

| Encoder / idea | Result | Notes |
|---|---|---|
| ETCPACK -s slow | Removed | Too slow, non-OSI license, no quality win |
| etcpak | Kept as floor | 3.5 dB below SOTA, speed-first only |
| v4 adaptive per-block effort | -0.57 dB Y-PSNR | Speed/quality trade, not Pareto |
| v4 YCoCg metric | -0.15 dB | YCoCg's Y differs from BT.601 luma we measure |
| v4 Floyd-Steinberg dither preprocess | -5.2 dB | Cluster fit then optimizes for dithered pixels but PSNR is vs original — solving the wrong problem |
| try-all-corners patch (main) | no-op at Uber+perceptual | Consider reverting |

## The empirical findings driving the next phase

Measured on `basisu_v3_corners_perc`'s residual error across all 6 images:

1. **B1 — Border vs interior MSE.** Border-pixel MSE is only 4–12% higher
   than interior. Block-boundary aware encoding has a low ceiling.
2. **B2 — Adjacent-block error correlation 0.4–0.6.** P(both blocks in
   top 10% of error) is 5–6× the random baseline. Hard regions span
   multiple blocks but per-block decisions aren't bad.
3. **B3 (the big one) — Top 10% of blocks contribute 60–87% of total
   error.** On `gamelist_sonic2`, the top 1% (~1,300 blocks of 130k)
   produce **44% of all error**. Cluster fit at Uber spends the same
   effort on every block — this is gross misallocation.

## Plan for tomorrow (priority-ordered)

### 1. Idea #4 — Importance-driven worst-N re-encoding **[start here]**
- Highest leverage: directly exploits B3.
- Estimated gain: **+0.4 to +0.7 dB Y-PSNR** on natural content.
- Cost: ~0.5 day.

**Mechanism:**
1. Pass 1 = current Uber cluster fit, record per-block MSE.
2. Identify worst 5% blocks (`np.percentile(block_err, 95)` threshold).
3. Pass 2: re-encode each worst block with an *expanded search*:
   - All 165 cluster-fit selector distributions (already done at Uber)
   - All 8 intensity tables enumerated
   - Base-color search cube ±2 per channel = 125 quantization candidates
   - Both flip orientations, both diff/individual modes
   - = 4 × 8 × 165 × 125 ≈ 660,000 candidates per worst block
4. Take the lowest-error result for those blocks; leave the easy 95% alone.

**Where to land it:** `encoders/basisu_etc1_tool_v4.cpp`. Extend
`encode_block_full_etc1` to take a `search_radius` and `enumerate_all_tables`
pair of params. Add a 2-pass main loop in `main()`.

**Concrete math** (MIX1, current MSE_Y = 2.51):
- Top-5%-block error contribution ≈ 44% of total = 1.10
- If wider search recovers 30% on those: new MSE_Y ≈ 2.18
- PSNR_Y goes from 44.13 → 44.74 = **+0.61 dB on this image**

**Risk:** low. Worst case: no gain, encode time goes up ~7×. Acceptable.

### 2. Idea #2 — MS-SSIM top-K rerank (stacks with #4)
- Estimated gain: +0.10 to +0.25 dB SSIM_Y on top of #4.
- Cost: 1 day.

**Mechanism:**
1. Modify `basisu_etc.cpp::etc1_optimizer` to maintain a top-K=8 heap of
   `potential_solution` instead of just `m_best_solution`. Look at
   `evaluate_solution_slow()` line 1118 for the insertion point.
2. After pass 1 (#4 above) produces one decoded image, sweep over the
   worst-N% blocks again. For each, evaluate MS-SSIM of each top-K
   candidate vs original on a 3×3-block window (12×12 pixels) that
   includes the already-encoded neighbors.
3. Pick the candidate that maximizes MS-SSIM; bitstream-replace.

**Caveat:** SSIM is non-local; this introduces order dependency. Mitigation:
do the rerank in raster order, using already-reranked neighbors as context.

### 3. Idea #5 — Per-block multi-table search on worst-N
- Stacks with #4. +0.05 to +0.15 dB.
- Cost: 0.5 day on top of #4 (the table enumeration is already in #4 above).

### 4. Idea #9 — Auto-preprocess for HF regions
- Big payoff (+2 to +4 dB) but only on grid-like content like `all.png`.
- 0 dB on natural content.
- Authoring pipeline change. Gate behind `--auto-preprocess` flag.
- Cost: 1 day.

**Mechanism:** detect high-Laplacian regions, apply gentle blur or sharper
downsample before encoding so ETC1 has a representable signal.

### Deprioritized
- **#1 Block-boundary aware joint encoding.** B1 caps the gain at ~0.15 dB
  SSIM_Y. Not worth 2 days when we have +0.5–0.7 dB available from #4.
- **#3 Differentiable post-pass.** NTBC paper (https://arxiv.org/html/2407.09543v2)
  actually reports *worse* PSNR than traditional BC1 (their goal was
  supercompression, not quality). 3–5 days, high risk, low payoff.
- **#7 Saliency-weighted MSE.** Subjective; needs perceptual study.
- **#8 Variable-precision base color.** Already covered by try-all-corners.
- **#10 Joint sub-block iteration.** Audit showed <0.05 dB potential.

### Open question to resolve before any RDO work
**Is the production deployment LZ-compressed?** If yes (typical Android
.ktx2 + Zstd), Idea **#6 selector-bit RDO** becomes the highest *file-size*
lever (20–50% shrink at near-equal quality, per `bc7enc_rdo` on BC formats).
Different axis from PSNR but might matter more for shipping. Cost: 2–4 weeks
because we'd need to port bc7enc_rdo's RDO machinery to ETC1. **Ask the team
before scheduling this.**

## Files to know about

| File | What it is |
|---|---|
| `RESEARCH_REPORT.md` | Full encoder survey + algorithmic catalog + §5.2 enhancement roadmap |
| `RESULTS.md` | Main-branch benchmark numbers |
| `RESULTS_RESEARCH.md` | Research-branch v4 results (the three failed ideas) |
| `DEEP_DIVE.md` | Empirical analysis + 7 new ideas + revised priority list |
| `PLAN.md` | This file (resumable plan) |
| `bench.py` | Encode → decode → PSNR/SSIM pipeline |
| `gpu_validate.py` | EGL+GLES2 GPU-decode cross-check |
| `encoders/basisu_etc1_tool_v4.cpp` | Research driver — where #4 lands |
| `encoders/gl_decode.c` | GPU validator |
| `patches/0001-try-all-corners.patch` | basis_universal modification (probably revert) |
| `audit_basisu.{sh,txt}` etc | Feature audits per encoder |

## Quick resume commands

```bash
cd /home/romain/etc1_bench
git status
git log --oneline | head -5
cat PLAN.md DEEP_DIVE.md           # what we're doing and why
ls -la build/basisu_etc1_tool_v4*  # encoder binaries already built

# Concrete starting point for Idea #4:
$EDITOR encoders/basisu_etc1_tool_v4.cpp
# Extend encode_block_full_etc1() to take (search_radius, enumerate_all_tables)
# Add 2-pass main() that does normal encode, finds worst 5%, re-encodes them.
# Rebuild and re-run bench.py with v4_worstN as a new variant key.
```

## Test-set images (under test_images/, ignored by git)

- `MIX1.png`, `MIX5.png` — Batocera Pulse artistic backgrounds
- `gamelist_doom.png`, `gamelist_sonic2.png`, `gamelist_final_fantasy_vii.png` — es-theme-next-slide
- `all.png` — es-theme-next-pixel system grid (the structural-failure case)
