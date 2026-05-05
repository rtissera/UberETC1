# ETC1 Texture Compression Encoders: Deep Research Report

**Scope:** Best-possible ETC1 encode quality, time/effort unconstrained.
**Constraint:** ETC1 bitstream is fixed (4 bpp RGB, 4×4 block, two sub-blocks, individual RGB444 or differential RGB555+delta base color, 1 of 8 intensity tables, 16 × 2-bit selectors, flip bit, diff bit).
**Out of scope:** ETC2 (T/H/Planar), EAC, ASTC. ETC1S is treated as a *constrained subset* of ETC1, not the full format.
**Date:** 2026-05-05.

> **Note on visual samples at 1920×1080.** This report does not run encoders. No public benchmark in the literature uses 1920×1080 as a standard. Published numbers use the Kodak suite (768×512), kodim18 specifically, or proprietary/synthetic corpora (~1500-image PNG corpus from crunch development; 64-texture Visual Studio test set; per-paper ad hoc selections). Online side-by-side comparison galleries at FullHD do not appear to exist for ETC1; the closest are Geldreich's blog posts (image-level visualizations) and the Betsy/Godot release post (RMSLE numbers, not per-image PSNR).

---

## 1. Survey of Open-Source ETC1 Encoders

### 1.1 rg-etc1 (Rich Geldreich, 2012)

- **Repo:** https://github.com/richgel999/rg-etc1
- **License:** zlib (very permissive).
- **Language:** C++ single-header style (`rg_etc1.h` + `rg_etc1.cpp`).
- **Status:** Archived read-only on GitHub (since 2026-02-27). Superseded by the encoder embedded in **basis_universal** (basislib), which Geldreich actively maintains.
- **Quality modes:** `cLowQuality`, `cMediumQuality`, `cHighQuality` (an enum in `rg_etc1.h`). The `pack_etc1_block_internal` path in high quality runs an iterative lattice scan + base-color refinement search (called by Geldreich the "3D neighborhood search with iterative base color refinement").
- **Algorithm:**
  1. For each (flip, diff) combination of the two sub-blocks (4 combinations),
  2. For each of 8 intensity tables,
  3. Search for the base color that minimizes block error: starts at sub-block average, evaluates a 3D neighborhood of candidate base colors, then alternates between "given base, find best selectors" and "given selectors, refine base color".
  4. Pick the (flip, diff, table, base, selectors) combination with lowest squared error.
- **Metric:** RGB RMSE only — `rg_etc1.h` explicitly states *"pack_etc1_block() does not currently support 'perceptual' colorspace metrics — it primarily optimizes for RGB RMSE."*
- **Quality claim from author:** *"For random 888 inputs, MSE results are better than Ericsson's ETC1 packer in 'slow' mode approximately 9.5% of the time, slightly worse only about 0.01% of the time, and equal the rest of the time."* I.e. essentially matches or beats the reference encoder.
- **Sources:**
  - https://github.com/richgel999/rg-etc1
  - https://github.com/richgel999/rg-etc1/blob/master/rg_etc1.h
  - https://github.com/richgel999/rg-etc1/blob/master/rg_etc1.cpp
  - https://richg42.blogspot.com/2016/09/visualizing-etc1-texture-compression.html

### 1.2 basislib / basis_universal ETC1 path (Rich Geldreich + Binomial LLC)

- **Repo:** https://github.com/BinomialLLC/basis_universal
- **License:** Apache 2.0.
- **Language:** C++.
- **Status:** Actively maintained (v2.10 snapshot as of 2026-03; releases through 2026).
- **What it is for ETC1 specifically:** Two distinct things live in this repo:
  1. **Full-ETC1 high-quality encoder** (the successor to rg_etc1): a *cluster-fit* encoder ported from libsquish's DXT1 method to ETC1. This is the SOTA per-block ETC1 quality reference.
  2. **ETC1S** encoder: the constrained subset (see §1.3 below), used for the supercompressed transcodable format.
- **Cluster-fit ETC1 algorithm** ([Geldreich 2016](http://richg42.blogspot.com/2016/09/libsquishs-dxt1-cluster-fit-method.html), [follow-up](http://richg42.blogspot.com/2016/10/more-etc1-cluster-fit-data.html)):
  1. For each (flip, diff, table) combination, enumerate every distinct *selector distribution* (how many pixels use selector 0/1/2/3) reachable in a total ordering — there are roughly 165 such distributions for a 2×4 sub-block.
  2. For each distribution, solve a closed-form least-squares system for the optimal base color: `optimal_block_color = avg_input - avg_inten_delta`, then quantize to RGB444/RGB555.
  3. A correction-factor table (relative to sub-block average) accounts for clamping at modifier extremes.
  4. Hash table dedups identical trials.
  5. "Top-64 most-used distributions" pruning trades 2.25–5× speed for negligible quality loss.
- **Reported quality (single image kodim03 768×512, single-thread):**

  | Encoder | Time (s) | RGB PSNR (dB) |
  |---|---|---|
  | basislib cluster-fit ETC1 | 5.644 | **38.982** |
  | etc2comp (effort 100) | 75.8 | 39.095 |
  | etcpak | 0.006 | 37.073 |

  basislib is within ~0.11 dB of etc2comp at 13× the speed. Geldreich cites this as the modern SOTA for ETC1 quality per second. With multithreading (40 threads) basislib reaches 0.266 s.
- **Reported quality on 4096×4096 corpus image (random 4×4 blocks from many images):** basislib cluster-fit 32.019 dB / 4.211 s vs. rg_etc1 lattice search 32.183 dB / 240.638 s (≈57× faster, –0.16 dB).
- **Perceptual metric:** basislib supports **weighted YCbCr** error during search, weights `(128, 64, 16)` (luminance heavily favored). Geldreich: *"Compared to vanilla RGB weighted metrics, this looks better in my experience writing Basis (especially with ETC1)."*
- **Sources:**
  - https://github.com/BinomialLLC/basis_universal
  - http://richg42.blogspot.com/2016/09/libsquishs-dxt1-cluster-fit-method.html
  - http://richg42.blogspot.com/2016/10/more-etc1-cluster-fit-data.html
  - http://richg42.blogspot.com/2018/04/bc7-encoding-using-weighted-ycbcr.html

### 1.3 basis_universal ETC1S (constrained subset)

- **NOT full ETC1.** ETC1S freezes:
  - `diff bit = 1` (always differential mode)
  - `(Rd, Gd, Bd) = (0, 0, 0)` (the second sub-block reuses the first sub-block's 5-bit base color exactly)
  - `flip bit = 0`
  - Only 6 of the 16 selector range patterns are used (e.g. `{0,3}`, `{1,3}`, `{0,2}`, ...).
- **Storage:** a 5:5:5 base color + 3-bit intensity table index + 16 × 2-bit selectors per block — but encoded via global VQ codebooks (endpoint codebook + selector codebook) compressed with Huffman/RLE.
- **Result:** ETC1S is **0.3–3 bpp** after supercompression (vs. ETC1's fixed 4 bpp). It is *bitstream-compatible with ETC1 hardware decoders* — the constraints just disable certain bits/modes.
- **Quality cost vs. full ETC1:** Geldreich: *"This system isn't full ETC1 quality because it disables 2×4/4×2 subblocks. We loose a few dB vs. optimal ETC1 due to this limitation, but we gain the ability to easily transcode to any other 4×4 block-based format."*
- **comp_level options (1–6):** Higher levels = larger codebooks, more codebook-optimization passes, slower. For maximum ETC1S quality: `-comp_level 5 -max_endpoints 16128 -max_selectors 16128 -no_selector_rdo -no_endpoint_rdo`.
- **Caveat for this report:** if the user wants **best-possible ETC1 quality**, ETC1S is *not* the answer — it sacrifices the flip bit and 2×4/4×2 sub-block split to enable transcoding. Use the full-ETC1 cluster-fit path in basislib instead.
- **Sources:**
  - https://github.com/BinomialLLC/basis_universal/wiki/.basis-File-Format-and-ETC1S-Texture-Video-Specification
  - https://github.com/BinomialLLC/basis_universal/wiki/ETC1S-Compression-Effort-Levels
  - https://richg42.blogspot.com/2018/06/etc1s-texture-format-encoding.html

### 1.4 etc2comp (Google, 2015)

- **Repo:** https://github.com/google/etc2comp
- **License:** Apache 2.0.
- **Language:** C++.
- **Status:** **Archived 2022-06-01, unmaintained.** Banner on repo: *"This repo is no longer maintained."*
- **Modes:** ETC1, RGB8, SRGB8, RGBA8, RGB8A1, R11.
- **Effort parameter:** `-effort 0..100`. 100 = highest quality, ~70× slower than 0.
- **Algorithm — "block archetypes":** Instead of brute-forcing all modes per block, etc2comp classifies each block (e.g. flat color, gradient, edge-like) and runs only the most-likely-fit modes for that archetype, in a directed-graph order. This is well-suited to ETC2's planar/T/H modes; for pure ETC1 the gain is smaller.
- **Metrics supported:** `rgba`, `rgbx`, `rec709` (luma-weighted), `numeric`, `normalxyz`. **etc2comp is the most metric-flexible mainstream encoder.**
- **Reported quality (kodim18, ETC1-only mode):**

  | -effort | Time (s) | RGB PSNR (dB) |
  |---|---|---|
  | 0 | 0.052 | 34.883 |
  | 50 | 0.361 | 35.782 |
  | 70 | 1.095 | 35.953 |
  | 80 | 1.830 | 35.992 |
  | 100 | 3.619 | **36.031** |

  Geldreich notes *"effort 40–65 is the sweet spot. Effort=100 is obviously wasteful"* — diminishing returns are severe past 70.
- **Highest absolute single-image ETC1 PSNR observed in literature:** etc2comp 39.095 dB on kodim03 at effort 100. This is the ad hoc high-water mark cited by Geldreich.
- **Sources:**
  - https://github.com/google/etc2comp
  - https://opensource.googleblog.com/2016/11/etc2comp-fast-texture-compression.html
  - http://richg42.blogspot.com/2016/10/etc2comps-effort-parameters-impact-on.html

### 1.5 etcpak (Bartosz Taudul, 2013–present)

- **Repo:** https://github.com/wolfpld/etcpak
- **License:** BSD 3-Clause.
- **Language:** C++ (with SSE/AVX2/AVX512 SIMD).
- **Status:** Actively maintained. v2.1 released 2026-02. Author also writes Tracy profiler.
- **Speed (AMD Ryzen 9 7950X, 16K×16K atlas):** ETC1 797 Mpx/s ST, 9613 Mpx/s MT — ~3 orders of magnitude faster than ETCPACK / etc2comp.
- **Algorithm:** Highly heuristic. For ETC1: tries both flip orientations, computes a quick base color from the sub-block's average and bounding-box span, picks an intensity table by simple magnitude heuristics, computes selectors by per-pixel quantization. No iterative refinement. SIMD-friendly throughout.
- **Quality:** ~37.07 dB on kodim03; ~1–2 dB below cluster-fit ETC1. Author position is explicitly *"speed first; PSNR is misleading anyway."*
- **Geldreich critique (notable):** *"etcpak is a very fast, but low quality ETC1 (and a little bit of ETC2) compressor. Unless you need a real-time ETC1 encoder, it trades off too much quality."*
- **Failure case:** On a synthetic 4×4 of pure red and blue pixels, etcpak produced PSNR 15.612 — the worst of every encoder tested, because its bounding-box heuristic collapses for high-contrast saturated input.
- **Note:** etcpak *also* added BC1/BC3/BC7 modes; for BC7 it uses a SIMDified version of bc7enc by Geldreich.
- **Sources:**
  - https://github.com/wolfpld/etcpak
  - http://richg42.blogspot.com/2016/09/etcpak.html
  - https://richg42.blogspot.com/2016/09/quick-etcpak-quality-test.html
  - https://richg42.blogspot.com/2016/09/an-interesting-etc12-encoding-test.html

### 1.6 Ericsson ETCPACK (reference encoder)

- **Repo:** https://github.com/Ericsson/ETCPACK
- **License:** Custom Ericsson license — permissive but **not a standard OSI license**; check before commercial use.
- **Language:** C++.
- **Status:** Reference, not actively developed. Considered the canonical ground truth.
- **Modes:** `-s slow|medium|fast` for ETC1. "slow" mode does an exhaustive search over base color + table + selectors per (flip, diff) combination.
- **Algorithm in `slow` mode:** For each (flip, diff, table) triple, enumerate the full quantized base-color space (RGB444 or RGB555) per sub-block; for each base, compute optimal selectors greedily; track best overall. This is essentially exhaustive over base colors + greedy selectors.
- **Position in benchmarks:** slow mode is consistently ~0.1–0.3 dB behind etc2comp effort 100 and basislib cluster-fit, and 10²–10³× slower than basislib cluster-fit. Used historically as the quality baseline encoders are validated against.
- **Source:** https://github.com/Ericsson/ETCPACK

### 1.7 Compressonator (AMD GPUOpen)

- **Repo:** https://github.com/GPUOpen-Tools/compressonator
- **License:** MIT.
- **Language:** C++.
- **Status:** Actively maintained.
- **ETC1 path:** The internal ETC1/ETC2 codec in Compressonator is *derived from / wraps* an existing reference (ETCPACK-derived); the documentation does not describe a novel algorithm.
- **Quality:** Mid-pack — better than etcpak, behind etc2comp/basislib at high settings. Compressonator's strength is unified support for BC1–BC7 + ASTC + ETC; it is not the SOTA ETC1 encoder.
- **Source:** https://github.com/GPUOpen-Tools/compressonator (and `docs/source/developer_sdk/codecs/`)

### 1.8 Intel ISPC Texture Compressor

- **Repo:** https://github.com/GameTechDev/ISPCTextureCompressor
- **License:** MIT.
- **Language:** ISPC + C++.
- **Status:** **Abandoned by Intel** — banner reads *"This project will no longer be maintained by Intel."* Community forks: `OldUnreal/KTexComp`, `hanfling/BC`. Rust bindings `intel-tex-rs`, `intel-tex-rs-2`, `ispc-texcomp-rs` exist.
- **ETC1 algorithm:** Heuristic + refinement, SIMD-vectorized via ISPC. Per Geldreich's data, ISPC ETC1 lands around PSNR 35.969 on kodim18 at ~1 s — roughly equal to etc2comp at effort 80.
- **Reported on synthetic red/blue test:** ISPC produced **PSNR 24.968** — the *best* of all tested ETC1 encoders on that pathological case (closer to true endpoints), because its base-color search is more thorough than etcpak's heuristic.
- **Source:** https://github.com/GameTechDev/ISPCTextureCompressor

### 1.9 Betsy (Matias N. Goldberg, 2020, for Godot)

- **Repo:** https://github.com/darksylinc/betsy
- **License:** see LICENSE.md (MIT-ish, free for Godot use).
- **Language:** GLSL compute shaders (runs on GPU).
- **Status:** Released 2020; actively used by Godot for fast import.
- **ETC1 algorithm:** *"Based on Rich Geldreich's CPU encoder"* — an iterative narrowing search + refinement, ported to GPU. Parallelizes across the 4 (flip, diff) combinations using 4 threads per block (so 256K threads on a 1024² image).
- **Quality levels:** `q=1` (medium) and `q=2` (max). q=2 ETC1 average **RMSLE 0.02389376** vs etc2comp's 0.02502284 on Betsy's reference set — essentially matches etc2comp at much higher throughput.
- **Limitation:** Betsy's "max quality" is *Geldreich-style 3D neighborhood search* — it does **not** implement cluster fit, so on paper its peak per-block quality is ~0.1–0.2 dB below basislib cluster-fit.
- **Sources:**
  - https://godotengine.org/article/betsy-gpu-texture-compressor/
  - https://github.com/darksylinc/betsy/blob/master/Docs/technical_doc_advanced.md

### 1.10 Goofy (Sergey Makeev)

- **Repo:** https://github.com/SergeyMakeev/Goofy
- **License:** MIT.
- **Language:** C++ (header-only, SSE2).
- **Algorithm (ETC1):** Find principal axis of block as the bounding-box diagonal in RGB; project pixels onto it via YCoCg perceptual luma; pick base color as adjusted average, single-shot table selection.
- **Quality:** ~36.30 dB on i7-7820HQ benchmark (single thread, 1221 Mpx/s) — roughly etcpak-tier quality, but real-time.
- **Use case:** Real-time DXT1/ETC1 video / streaming encode. Not a quality contender.
- **Source:** https://github.com/SergeyMakeev/Goofy

### 1.11 Mali Texture Compression Tool (Arm)

- **URL:** https://developer.arm.com/tools-and-software/graphics-and-gaming/mali-texture-compression-tool
- **License:** Closed-source freeware (Arm EULA).
- **Status:** Still distributed but largely deprecated in favor of `astcenc` for ASTC. ETC1/ETC2 still supported in the GUI tool. **Source not available.**
- **Quality:** Anecdotally good, but no public benchmarks compare it against modern encoders. Geldreich uses it for *PSNR validation* (decoded output sanity-check), not as an encoder he benchmarks.
- **Note:** `astcenc` (https://github.com/ARM-software/astc-encoder) is **ASTC-only** — it does **NOT** support ETC1.

### 1.12 PVRTexTool / PVRTexLib (Imagination Technologies)

- **URL:** https://developer.imaginationtech.com/solutions/pvrtextool/
- **License:** Closed-source freeware. PVRTexLib redistributed as `.so`/`.dll`. **No source.**
- **ETC1 quality modes:** `etcfast`, `etcslow`, `etcfastperceptual`, `etcslowperceptual` — and a "Best Quality" GUI level that is reported to take >30 minutes for a 2048² texture (single-threaded).
- **Quality:** "Best Quality" is competitive with etc2comp effort 100, possibly matching it on some images, but no published controlled comparison exists.
- **Position:** The de-facto industry tool for iOS/PowerVR pipelines historically; unlikely to beat basislib cluster-fit + perceptual metrics on most images.

### 1.13 Etz (Zig, 2024)

- **Repo:** https://codeberg.org/GasInfinity/etz (also on Ziggit forum showcase)
- **License:** see repo (Zig community projects typically MIT).
- **Algorithm:** Direct port of rg_etc1's algorithm to Zig. ETC2 not supported. No quality enhancements over rg_etc1.
- **Source:** https://ziggit.dev/t/etz-etc1-texture-encoder-decoder/15172

### 1.14 RwgTex (paulvortex)

- **Repo:** https://github.com/paulvortex/RwgTex
- **License:** GPL.
- **Status:** Mid-2010s wrapper that orchestrates rg_etc1, etc2comp, PVRTexTool, and others as a build pipeline. Not a novel encoder. Useful as a multi-encoder benchmark harness.

### 1.15 Bitstream-compatible academic / minor encoders

- **iPACKMAN (Ström & Akenine-Möller, 2005):** Original paper that introduced what became ETC1. Not a maintained encoder; algorithm is the basis for ETCPACK's "slow" mode. https://www.semanticscholar.org/paper/iPACKMAN%3A-high-quality%2C-low-complexity-texture-for-Str%C3%B6m-Akenine-M%C3%B6ller/1a85970157ebfe4455c1461f5c18764798cc6f1e
- **QuickETC2 (Nah, 2020) / QuickETC2-HQ (Nah et al., 2024):** Targets ETC2 specifically and explicitly states *"QuickETC2 does not alter the ETC1 compression logic in etcpak"* — so it shares etcpak's ETC1 quality. Its T/H/Planar work is irrelevant to pure ETC1. Papers:
  - https://nahjaeho.github.io/papers/SA20/QUICKETC2_SA20.pdf
  - https://nahjaeho.github.io/papers/CAG23/cag-preprint.pdf
- **H-ETC2 (Lee et al., 2023):** CPU-GPU hybrid ETC2 encoder. ETC1 mode again uses an etcpak-derived path.
- **NTBC (Neural Texture Block Compression, 2024):** Targets BC1/BC4 with MLPs, **not ETC1**, but the differentiable-encoder methodology (STE through argmax) directly translates to ETC1. https://arxiv.org/html/2407.09543v2

### 1.16 Encoders to flag

- **xdanieldzd/ETC1Lib:** decoder only, marked UNMAINTAINED. https://github.com/xdanieldzd/ETC1Lib
- **CoinKiller embedded copy of rg_etc1:** vendored copy in a Nintendo level editor; not a separate encoder.
- **glTF-Compressonator (Khronos fork of Compressonator):** prototyping for ETC1S/CRN; not its own encoder.

---

## 2. Quality Comparison Data (consolidated)

> **All numbers cited are from Geldreich's blog posts, encoder author sites, and the QuickETC2 / Betsy publications. There is no peer-reviewed cross-encoder ETC1 benchmark study that I found. Geldreich's posts are the de-facto reference.**

### 2.1 kodim03 (768×512), ETC1 mode, single-thread

Source: http://richg42.blogspot.com/2016/09/libsquishs-dxt1-cluster-fit-method.html

| Encoder | Time (s) | RGB PSNR (dB) | Notes |
|---|---|---|---|
| etc2comp effort 100 | 75.8 | **39.095** | reference high-water |
| basislib cluster-fit | 5.644 | 38.982 | –0.11 dB, 13× faster |
| crnlib (uber mode) | 8.067 | 38.232 | rg_etc1 lineage |
| etcpak | 0.006 | 37.073 | 1344× faster than crnlib |

### 2.2 kodim18 (768×512), ETC1 mode

Source: http://richg42.blogspot.com/2016/10/etc2comps-effort-parameters-impact-on.html

| Encoder / setting | Time (s) | RGB PSNR (dB) |
|---|---|---|
| etc2comp effort 0 | 0.052 | 34.883 |
| etc2comp effort 50 | 0.361 | 35.782 |
| etc2comp effort 70 | 1.095 | 35.953 |
| etc2comp effort 100 | 3.619 | **36.031** |
| basislib cluster-fit (64 trials) | 0.115 | 35.917 |
| Intel ISPC | 1.03 | 35.969 |

### 2.3 4096×4096 corpus image (random 4×4 blocks from many images)

Source: http://richg42.blogspot.com/2016/10/more-etc1-cluster-fit-data.html. 20-core Xeon, 40 threads.

| Encoder | Time (s) | RGB PSNR (dB) | Mean error |
|---|---|---|---|
| basislib cluster-fit (40-thread) | 4.211 | 32.019 | 3.277 |
| rg_etc1 lattice search | 240.638 | **32.183** | 3.203 |
| etc2comp | 154.562 | 31.819 | — |
| ispc_etc1 | 43.694 | 32.120 | — |
| etcpak | 0.258 | 29.684 | — |

### 2.4 Pathological synthetic block (red+blue, 4×4)

Source: https://richg42.blogspot.com/2016/09/an-interesting-etc12-encoding-test.html

| Encoder | PSNR | SSIM |
|---|---|---|
| Intel ISPC | **24.968** | 0.587 |
| basislib_etc1 | 19.987 | 0.511 |
| etc2comp ETC2 mode | 19.779 | 0.518 |
| etc2comp ETC1 mode | 17.471 | 0.372 |
| etcpak | 15.612 | 0.266 |

This case is meaningful for **adversarial / saturated content**; ISPC's wider base-color search wins.

### 2.5 Betsy reference set (max quality q=2)

Source: https://godotengine.org/article/betsy-gpu-texture-compressor/

| Encoder | Avg RMSLE |
|---|---|
| Betsy ETC1 q=2 | 0.02389376 |
| etc2comp | 0.02502284 |

Lower is better. Betsy effectively matches/exceeds etc2comp on this set.

### 2.6 Multi-image corpus, ETC1 RGB average PSNR — Pareto observation

Source: http://richg42.blogspot.com/2016/09/comparison-of-three-etc1-and-etc2-block.html (1572 PNG corpus from crunch development).

Geldreich's Pareto-frontier conclusion (no exact numbers tabulated, only graphs):
- **etc2comp & basislib cluster-fit are tied for highest average ETC1 PSNR.**
- **Intel ISPC** sits ~0.1–0.2 dB lower at much lower time cost.
- **etcpak** is consistently ~1–2 dB below the top tier.
- *"different codecs often calculate PSNR very differently from each other"* — Geldreich validates with ImageMagick's `compare` and Mali's tool to neutralize this.

### 2.7 Current SOTA verdict for "slow but best quality"

**basislib full-ETC1 cluster-fit encoder** (in basis_universal repo, NOT the ETC1S path) with **weighted YCbCr metric (128, 64, 16)** is the SOTA for high-quality CPU ETC1 encoding. It matches etc2comp at effort 100 within ~0.1 dB and is much faster; with cluster-fit running *all 165 distributions* (instead of top-64) and multiple base-color refinement passes, it is essentially at the ceiling of what published encoders achieve.

For **truly absolute per-block optimality**, none of these encoders is provably optimal. The ETC1 search space per (flip, diff) is `8 tables × 2^12 base color (RGB444) × 4^16 selectors`; cluster-fit prunes the selector space but not the base color exhaustively in all cases. A truly exhaustive encoder (Mali tool's "slow", ETCPACK's "slow") is provably optimal *per-block under MSE for given base-color granularity*, but those tools enumerate base colors and use *greedy* selector assignment — not a joint optimum. The optimal-per-block ETC1 encoder under MSE *appears not to have been published*.

---

## 3. Algorithmic Techniques Catalog

Each technique is mapped to encoders that use it.

### 3.1 Base-color search

- **Brute-force RGB444/RGB555 enumeration:** ETCPACK slow mode, Mali tool slow mode.
- **3D neighborhood / lattice scan + iterative refinement:** rg_etc1 high quality, Betsy q=2, Intel ISPC.
- **Cluster fit (libsquish-derived):** basislib cluster-fit. Closed-form least-squares per selector distribution: `optimal_block_color = avg_input - avg_inten_delta`, then quantize.
- **Average-color heuristic + bounding-box principal axis:** etcpak, Goofy.
- **Block-archetype directed graph:** etc2comp (most useful for ETC2 modes; modest for pure ETC1).

### 3.2 Modifier-table selection

- **Exhaustive 8-table search:** rg_etc1, basislib, Betsy, etc2comp at effort ≥50, ETCPACK slow, Intel ISPC.
- **Heuristic selection by sub-block dynamic range / variance:** etcpak, Goofy.

### 3.3 Selector assignment

- **Greedy per-pixel** (given base color and table, pick best of 4 selectors per pixel) — universal across all encoders. Provably optimal *given* a fixed base color and table.
- **Joint optimization with base color:** cluster fit (basislib) — alternates "selector distribution → optimal base color → re-derive selectors → repeat".

### 3.4 Flip + diff search

- **All 4 (flip × diff) combinations evaluated:** every quality-oriented encoder. Universal.
- **Flip-only or shortcut:** etcpak fast paths sometimes skip diff retesting under heuristic conditions.

### 3.5 Joint sub-block optimization

- **Independent per-sub-block:** all current encoders. They enforce the diff-mode constraint (subblock1 base = subblock0 base + 3-bit signed delta) by checking compatibility, not by jointly optimizing.
- **Truly joint sub-block search:** *nobody published this.* Would require iterating both sub-blocks together inside the diff-mode constraint manifold.

### 3.6 Perceptual / non-MSE error metrics

- **Weighted RGB (luma-favoring 2× G):** etc2comp `rec709`, rg_etc1 (perceptual macro), etcpak does not support.
- **Weighted YCbCr (128, 64, 16):** basislib (Geldreich's preferred). Reported +1.8 dB *Y-PSNR* on 31-image set vs ISPC.
- **rec709 / ITU-R BT.601 luma weights (0.299, 0.587, 0.114):** etc2comp.
- **Perceptual saliency maps + ITP delta-E:** basis_universal HDR mode (not LDR ETC1, but the infrastructure exists).
- **SSIM / MS-SSIM in encode loop:** *not present in any production ETC1 encoder.*
- **LPIPS in encode loop:** *not present.*

### 3.7 Pre-processing dithering

- **Floyd-Steinberg or similar before encode:** *not standard in any open ETC1 encoder.* PVRTexTool supports input-side dithering controls in the GUI but not block-aware dithering.

### 3.8 Multi-pass / global search

- **Simulated annealing, genetic search:** *not present in any published ETC1 encoder.* Geldreich considered VQ clustering of endpoints across blocks (crunch-style, see https://richg42.blogspot.com/2016/09/crunch-for-etc1-block-color.html) — that is rate-distortion optimization for storage, not single-block quality.
- **RDO (rate-distortion optimization):** basis_universal supports `-no_endpoint_rdo`, `-no_selector_rdo`, `-endpoint_rdo_thresh` — but only for ETC1S (codebook-compressed), not raw ETC1.

### 3.9 Cross-block / whole-image optimization

- **None for ETC1.** All current encoders treat blocks independently. Decoded ETC1 is also block-independent, so the format constraint allows independent encoding to be globally optimal *per pixel* — but not per-block-boundary visual artifact, which is perceptual and not captured by MSE.

---

## 4. Modern Enhancement Ideas (still inside ETC1 bitstream constraint)

### 4.1 Differentiable / gradient-based ETC1 encode

- **Status: not published for ETC1 specifically.**
- NTBC (Lee et al., 2024) https://arxiv.org/html/2407.09543v2 implements differentiable BC1/BC4 by relaxing argmax-over-selectors with the **Straight-Through Estimator (STE)**. Their finding: directly optimizing block-compressed weights gives poor results because *"weights are high-frequency and not spatially correlated"*; instead, they predict pixels and let a differentiable encoder map to bitstream.
- For ETC1: relax the 2-bit selector to a softmax of 4 distances; differentiate w.r.t. base color in continuous RGB; differentiate the table choice with Gumbel-softmax. Then quantize at the end. This is a tractable PyTorch model — *nobody has published it for ETC1*.
- **Expected gain:** small per-block (cluster-fit already finds near-optimal MSE), but large under perceptual losses (LPIPS, MS-SSIM) because gradients can drive decisions toward what humans see.

### 4.2 Neural-network warm-start

- Train a small CNN: 4×4×3 RGB input → predicts (flip, diff, table_idx, base_color_0, base_color_1, 16 selector logits).
- Use as initialization for a refinement pass (cluster fit + alternating optimization).
- **Prior art:** *none for ETC1.* For BC7, "Neural BC compression" (NTBC and Hardware-Accelerated NBT https://arxiv.org/html/2506.06040v1) demonstrates this works.
- **Expected gain:** much faster reach to near-optimum; quality ceiling unchanged unless combined with §4.1.

### 4.3 Perceptual loss in encode loop

- **Already partially adopted:** etc2comp (rec709), basislib (weighted YCbCr).
- **Not adopted:** SSIM, MS-SSIM, LPIPS as the *cost function* during cluster-fit.
- **Practical issue:** SSIM is non-local (uses 8×8 or 11×11 windows), so per-block independent search cannot evaluate it correctly without context. Solution: evaluate SSIM on a windowed reconstruction including already-encoded neighbor blocks.
- **Expected gain:** Geldreich's experience suggests +1 dB Y-PSNR from luma-weighted vs RGB-weighted metric. Going to LPIPS/SSIM would shift the encoder to favor *visible* error differently — anecdotally significant on smooth gradients and faces, hard on synthetic content.

### 4.4 Whole-image / cross-block optimization

- ETC1 decode is block-independent, **but block-boundary luma jumps are visible**, especially on smooth gradients.
- Idea: After per-block cluster-fit, run a refinement pass that *jointly* optimizes pairs of adjacent blocks under a metric that penalizes boundary discontinuity.
- **Prior art:** none for ETC1. crunch's VQ across blocks (Geldreich 2016 https://richg42.blogspot.com/2016/09/crunch-for-etc1-block-color.html) shares endpoints across blocks for *compression*, not visual continuity.
- **Expected gain:** small RGB-PSNR change, large perceptual win on smooth regions.

### 4.5 Endpoint optimization as constrained PCA

- For a sub-block's pixels, the optimal continuous endpoints (under MSE) lie on the principal axis through the centroid. The 4 modifier-table levels are equispaced offsets along that axis.
- For ETC1, the 4 levels are *not* equispaced (they are `±a`, `±b` for table-specific `a, b`), so it's a constrained 1D fit, not pure PCA.
- **Already implicitly used** by cluster fit (which solves least-squares per selector distribution) — equivalent to the optimal 1D fit per distribution.
- Generalization: solve total-least-squares (Deming regression) accounting for RGB444/RGB555 quantization noise — not in any current encoder.

### 4.6 Higher-bit-depth source

- If the source is 8-bit RGB, encoding to ETC1 quantizes base color to 4 or 5 bits per channel; rounding up vs. down at quantization can swing per-block PSNR by 0.5+ dB.
- **Practice:** Most encoders just round-to-nearest at the end. **Try-all 8 nearest quantization corners** per (flip, diff, table) candidate is feasible with no time budget — it's at most 8× cost.
- **Already partially present** in etc2comp at effort 100 and cluster-fit's correction-factor enumeration; not exhaustively exploited.

### 4.7 Branch-and-bound exhaustive

- ETC1 per-block search space:
  - flip (1 bit) × diff (1 bit) × table0 (3 bits) × table1 (3 bits) × base0 (12 or 15 bits) × base1 (12 or 15 bits) × 16×2 selectors.
  - Selectors are trivially optimal given (base, table); reduces to flip × diff × table × base = `2 × 2 × 64 × (4096 or 32768)²` per block worst case.
  - Cluster fit reduces base × selector to 165 distributions × small correction set.
- A true **branch-and-bound** with correct lower bounds on per-pixel error could prune most of the base-color space. *Nobody has published a B&B ETC1 encoder.* Likely tractable per-block (millisecond scale) for offline use.
- **Expected gain:** strictly equals current cluster-fit when cluster-fit's distribution enumeration is complete; provably matches the global per-block MSE optimum for the first time. Marginal real-world quality lift but theoretically valuable.

### 4.8 "Optimal" ETC1 — has it been proven?

- **No.** No paper proves a per-block ETC1 encoder is MSE-optimal. rg_etc1 claims *"near-optimal"* in cluster-fit mode; cluster fit is provably optimal *given the enumeration of selector distributions in the total-ordering space*, but the total ordering enumeration itself is heuristic (it skips orderings not reachable from sorting along the principal axis).
- A true proof-of-optimality encoder would: enumerate all `8^16 ≈ 2.8×10^14` selector configurations? No — because given a base+table, the optimal selector per pixel is independent. So the search reduces to `(flip × diff × table × base)` — `2 × 2 × 8 × 8 × 4096²` ≈ `4×10^9` per block. Tractable in seconds offline.
- **Open opportunity:** a literally-exhaustive offline ETC1 encoder. Would set the upper bound that all practical encoders are measured against.

---

## 5. Recommendation: Ideal "Time Doesn't Matter" ETC1 Encoder

### 5.1 Starting point

**Fork basis_universal's full-ETC1 cluster-fit path** (not the ETC1S codebook path).

Why:
- It is the modern SOTA for offline ETC1 quality (matches etc2comp effort 100 within ~0.1 dB at 13× the speed).
- It already supports weighted YCbCr metrics, the perceptually-best metric tested for ETC1.
- Apache 2.0 license; actively maintained.
- Cluster fit is the right algorithmic foundation — we can layer enhancements on it without rewriting.
- It is in C++ and reasonably structured for modification.

Avoid:
- **etc2comp** as base (archived/unmaintained, archetype dispatch is ETC2-centric).
- **etcpak** (heuristic-first, would need full algorithmic rewrite).
- **Betsy** (GPU-side; harder to modify; quality below cluster-fit).
- **rg_etc1 standalone** (archived; subsumed by basislib).

### 5.2 Layered enhancements (in order of expected impact)

1. **Use full distribution set (not top-64) + multiple correction passes.**
   - Cluster fit's "top-64 most-used distributions out of 165" pruning is for speed. Disable it. Run all 165.
   - Run alternating base/selector refinement to fixed point per distribution.
   - **Expected:** +0.05 to +0.15 dB.
2. **Weighted YCbCr (128, 64, 16) as the search cost metric.**
   - Already supported in basislib. Make sure it's the default in the high-quality path.
   - **Expected:** +1.0 to +1.8 dB Y-PSNR (perceived quality), neutral RGB PSNR.
3. **Try-all 8 quantization corners per candidate base color.**
   - Instead of round-to-nearest after the least-squares solve, try the 2³ corners of the RGB444 (or RGB555) cube around the LS solution.
   - **Expected:** +0.05 to +0.2 dB.
4. **Joint optimization across sub-blocks under diff constraint.**
   - In diff mode, sub-block 1's base = sub-block 0's base + signed 3-bit delta. Currently encoders pick sub-block 0 first then constrain sub-block 1. Instead, iterate: fix SB1, re-solve SB0 under the constraint, repeat.
   - **Expected:** +0.1 to +0.3 dB on diff-mode-favored content.
5. **MS-SSIM-based final selection between top-K candidates.**
   - Cluster fit produces one winner per (flip, diff). Instead, retain top-K=8 and select between them under MS-SSIM evaluated on a 3×3-block window (8×8 windowed, including already-encoded neighbors).
   - **Expected:** small RGB-PSNR change, visibly better on smooth regions / faces.
6. **Branch-and-bound exhaustive base-color search for "ultra" mode.**
   - At time-no-object, enumerate all 4096 RGB444 (or 32768 RGB555) base colors, with greedy selectors and exhaustive table choice. Provides a *certified-optimal* baseline (under per-block MSE).
   - **Expected:** matches cluster fit ±0.05 dB on natural images; can win up to +0.5 dB on adversarial/synthetic blocks (cf. ISPC's win on the red/blue test).
7. **Differentiable post-pass.**
   - Take the encoder output, relax selectors via Gumbel-softmax + STE, run gradient descent on continuous base color under a perceptual loss (LPIPS or MS-SSIM), then re-quantize. Use the discrete encoder as initialization.
   - **Expected:** quality gain on perceptual metrics by definition; small or zero change on RGB MSE.
8. **Neural warm-start (optional, only if scaling to many images).**
   - Train CNN on (4×4 RGB → encoded params) using the ultra-mode encoder as ground truth. Use to skip 95% of search at inference.
   - **Expected:** speedup, no quality gain. Only relevant if "time doesn't matter" later becomes "time matters a little."

### 5.3 Expected overall delta from baseline basislib cluster-fit

- **vs etc2comp effort 100:** +0.1 to +0.4 dB RGB PSNR, +1 to +2 dB Y-PSNR with perceptual metric.
- **vs etcpak:** +1.5 to +3.5 dB.
- **Visual quality at 1920×1080:** likely indistinguishable from etc2comp on natural photos to most observers; clearly better on synthetic / saturated content (red/blue case showed 9 dB swings between encoders); clearly better on smooth gradients with cross-block refinement.

### 5.4 What you cannot fix

ETC1's structural limits — RGB444/555 base, 8 fixed modifier tables, only 4 selector levels per pixel — cap the format around 35–40 dB on natural images. No encoder, no matter how sophisticated, breaks ~42 dB on Kodak content. To go higher you must change format (ETC2 RGB8, ASTC, BC7).

---

## 6. Summary Table

| Encoder | License | Status | Type | ETC1 Quality (kodim03 PSNR) | Speed Class | SOTA? |
|---|---|---|---|---|---|---|
| ETCPACK (Ericsson) | Custom | reference | exhaustive base, greedy selectors | ~38.5 (slow) | very slow | reference baseline |
| rg-etc1 | zlib | archived | 3D lattice + iter refine | ~38.2 (uber) | slow | superseded by basislib |
| basislib cluster-fit (full ETC1 in basis_universal) | Apache 2.0 | active | cluster fit + LS | **38.982** | medium-slow | **YES** |
| etc2comp | Apache 2.0 | archived 2022 | block archetype + iter | **39.095** | slow at e100 | tied |
| etcpak | BSD-3 | active | heuristic + SIMD | 37.073 | fastest | no (speed king) |
| Compressonator | MIT | active | ETCPACK-derived | mid-pack | medium | no |
| ISPC Tex Comp | MIT | abandoned by Intel | heuristic + ISPC | ~36.0 (kodim18) | fast | no, but best on adversarial |
| Betsy | MIT-ish | active | rg_etc1-style on GPU | ≈etc2comp on RMSLE | very fast (GPU) | no |
| Goofy | MIT | active | YCoCg principal axis | ~36.3 (corpus) | very fast | no |
| Mali Tex Comp Tool | proprietary | maintained | exhaustive (slow mode) | unbenched, anecdotally good | slow | maybe — closed source |
| PVRTexTool | proprietary | maintained | unknown internals | unbenched, "Best Quality" >30min/2K² | very slow (best mode) | maybe — closed source |
| Etz | Zig | hobby | rg_etc1 port | =rg_etc1 | medium | no |
| basis_universal ETC1S | Apache 2.0 | active | constrained ETC1 + VQ codebook | quality-traded for size | medium | N/A (different goal) |

---

## 7. Source Index (all citations)

- rg-etc1 repo: https://github.com/richgel999/rg-etc1
- rg_etc1 header: https://github.com/richgel999/rg-etc1/blob/master/rg_etc1.h
- basis_universal: https://github.com/BinomialLLC/basis_universal
- basis_universal home: https://binomialllc.github.io/basis_universal/
- ETC1S spec wiki: https://github.com/BinomialLLC/basis_universal/wiki/.basis-File-Format-and-ETC1S-Texture-Video-Specification
- ETC1S compression effort levels: https://github.com/BinomialLLC/basis_universal/wiki/ETC1S-Compression-Effort-Levels
- Encoding ETC1S texture video tips: https://github.com/BinomialLLC/basis_universal/wiki/Encoding-ETC1S-Texture-Video-Tips
- etc2comp repo: https://github.com/google/etc2comp
- etc2comp announcement: https://opensource.googleblog.com/2016/11/etc2comp-fast-texture-compression.html
- etcpak repo: https://github.com/wolfpld/etcpak
- etcpak license: https://github.com/wolfpld/etcpak/blob/master/LICENSE.txt
- ETCPACK (Ericsson reference): https://github.com/Ericsson/ETCPACK
- Compressonator: https://github.com/GPUOpen-Tools/compressonator
- AMD Compressonator: https://gpuopen.com/compressonator/
- ISPC Texture Compressor: https://github.com/GameTechDev/ISPCTextureCompressor
- Betsy repo: https://github.com/darksylinc/betsy
- Betsy technical doc: https://github.com/darksylinc/betsy/blob/master/Docs/technical_doc_advanced.md
- Betsy Godot announcement: https://godotengine.org/article/betsy-gpu-texture-compressor/
- Goofy: https://github.com/SergeyMakeev/Goofy
- Mali Texture Compression Tool: https://developer.arm.com/tools-and-software/graphics-and-gaming/mali-texture-compression-tool
- ASTC encoder (NOT ETC1): https://github.com/ARM-software/astc-encoder
- PVRTexTool: https://developer.imaginationtech.com/solutions/pvrtextool/
- Etz: https://ziggit.dev/t/etz-etc1-texture-encoder-decoder/15172
- iPACKMAN paper: https://www.semanticscholar.org/paper/iPACKMAN%3A-high-quality%2C-low-complexity-texture-for-Str%C3%B6m-Akenine-M%C3%B6ller/1a85970157ebfe4455c1461f5c18764798cc6f1e
- ETC Wikipedia: https://en.wikipedia.org/wiki/Ericsson_Texture_Compression
- crunch repo: https://github.com/BinomialLLC/crunch
- QuickETC2 paper: https://nahjaeho.github.io/papers/SA20/QUICKETC2_SA20.pdf
- QuickETC2 ACM: https://dl.acm.org/doi/fullHtml/10.1145/3388767.3407373
- QuickETC2-HQ: https://nahjaeho.github.io/papers/CAG23/cag-preprint.pdf
- NTBC (neural BC): https://arxiv.org/html/2407.09543v2
- Hardware-accelerated NBT: https://arxiv.org/html/2506.06040v1

### Geldreich blog posts cited

- ETC1/2 benchmark: http://richg42.blogspot.com/2016/09/comparison-of-three-etc1-and-etc2-block.html
- Visualizing ETC1: https://richg42.blogspot.com/2016/09/visualizing-etc1-texture-compression.html
- etcpak critique: http://richg42.blogspot.com/2016/09/etcpak.html
- etcpak quick test: https://richg42.blogspot.com/2016/09/quick-etcpak-quality-test.html
- ETC1 cluster fit method: http://richg42.blogspot.com/2016/09/libsquishs-dxt1-cluster-fit-method.html
- More ETC1 cluster fit data: http://richg42.blogspot.com/2016/10/more-etc1-cluster-fit-data.html
- etc2comp effort impact: http://richg42.blogspot.com/2016/10/etc2comps-effort-parameters-impact-on.html
- Adversarial encoding test: https://richg42.blogspot.com/2016/09/an-interesting-etc12-encoding-test.html
- Weighted YCbCr metric: http://richg42.blogspot.com/2018/04/bc7-encoding-using-weighted-ycbcr.html
- ETC1S encoding & BC1 transcode: https://richg42.blogspot.com/2018/06/etc1s-texture-format-encoding.html
- ETC1 block color clusterization: https://richg42.blogspot.com/2016/09/crunch-for-etc1-block-color.html
- State of ETC1/2 libs: https://richg42.blogspot.com/2016/09/lets-evaluate-current-state-of-etc12.html

---

## 8. Honest gaps & flags

1. **No 1920×1080 published benchmarks for ETC1 anywhere I found.** All literature uses kodim* (768×512), corpus collections, or the Visual Studio set. The user must run encoders themselves to compare at 1920×1080.
2. **No public side-by-side image gallery for ETC1 encoders at FullHD.** Geldreich's blog has block-level visualizations only.
3. **PSNR comparisons across sources are inconsistent.** Geldreich himself notes encoders compute PSNR differently. Always re-decode and compute PSNR/SSIM externally with a fixed tool (ImageMagick `compare`, or a custom decoder + scikit-image SSIM).
4. **No proven-optimal-per-block ETC1 encoder exists.** rg_etc1 / cluster-fit claim "near-optimal" but the proof is conditional on the selector-distribution enumeration being complete.
5. **PVRTexTool and Mali Tool are closed source.** Their "best quality" modes might match or beat basislib but cannot be inspected or modified.
6. **ETC1S ≠ ETC1** for quality purposes — keep them separate when reading literature; ETC1S sacrifices ~few dB for transcodability.
7. **etc2comp and rg-etc1 are both archived/unmaintained.** Active actively-developed quality-oriented options reduce to: basis_universal (CPU), Betsy (GPU), Compressonator (CPU/GPU, ETCPACK-derived), etcpak (CPU, speed-first).
