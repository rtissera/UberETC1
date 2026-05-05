// basisu_etc1_tool_v7_guarded_idea5.cpp — v6_v2 + chroma-floor guard rail + #5
// (multi-table sub-block refinement on worst-N).  Built strictly on top of
// v6_idea2_v2; v6_v2's source is left untouched.
//
// Two stacked features over v6_v2:
//
// (1) Guard rail (chroma sanity floor on the MS-SSIM rerank pool):
//     v6_v2 picks the MS-SSIM winner from the {top-K + pass-1} pool.  The
//     known failure mode is a -0.392 dB RGB-PSNR regression vs v6 because
//     MS-SSIM occasionally rewards a candidate whose RGB-MSE blows up for
//     a small Y-MSE / SSIM win.  Guard rail forbids that: of the 9 ranked
//     candidates, walk in MS-SSIM-descending order and commit the first
//     whose decoded RGB-MSE <= floor_ratio × pool_best_RGB_MSE.  With
//     T = 1.5 dB chroma floor, ratio = 10^(1.5/10) ≈ 1.413.  No candidate
//     passes? Fall back to the pass-1 pool entry (safest).
//
//     Knob: g_uberetc1_chroma_floor_db (compiled-in, default 1.5).
//
// (2) Idea #5 — multi-table sub-block refinement on the picked candidate:
//     After the guard rail commits a candidate, run an iterative refiner
//     (max 4 iters), per sub-block:
//       a) Given current selectors + current inten table → LS-solve base
//          color (continuous, RGB; mean over 8 pixels of (pixel - I[T,s])).
//       b) Quantize to RGB444 (color4=true / diff=false) or RGB555
//          (color4=false / diff=true).  Try all 27 corners of the
//          {center, ±1}^3 cube around the LS-quantized point and pick the
//          MSE-best (the cube includes round-to-nearest at offset (0,0,0)).
//          In diff mode, sub-block 1 is constrained to within
//          [cETC1ColorDeltaMin..cETC1ColorDeltaMax] of sub-block 0's
//          quantized color5 — invalid candidates are skipped.
//       c) Re-pick selectors greedily per pixel from the 4-color palette
//          for (base, T).
//       d) Pick best inten table among all 8 (sub-block local; in diff
//          mode the constraint stays on color5 not on table — tables are
//          per-sub-block in ETC1).
//       e) If neither (color, table, selectors) changed → break.
//     Reconstruct the full block; if the new block is valid (color5 delta
//     ok), compete it against the committed v6_v2-pick under MS-SSIM on
//     the same 12×12 window.  Take winner.
//
//     Implementation does the refiner driver-side using the public ETC1
//     building blocks (g_etc1_inten_tables, get_block_colors{4,5}).  No
//     new patch is needed — the optimizer's own internals don't have to
//     be touched.  Refinement runs only on the worst N% blocks and only
//     on the surviving candidate, so per-block cost is bounded.
//
// Env knobs (same as v6_v2 plus three extras):
//   UBERETC1_WORST_PCT       (5.0)
//   UBERETC1_WORST_RADIUS    (2)
//   UBERETC1_TOPK            (8)
//   UBERETC1_CHROMA_FLOOR_DB (1.5)   — guard rail T (dB), 0 disables
//   UBERETC1_REFINE_ITERS    (4)     — multi-table refiner max iters, 0 disables
//
// Driver-only changes; no patches modified.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "basis_universal/encoder/basisu_etc.h"
#include "basis_universal/encoder/basisu_enc.h"

using namespace basisu;

// ---------- Decoded block + Y-MSE / RGB-MSE helpers ----------
static void decode_block(const etc_block &blk, color_rgba out[16])
{
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
            uint32_t s = blk.get_selector(x, y);
            out[y * 4 + x] = blk.get_selector_color(x, y, s);
        }
}

static inline double y601(const color_rgba &c)
{
    return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
}

static double block_y_mse(const color_rgba src[16], const color_rgba dec[16])
{
    double acc = 0.0;
    for (int i = 0; i < 16; ++i) {
        double d = y601(src[i]) - y601(dec[i]);
        acc += d * d;
    }
    return acc / 16.0;
}

static double block_rgb_mse(const color_rgba src[16], const color_rgba dec[16])
{
    double acc = 0.0;
    for (int i = 0; i < 16; ++i) {
        double dr = (double)src[i].r - (double)dec[i].r;
        double dg = (double)src[i].g - (double)dec[i].g;
        double db = (double)src[i].b - (double)dec[i].b;
        acc += dr*dr + dg*dg + db*db;
    }
    return acc / (16.0 * 3.0);  // per-channel mean to be consistent with PSNR_RGB
}

// ---------- Build full ETC1 block from per-sub-block top-K entries ----------
static bool make_block_from_entries(int flip, int diff,
                                    const uberetc1_topk_entry &e0,
                                    const uberetc1_topk_entry &e1,
                                    etc_block &out_block)
{
    etc_block blk; memset(&blk, 0, sizeof(blk));
    blk.set_flip_bit(flip != 0);
    blk.set_diff_bit(diff != 0);
    if (diff) {
        if (!blk.set_block_color5_check(e0.m_color_unscaled, e1.m_color_unscaled))
            return false;
    } else {
        blk.set_block_color4(e0.m_color_unscaled, e1.m_color_unscaled);
    }
    blk.set_inten_table(0, e0.m_inten_table);
    blk.set_inten_table(1, e1.m_inten_table);
    for (int s = 0; s < 2; ++s) {
        const uberetc1_topk_entry &e = (s == 0) ? e0 : e1;
        for (int i = 0; i < 8; ++i) {
            int x, y;
            if (flip) { x = i & 3; y = (i >> 2) + s * 2; }
            else      { x = (i >> 2) + s * 2; y = i & 3; }
            blk.set_selector(x, y, e.m_selectors[i]);
        }
    }
    out_block = blk;
    return true;
}

// ---------- Pass 1 encode (== v3_corners_perc / v5 / v6_v2) ----------
static void encode_block_full_etc1(const color_rgba pixels[16], etc_block &out_block,
                                   int wide_radius_override)
{
    int saved = g_uberetc1_wide_corner_radius;
    if (wide_radius_override >= 0)
        g_uberetc1_wide_corner_radius = wide_radius_override;

    uint64_t best_err = ~0ULL;
    etc_block best_blk; memset(&best_blk, 0, sizeof(best_blk));

    for (int flip = 0; flip < 2; ++flip) {
        for (int diff = 0; diff < 2; ++diff) {
            color_rgba sub[2][8];
            for (int s = 0; s < 2; ++s) {
                for (int i = 0; i < 8; ++i) {
                    int x, y;
                    if (flip) { x = i & 3; y = (i >> 2) + s * 2; }
                    else      { x = (i >> 2) + s * 2; y = i & 3; }
                    sub[s][i] = pixels[y * 4 + x];
                }
            }

            etc1_optimizer opt0; etc1_optimizer::params p0; etc1_optimizer::results r0;
            uint8_t sel0[8];
            p0.m_num_src_pixels = 8; p0.m_pSrc_pixels = sub[0];
            p0.m_quality = cETCQualityUber; p0.m_perceptual = true;
            p0.m_cluster_fit = true; p0.m_use_color4 = !diff; p0.m_refinement = true;
            r0.m_pSelectors = sel0; r0.m_n = 8;
            opt0.init(p0, r0); if (!opt0.compute()) continue;

            etc1_optimizer opt1; etc1_optimizer::params p1; etc1_optimizer::results r1;
            uint8_t sel1[8];
            p1.m_num_src_pixels = 8; p1.m_pSrc_pixels = sub[1];
            p1.m_quality = cETCQualityUber; p1.m_perceptual = true;
            p1.m_cluster_fit = true; p1.m_use_color4 = !diff; p1.m_refinement = true;
            if (diff) {
                p1.m_constrain_against_base_color5 = true;
                p1.m_base_color5 = r0.m_block_color_unscaled;
            }
            r1.m_pSelectors = sel1; r1.m_n = 8;
            opt1.init(p1, r1); if (!opt1.compute()) continue;

            uint64_t err = r0.m_error + r1.m_error;
            if (err < best_err) {
                best_err = err;
                etc_block blk; memset(&blk, 0, sizeof(blk));
                blk.set_flip_bit(flip != 0); blk.set_diff_bit(diff != 0);
                if (diff) {
                    if (!blk.set_block_color5_check(r0.m_block_color_unscaled, r1.m_block_color_unscaled)) continue;
                } else {
                    blk.set_block_color4(r0.m_block_color_unscaled, r1.m_block_color_unscaled);
                }
                blk.set_inten_table(0, r0.m_block_inten_table);
                blk.set_inten_table(1, r1.m_block_inten_table);
                for (int s = 0; s < 2; ++s) {
                    const uint8_t *sel = (s == 0) ? sel0 : sel1;
                    for (int i = 0; i < 8; ++i) {
                        int x, y;
                        if (flip) { x = i & 3; y = (i >> 2) + s * 2; }
                        else      { x = (i >> 2) + s * 2; y = i & 3; }
                        blk.set_selector(x, y, sel[i]);
                    }
                }
                best_blk = blk;
            }
        }
    }
    out_block = best_blk;
    g_uberetc1_wide_corner_radius = saved;
}

// ---------- Sub-block top-K drain ----------
static bool run_subblock_topk(const color_rgba sub_pixels[8], bool color4,
                              int wide_radius, int K,
                              const color_rgba *constrain_color5,
                              std::vector<uberetc1_topk_entry> &dst)
{
    int saved_r = g_uberetc1_wide_corner_radius;
    int saved_k = g_uberetc1_topk_capture;
    g_uberetc1_wide_corner_radius = wide_radius;
    g_uberetc1_topk_buffer.clear();
    g_uberetc1_topk_capture = K;

    etc1_optimizer opt; etc1_optimizer::params p; etc1_optimizer::results r;
    uint8_t sel[8];
    p.m_num_src_pixels = 8;
    p.m_pSrc_pixels = sub_pixels;
    p.m_quality = cETCQualityUber;
    p.m_perceptual = true;
    p.m_cluster_fit = true;
    p.m_use_color4 = color4;
    p.m_refinement = true;
    if (constrain_color5) {
        p.m_constrain_against_base_color5 = true;
        p.m_base_color5 = *constrain_color5;
    }
    r.m_pSelectors = sel;
    r.m_n = 8;
    opt.init(p, r);
    bool ok = opt.compute();

    dst.clear();
    if (ok) {
        for (uint32_t i = 0; i < g_uberetc1_topk_buffer.size(); ++i)
            dst.push_back(g_uberetc1_topk_buffer[i]);
    }

    g_uberetc1_topk_capture = saved_k;
    g_uberetc1_topk_buffer.clear();
    g_uberetc1_wide_corner_radius = saved_r;
    return ok;
}

// ---------- MS-SSIM_Y window ----------
static double ssim_y(const std::vector<double> &a, const std::vector<double> &b)
{
    if (a.empty()) return 1.0;
    const size_t n = a.size();
    double mu_a = 0, mu_b = 0;
    for (size_t i = 0; i < n; ++i) { mu_a += a[i]; mu_b += b[i]; }
    mu_a /= n; mu_b /= n;
    double var_a = 0, var_b = 0, cov = 0;
    for (size_t i = 0; i < n; ++i) {
        double da = a[i] - mu_a, db = b[i] - mu_b;
        var_a += da*da; var_b += db*db; cov += da*db;
    }
    var_a /= n; var_b /= n; cov /= n;
    const double L = 255.0;
    const double C1 = (0.01*L)*(0.01*L);
    const double C2 = (0.03*L)*(0.03*L);
    double num = (2*mu_a*mu_b + C1) * (2*cov + C2);
    double den = (mu_a*mu_a + mu_b*mu_b + C1) * (var_a + var_b + C2);
    if (den <= 0) return 1.0;
    return num / den;
}

static void downsample_2x_y(const std::vector<double> &in, int W, int H,
                            std::vector<double> &out, int &outW, int &outH)
{
    outW = W / 2; outH = H / 2;
    out.assign((size_t)outW * (size_t)outH, 0.0);
    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            double s = in[(2*y + 0)*W + 2*x + 0] + in[(2*y + 0)*W + 2*x + 1]
                     + in[(2*y + 1)*W + 2*x + 0] + in[(2*y + 1)*W + 2*x + 1];
            out[y*outW + x] = s * 0.25;
        }
    }
}

static double msssim_y(const std::vector<double> &orig_y, int W, int H,
                       const std::vector<double> &cand_y)
{
    double prod = 1.0;
    int n_scales = 0;
    {
        double s = ssim_y(orig_y, cand_y);
        if (s < 0) s = 0;
        prod *= std::max(s, 1e-6);
        n_scales++;
    }
    std::vector<double> oa, ca;
    int W2, H2;
    downsample_2x_y(orig_y, W, H, oa, W2, H2);
    int W2c, H2c;
    downsample_2x_y(cand_y, W, H, ca, W2c, H2c);
    if (W2 >= 2 && H2 >= 2) {
        double s = ssim_y(oa, ca);
        if (s < 0) s = 0;
        prod *= std::max(s, 1e-6);
        n_scales++;
        std::vector<double> oa2, ca2;
        int W4, H4, W4c, H4c;
        downsample_2x_y(oa, W2, H2, oa2, W4, H4);
        downsample_2x_y(ca, W2, H2, ca2, W4c, H4c);
        if (W4 >= 2 && H4 >= 2) {
            double s2 = ssim_y(oa2, ca2);
            if (s2 < 0) s2 = 0;
            prod *= std::max(s2, 1e-6);
            n_scales++;
        }
    }
    return std::pow(prod, 1.0 / (double)n_scales);
}

// ---------- Idea #5 multi-table refiner (driver-side) ----------
//
// We work on a single 4×4 etc_block and improve it by iterating, per
// sub-block: LS base color → quantize/try-corners → re-pick selectors →
// re-pick best inten table.  In diff mode we always keep sub-block 0's
// chosen color5 fixed before re-solving sub-block 1 (so its constraint
// holds), then re-solve sub-block 0 next iter against the new sub-block 1.
//
// All sub-block math is in RGB MSE (per the spec — "MSE-best modifier
// table").  Returns the refined etc_block in `out` and true if any change.

// Pick best selector per pixel from a 4-color palette.  Returns sum RGB MSE.
static double pick_selectors_greedy(const color_rgba sub[8], const color_rgba pal[4],
                                    uint8_t sel_out[8])
{
    double total = 0;
    for (int i = 0; i < 8; ++i) {
        double best = 1e30; int best_s = 0;
        for (int s = 0; s < 4; ++s) {
            double dr = (double)sub[i].r - (double)pal[s].r;
            double dg = (double)sub[i].g - (double)pal[s].g;
            double db = (double)sub[i].b - (double)pal[s].b;
            double e = dr*dr + dg*dg + db*db;
            if (e < best) { best = e; best_s = s; }
        }
        sel_out[i] = (uint8_t)best_s;
        total += best;
    }
    return total;
}

// Given inten table T and selectors s[i], LS-solve scaled base color (b
// in [0..255]) per channel via mean(pixel - I[T,s[i]]).
static void ls_solve_scaled_base(const color_rgba sub[8], int T, const uint8_t sel[8],
                                 double scaled_out[3])
{
    const int *I = g_etc1_inten_tables[T];
    double sum_r = 0, sum_g = 0, sum_b = 0;
    for (int i = 0; i < 8; ++i) {
        sum_r += (double)sub[i].r - (double)I[sel[i]];
        sum_g += (double)sub[i].g - (double)I[sel[i]];
        sum_b += (double)sub[i].b - (double)I[sel[i]];
    }
    scaled_out[0] = sum_r * 0.125;
    scaled_out[1] = sum_g * 0.125;
    scaled_out[2] = sum_b * 0.125;
}

// Quantize scaled [0..255] to color4 (4-bit) or color5 (5-bit) unscaled,
// rounded to nearest, clamped.  Returns 0..15 / 0..31 per channel.
static inline int quantize_to_unscaled(double v, bool color4)
{
    if (color4) {
        // 4-bit: scaled = (u << 4) | u = u * 17.  u = round(v / 17).
        int u = (int)std::lround(v / 17.0);
        if (u < 0) u = 0; if (u > 15) u = 15;
        return u;
    } else {
        // 5-bit: scaled = (u << 3) | (u >> 2).  Approx u = round(v / 8.226).
        // Use exact inverse via search over 32 values? Just round v * 31/255.
        int u = (int)std::lround(v * (31.0 / 255.0));
        if (u < 0) u = 0; if (u > 31) u = 31;
        return u;
    }
}

// Refine one sub-block: try all 8 inten tables × all 27 corners around the
// LS-quantized base.  Optional diff constraint: enforce
// |delta_chan| <= 4 against constrain_unscaled in unscaled color5 space.
// Returns best (T, unscaled, sel, sum-RGB-MSE).
struct SubblockSolution {
    int T;
    color_rgba unscaled;  // 4-bit or 5-bit
    uint8_t sel[8];
    double err;
};

static bool refine_subblock(const color_rgba sub[8], bool color4,
                            const color_rgba *constrain_color5_unscaled,
                            // initial state to seed LS solve:
                            int T_init, const uint8_t sel_init[8],
                            SubblockSolution &out)
{
    double best_err = 1e30;
    bool found = false;

    // The spec's per-iteration sequence is: (a) LS-solve base, (b)
    // quantize / try-corners, (c) re-pick selectors, (d) re-pick best
    // inten table.  We implement (a)+(b)+(c)+(d) jointly by computing
    // the LS base once using (T_init, sel_init), then sweeping all
    // (T_try ∈ 0..7) × (corner ∈ ±1 around the LS-quantized base) and
    // re-picking selectors greedily for each.  The argmin is the joint
    // best; this is a strict superset of the literal sequence.

    // Compute LS base once using initial selectors+table (in scaled space).
    double b_scaled[3];
    ls_solve_scaled_base(sub, T_init, sel_init, b_scaled);

    // Quantize to unscaled (4 or 5 bits).
    int u_init[3];
    for (int c = 0; c < 3; ++c)
        u_init[c] = quantize_to_unscaled(b_scaled[c], color4);

    const int u_max = color4 ? 15 : 31;
    for (int dr = -1; dr <= 1; ++dr)
        for (int dg = -1; dg <= 1; ++dg)
            for (int db = -1; db <= 1; ++db) {
                int ur = u_init[0] + dr;
                int ug = u_init[1] + dg;
                int ub = u_init[2] + db;
                if (ur < 0 || ur > u_max) continue;
                if (ug < 0 || ug > u_max) continue;
                if (ub < 0 || ub > u_max) continue;

                // Diff constraint: only valid for color4=false (color5).
                if (constrain_color5_unscaled) {
                    int dr2 = ur - (int)constrain_color5_unscaled->r;
                    int dg2 = ug - (int)constrain_color5_unscaled->g;
                    int db2 = ub - (int)constrain_color5_unscaled->b;
                    if (dr2 < cETC1ColorDeltaMin || dr2 > cETC1ColorDeltaMax) continue;
                    if (dg2 < cETC1ColorDeltaMin || dg2 > cETC1ColorDeltaMax) continue;
                    if (db2 < cETC1ColorDeltaMin || db2 > cETC1ColorDeltaMax) continue;
                }

                color_rgba u_color((uint8_t)ur, (uint8_t)ug, (uint8_t)ub, 255);

                // For each inten table, build the 4-color palette and pick
                // selectors greedily.
                for (int T = 0; T < (int)cETC1IntenModifierValues; ++T) {
                    color_rgba pal[4];
                    if (color4)
                        etc_block::get_block_colors4(pal, u_color, (uint32_t)T);
                    else
                        etc_block::get_block_colors5(pal, u_color, (uint32_t)T);

                    uint8_t sel_try[8];
                    double e = pick_selectors_greedy(sub, pal, sel_try);
                    if (e < best_err) {
                        best_err = e;
                        out.T = T;
                        out.unscaled = u_color;
                        memcpy(out.sel, sel_try, 8);
                        out.err = e;
                        found = true;
                    }
                }
            }
    return found;
}

// Run multi-iter refinement starting from a full block.  Returns refined
// block in `refined_out` (always populated), and `improved` = true iff
// the refined block is different from input.
static bool multitable_refine(const color_rgba src[16], const etc_block &in_blk,
                              int max_iters, etc_block &refined_out)
{
    int flip = in_blk.get_flip_bit() ? 1 : 0;
    int diff = in_blk.get_diff_bit() ? 1 : 0;
    bool color4 = !diff;

    // Extract per-sub-block pixels and current selectors/table/unscaled-color.
    color_rgba sub[2][8];
    uint8_t cur_sel[2][8];
    int cur_T[2];
    color_rgba cur_unscaled[2];
    for (int s = 0; s < 2; ++s) {
        cur_T[s] = (int)in_blk.get_inten_table(s);
        for (int i = 0; i < 8; ++i) {
            int x, y;
            if (flip) { x = i & 3; y = (i >> 2) + s * 2; }
            else      { x = (i >> 2) + s * 2; y = i & 3; }
            sub[s][i] = src[y * 4 + x];
            cur_sel[s][i] = (uint8_t)in_blk.get_selector(x, y);
        }
    }
    cur_unscaled[0] = in_blk.get_block_color(0, /*scaled=*/false);
    cur_unscaled[1] = in_blk.get_block_color(1, /*scaled=*/false);
    cur_unscaled[0].a = 255;
    cur_unscaled[1].a = 255;

    bool any_change_overall = false;

    for (int it = 0; it < max_iters; ++it) {
        bool changed = false;

        // Sub-block 0 first.  No diff constraint on sub-0 (it sets the base color5).
        SubblockSolution s0;
        if (refine_subblock(sub[0], color4, /*constrain=*/nullptr,
                            cur_T[0], cur_sel[0], s0)) {
            // Accept new state if anything differs.
            if (s0.T != cur_T[0] ||
                s0.unscaled.r != cur_unscaled[0].r ||
                s0.unscaled.g != cur_unscaled[0].g ||
                s0.unscaled.b != cur_unscaled[0].b ||
                memcmp(s0.sel, cur_sel[0], 8) != 0) {
                cur_T[0] = s0.T;
                cur_unscaled[0] = s0.unscaled;
                memcpy(cur_sel[0], s0.sel, 8);
                changed = true;
            }
        }

        // Sub-block 1: if diff mode, constrain against current sub-0 unscaled (color5).
        SubblockSolution s1;
        if (refine_subblock(sub[1], color4,
                            diff ? &cur_unscaled[0] : nullptr,
                            cur_T[1], cur_sel[1], s1)) {
            if (s1.T != cur_T[1] ||
                s1.unscaled.r != cur_unscaled[1].r ||
                s1.unscaled.g != cur_unscaled[1].g ||
                s1.unscaled.b != cur_unscaled[1].b ||
                memcmp(s1.sel, cur_sel[1], 8) != 0) {
                cur_T[1] = s1.T;
                cur_unscaled[1] = s1.unscaled;
                memcpy(cur_sel[1], s1.sel, 8);
                changed = true;
            }
        }

        if (!changed) break;
        any_change_overall = true;
    }

    // Reconstruct block.
    etc_block blk; memset(&blk, 0, sizeof(blk));
    blk.set_flip_bit(flip != 0);
    blk.set_diff_bit(diff != 0);
    if (diff) {
        if (!blk.set_block_color5_check(cur_unscaled[0], cur_unscaled[1])) {
            // Should be impossible thanks to the constraint, but be safe.
            refined_out = in_blk;
            return false;
        }
    } else {
        blk.set_block_color4(cur_unscaled[0], cur_unscaled[1]);
    }
    blk.set_inten_table(0, (uint32_t)cur_T[0]);
    blk.set_inten_table(1, (uint32_t)cur_T[1]);
    for (int s = 0; s < 2; ++s) {
        for (int i = 0; i < 8; ++i) {
            int x, y;
            if (flip) { x = i & 3; y = (i >> 2) + s * 2; }
            else      { x = (i >> 2) + s * 2; y = i & 3; }
            blk.set_selector(x, y, cur_sel[s][i]);
        }
    }
    refined_out = blk;
    return any_change_overall;
}

static double env_double(const char *name, double defv) {
    const char *e = std::getenv(name); return e ? std::strtod(e, nullptr) : defv;
}
static int env_int(const char *name, int defv) {
    const char *e = std::getenv(name); return e ? std::atoi(e) : defv;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s encode|decode in out\n", argv[0]); return 1; }
    basisu_encoder_init();

    std::string mode = argv[1];
    if (mode == "encode") {
        const double worst_pct       = env_double("UBERETC1_WORST_PCT", 5.0);
        const int    radius          = env_int   ("UBERETC1_WORST_RADIUS", 2);
        const int    K               = std::max(1, env_int("UBERETC1_TOPK", 8));
        const double chroma_floor_db = env_double("UBERETC1_CHROMA_FLOOR_DB", 1.5);
        const int    refine_iters    = env_int   ("UBERETC1_REFINE_ITERS", 4);
        const double floor_ratio = (chroma_floor_db > 0)
                                       ? std::pow(10.0, chroma_floor_db / 10.0)
                                       : 1e30;  // disabled → no constraint
        fprintf(stderr,
                "v7_guarded_idea5 worst_pct=%.2f radius=%d K=%d "
                "chroma_floor_db=%.2f (ratio=%.4f) refine_iters=%d\n",
                worst_pct, radius, K, chroma_floor_db, floor_ratio, refine_iters);

        int w, h, n;
        unsigned char *img = stbi_load(argv[2], &w, &h, &n, 4);
        if (!img) { fprintf(stderr, "load fail\n"); return 1; }
        if ((w & 3) || (h & 3)) { fprintf(stderr, "bad dims\n"); return 1; }
        const int bw = w / 4, bh = h / 4;
        const int nblk = bw * bh;
        std::vector<etc_block> blocks(nblk);
        std::vector<double>    block_err(nblk, 0.0);

        std::vector<std::array<color_rgba, 16>> src_pix(nblk);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                color_rgba *px = src_pix[by * bw + bx].data();
                for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                    const uint8_t *p = &img[((by*4 + y)*w + (bx*4 + x))*4];
                    px[y*4+x] = color_rgba(p[0], p[1], p[2], 255);
                }
            }

        // ---- Pass 1 ----
        auto t0 = std::chrono::steady_clock::now();
        #pragma omp parallel for schedule(dynamic) collapse(2)
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                int idx = by * bw + bx;
                encode_block_full_etc1(src_pix[idx].data(), blocks[idx], -1);
                color_rgba dec[16]; decode_block(blocks[idx], dec);
                block_err[idx] = block_y_mse(src_pix[idx].data(), dec);
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        fprintf(stderr, "v7_guarded_idea5 pass1 %dx%d in %.3f s\n",
                w, h, std::chrono::duration<double>(t1-t0).count());

        // ---- Pass-1 decoded Y plane ----
        std::vector<double> pass1_y((size_t)w * (size_t)h, 0.0);
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                int idx = by * bw + bx;
                color_rgba dec[16]; decode_block(blocks[idx], dec);
                for (int yy = 0; yy < 4; ++yy) for (int xx = 0; xx < 4; ++xx) {
                    int gx = bx*4 + xx, gy = by*4 + yy;
                    pass1_y[(size_t)gy*w + gx] = y601(dec[yy*4+xx]);
                }
            }
        }
        std::vector<double> src_y((size_t)w * (size_t)h, 0.0);
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) {
                const uint8_t *p = &img[(yy*w + xx)*4];
                src_y[(size_t)yy*w + xx] = 0.299*p[0] + 0.587*p[1] + 0.114*p[2];
            }

        // ---- Identify worst N% by pass-1 Y-MSE ----
        std::vector<int> worst_idx;
        const double pct_clamped = std::max(0.0, std::min(100.0, worst_pct));
        const int target_count = (int)((pct_clamped / 100.0) * nblk + 0.5);
        double thresh = -1.0;
        if (target_count > 0) {
            std::vector<double> sorted_err = block_err;
            std::sort(sorted_err.begin(), sorted_err.end());
            const int idx_thresh = std::max(0, nblk - target_count);
            thresh = sorted_err[idx_thresh];
            worst_idx.reserve(target_count + 64);
            for (int i = 0; i < nblk; ++i)
                if (block_err[i] >= thresh) worst_idx.push_back(i);
        }

        // Counters.
        std::atomic<int> n_pool_drained{0};
        std::atomic<int> n_winner_pass1{0};
        std::atomic<int> n_winner_pass2{0};
        std::atomic<int> n_floor_kicked_in{0};      // guard rail rejected MS-SSIM-best
        std::atomic<int> n_floor_no_pass{0};         // no candidate passed → fallback pass-1
        std::atomic<int> n_refine_changed{0};       // refiner produced a different block
        std::atomic<int> n_refine_won_msssim{0};    // refined beat pre-refine on MS-SSIM
        std::atomic<int> n_refine_won_rgb_mse{0};   // refined had better RGB MSE
        std::atomic<long long> sum_pool_size{0};

        #pragma omp parallel for schedule(dynamic)
        for (size_t k = 0; k < worst_idx.size(); ++k) {
            int idx = worst_idx[k];
            int by = idx / bw, bx = idx % bw;
            const color_rgba *src = src_pix[idx].data();

            struct PoolEnt {
                etc_block blk;
                uint64_t  perc_err;
                double    y_mse;
                double    rgb_mse;
                bool      is_pass1;
            };
            std::vector<PoolEnt> pool;
            pool.reserve(4 * K * K);

            for (int flip = 0; flip < 2; ++flip) {
                color_rgba sub[2][8];
                for (int s = 0; s < 2; ++s)
                    for (int i = 0; i < 8; ++i) {
                        int x, y;
                        if (flip) { x = i & 3; y = (i >> 2) + s * 2; }
                        else      { x = (i >> 2) + s * 2; y = i & 3; }
                        sub[s][i] = src[y * 4 + x];
                    }
                for (int diff = 0; diff < 2; ++diff) {
                    bool color4 = !diff;

                    std::vector<uberetc1_topk_entry> buf0;
                    if (!run_subblock_topk(sub[0], color4, radius, K,
                                           nullptr, buf0)) continue;
                    if (buf0.empty()) continue;

                    if (!diff) {
                        std::vector<uberetc1_topk_entry> buf1;
                        if (!run_subblock_topk(sub[1], color4, radius, K,
                                               nullptr, buf1)) continue;
                        if (buf1.empty()) continue;
                        for (const auto &e0 : buf0)
                            for (const auto &e1 : buf1) {
                                etc_block bk;
                                if (!make_block_from_entries(flip, diff, e0, e1, bk))
                                    continue;
                                color_rgba dec[16]; decode_block(bk, dec);
                                PoolEnt pe;
                                pe.blk = bk;
                                pe.perc_err = e0.m_error + e1.m_error;
                                pe.y_mse   = block_y_mse(src, dec);
                                pe.rgb_mse = block_rgb_mse(src, dec);
                                pe.is_pass1 = false;
                                pool.push_back(pe);
                            }
                    } else {
                        for (const auto &e0 : buf0) {
                            std::vector<uberetc1_topk_entry> buf1;
                            color_rgba constrain = e0.m_color_unscaled;
                            if (!run_subblock_topk(sub[1], color4, radius, K,
                                                   &constrain, buf1)) continue;
                            if (buf1.empty()) continue;
                            for (const auto &e1 : buf1) {
                                etc_block bk;
                                if (!make_block_from_entries(flip, diff, e0, e1, bk))
                                    continue;
                                color_rgba dec[16]; decode_block(bk, dec);
                                PoolEnt pe;
                                pe.blk = bk;
                                pe.perc_err = e0.m_error + e1.m_error;
                                pe.y_mse   = block_y_mse(src, dec);
                                pe.rgb_mse = block_rgb_mse(src, dec);
                                pe.is_pass1 = false;
                                pool.push_back(pe);
                            }
                        }
                    }
                }
            }

            if (pool.empty()) continue;
            n_pool_drained.fetch_add(1, std::memory_order_relaxed);

            // Reduce pool to global top-K by perc_err.
            if ((int)pool.size() > K) {
                std::partial_sort(pool.begin(), pool.begin() + K, pool.end(),
                    [](const PoolEnt &a, const PoolEnt &b){ return a.perc_err < b.perc_err; });
                pool.resize(K);
            } else {
                std::sort(pool.begin(), pool.end(),
                    [](const PoolEnt &a, const PoolEnt &b){ return a.perc_err < b.perc_err; });
            }
            sum_pool_size.fetch_add((long long)pool.size(), std::memory_order_relaxed);

            // ---- Compute pool RGB-MSE floor BEFORE adding pass-1 ----
            double pool_best_rgb_mse = 1e30;
            for (const auto &p : pool)
                if (p.rgb_mse < pool_best_rgb_mse) pool_best_rgb_mse = p.rgb_mse;
            double rgb_threshold = pool_best_rgb_mse * floor_ratio;

            // Add pass-1 result as candidate K+1.
            int pass1_idx;
            {
                color_rgba dec[16]; decode_block(blocks[idx], dec);
                PoolEnt pe;
                pe.blk = blocks[idx];
                pe.perc_err = ~0ULL;
                pe.y_mse = block_y_mse(src, dec);
                pe.rgb_mse = block_rgb_mse(src, dec);
                pe.is_pass1 = true;
                pool.push_back(pe);
                pass1_idx = (int)pool.size() - 1;
            }

            // ---- MS-SSIM rerank window ----
            int win_x0 = (bx > 0)   ? (bx - 1)*4 : bx*4;
            int win_y0 = (by > 0)   ? (by - 1)*4 : by*4;
            int win_x1 = (bx+1 < bw) ? (bx + 2)*4 : (bx+1)*4;
            int win_y1 = (by+1 < bh) ? (by + 2)*4 : (by+1)*4;
            int win_w  = win_x1 - win_x0;
            int win_h  = win_y1 - win_y0;

            std::vector<double> orig_win((size_t)win_w * (size_t)win_h);
            std::vector<double> base_win((size_t)win_w * (size_t)win_h);
            for (int yy = 0; yy < win_h; ++yy)
                for (int xx = 0; xx < win_w; ++xx) {
                    orig_win[(size_t)yy*win_w + xx] = src_y[(size_t)(win_y0+yy)*w + (win_x0+xx)];
                    base_win[(size_t)yy*win_w + xx] = pass1_y[(size_t)(win_y0+yy)*w + (win_x0+xx)];
                }
            int cx0 = bx*4 - win_x0;
            int cy0 = by*4 - win_y0;

            std::vector<double> ms_score(pool.size(), -1e30);
            for (int c = 0; c < (int)pool.size(); ++c) {
                color_rgba dec[16]; decode_block(pool[c].blk, dec);
                std::vector<double> cand_win = base_win;
                for (int yy = 0; yy < 4; ++yy)
                    for (int xx = 0; xx < 4; ++xx)
                        cand_win[(size_t)(cy0+yy)*win_w + (cx0+xx)] = y601(dec[yy*4+xx]);
                ms_score[c] = msssim_y(orig_win, win_w, win_h, cand_win);
            }

            // ---- Guard rail: walk MS-SSIM-desc, commit first that passes
            //      RGB-MSE floor.  If none pass → fall back to pass-1.
            std::vector<int> order(pool.size());
            for (int c = 0; c < (int)pool.size(); ++c) order[c] = c;
            std::sort(order.begin(), order.end(),
                      [&](int a, int b){ return ms_score[a] > ms_score[b]; });

            int chosen = -1;
            int ms_top1 = order[0];
            for (int oi = 0; oi < (int)order.size(); ++oi) {
                int ci = order[oi];
                if (pool[ci].rgb_mse <= rgb_threshold) {
                    chosen = ci;
                    break;
                }
            }
            if (chosen < 0) {
                chosen = pass1_idx;  // safest fallback
                n_floor_no_pass.fetch_add(1, std::memory_order_relaxed);
            }
            // Did the floor disagree with the unconstrained MS-SSIM top-1?
            if (chosen != ms_top1)
                n_floor_kicked_in.fetch_add(1, std::memory_order_relaxed);

            etc_block committed = pool[chosen].blk;
            bool committed_is_pass1 = pool[chosen].is_pass1;

            // ---- Idea #5: multi-table refiner on `committed` ----
            etc_block refined;
            bool refined_changed = false;
            if (refine_iters > 0) {
                refined_changed = multitable_refine(src, committed, refine_iters, refined);
            } else {
                refined = committed;
            }

            if (refined_changed) {
                n_refine_changed.fetch_add(1, std::memory_order_relaxed);

                // Compete refined vs committed under MS-SSIM on same window.
                color_rgba decC[16], decR[16];
                decode_block(committed, decC);
                decode_block(refined,   decR);
                std::vector<double> winC = base_win, winR = base_win;
                for (int yy = 0; yy < 4; ++yy)
                    for (int xx = 0; xx < 4; ++xx) {
                        winC[(size_t)(cy0+yy)*win_w + (cx0+xx)] = y601(decC[yy*4+xx]);
                        winR[(size_t)(cy0+yy)*win_w + (cx0+xx)] = y601(decR[yy*4+xx]);
                    }
                double msC = msssim_y(orig_win, win_w, win_h, winC);
                double msR = msssim_y(orig_win, win_w, win_h, winR);

                double rgbC = block_rgb_mse(src, decC);
                double rgbR = block_rgb_mse(src, decR);

                if (msR > msC) {
                    n_refine_won_msssim.fetch_add(1, std::memory_order_relaxed);
                    committed = refined;
                    if (rgbR < rgbC)
                        n_refine_won_rgb_mse.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // Final commit.  Recompute y_mse for the chosen final.
            color_rgba decF[16]; decode_block(committed, decF);
            blocks[idx] = committed;
            block_err[idx] = block_y_mse(src, decF);

            if (committed_is_pass1)
                n_winner_pass1.fetch_add(1, std::memory_order_relaxed);
            else
                n_winner_pass2.fetch_add(1, std::memory_order_relaxed);
        }
        auto t2 = std::chrono::steady_clock::now();

        fprintf(stderr,
                "v7_guarded_idea5 pass2 %zu worst blocks (>=%.4f Y-MSE), pool_drained=%d, "
                "avg_pool_size=%.2f, winner_pass1=%d, winner_pass2=%d, "
                "floor_kicked_in=%d, floor_no_pass=%d, "
                "refine_changed=%d, refine_won_msssim=%d, refine_won_rgb_mse=%d, "
                "in %.3f s\n",
                worst_idx.size(), thresh,
                n_pool_drained.load(),
                n_pool_drained.load() ? (double)sum_pool_size.load() / (double)n_pool_drained.load() : 0.0,
                n_winner_pass1.load(), n_winner_pass2.load(),
                n_floor_kicked_in.load(), n_floor_no_pass.load(),
                n_refine_changed.load(), n_refine_won_msssim.load(), n_refine_won_rgb_mse.load(),
                std::chrono::duration<double>(t2-t1).count());
        fprintf(stderr, "v7_guarded_idea5 total encode in %.3f s\n",
                std::chrono::duration<double>(t2-t0).count());

        FILE *f = fopen(argv[3], "wb");
        int32_t hdr[2] = {w, h};
        fwrite(hdr, sizeof(hdr), 1, f);
        fwrite(blocks.data(), sizeof(etc_block), blocks.size(), f);
        fclose(f);
        stbi_image_free(img);
    } else if (mode == "decode") {
        FILE *f = fopen(argv[2], "rb");
        int32_t hdr[2]; size_t r = fread(hdr, sizeof(hdr), 1, f); (void)r;
        int w = hdr[0], h = hdr[1];
        int bw = w/4, bh = h/4;
        std::vector<etc_block> blocks(bw * bh);
        r = fread(blocks.data(), sizeof(etc_block), blocks.size(), f); (void)r;
        fclose(f);
        std::vector<uint8_t> img(w * h * 3);
        for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {
            color_rgba out[16]; decode_block(blocks[by*bw + bx], out);
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                uint8_t *p = &img[((by*4 + y)*w + (bx*4 + x))*3];
                p[0] = out[y*4+x].r; p[1] = out[y*4+x].g; p[2] = out[y*4+x].b;
            }
        }
        stbi_write_png(argv[3], w, h, 3, img.data(), w*3);
    }
    return 0;
}
