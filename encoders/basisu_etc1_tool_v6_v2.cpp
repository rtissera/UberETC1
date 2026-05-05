// basisu_etc1_tool_v6_v2.cpp — Idea #2 v2 (REAL top-K capture inside the
// optimizer + MS-SSIM rerank), stacked on idea #4.
//
// Differences vs basisu_etc1_tool_v6 (v6_idea2):
//   v6_idea2 approximates "top-K candidates" with a parameter-grid sweep:
//     {flip ∈ {0,1}} × {diff ∈ {0,1}} × {radius ∈ {1,2}} = 8 candidates,
//     each MSE-best for that slice (cluster_fit's top-1). + pass-1 = 9.
//   v6_idea2_v2 actually drains the global top-K from the cluster-fit
//   optimizer — every (color, inten, selectors) trial inside
//   evaluate_solution_slow() goes into a thread-local bounded heap of
//   size K=8.  Driver pulls K per sub-block, combines into K×K full-block
//   candidates per (flip, diff), pools global top-K=8 by total perceptual
//   error across all 4 (flip, diff) tuples, MS-SSIM-reranks K+1 (K + pass-1).
//
// Pipeline:
//   Pass 1 (every block): cluster_fit + Uber + perceptual YCbCr + 8-corner
//     (== basisu_v3_corners_perc / v5_idea4 pass-1).  Records per-block Y-MSE.
//   Pass 2 (worst UBERETC1_WORST_PCT% by Y-MSE):
//     For each worst block:
//       - For each (flip, diff) ∈ {0,1}^2:
//         (a) Run sub-block 0's optimizer at radius=R with topk_capture=K.
//             Drain buf0 = up to K (color, inten, selectors, err) entries.
//         (b) For each entry e0 in buf0:
//               If diff: run sub-block 1's optimizer constrained to
//                 base_color5=e0.color, radius=R, topk_capture=K.  Drain
//                 buf1 = up to K entries.
//               If !diff: run sub-block 1's optimizer once (no constraint),
//                 radius=R, topk_capture=K.  Drain buf1 (shared across e0).
//             Each combination (e0, e1) yields a full-block candidate:
//               etc_block built from (flip, diff, e0, e1), perc_err=e0.err+e1.err.
//       - Pool all candidates across (flip, diff) into a global heap of
//         size K=8 keyed by perc_err.
//       - Add pass-1 result as an extra candidate (perc_err proxy = its
//         pass-1 perceptual error, but ranked separately — all K+1 go to
//         MS-SSIM rerank).
//       - MS-SSIM rerank on a 12×12 (3×3 block) window centered on the
//         block, using already-pass-1-encoded neighbors as fixed context.
//       - Pick the MS-SSIM winner.  Commit.
//
// MS-SSIM unchanged from v6_idea2 (3 scales: full, /2, /4 with 2x2 box
// downsample, geometric mean; boundary blocks shrink the window).
//
// Top-K diversity reporting (vs v6_idea2's parameter-grid approximation):
//   - n_msssim_diverged_from_pool_top1: MS-SSIM winner != pool's MSE-top1.
//   - n_winner_outside_v6_grid: MS-SSIM winner's bitstream doesn't match
//     any of the 8 v6-grid candidates (re-encoded for that block) and
//     != pass-1.  This is the test of whether real top-K matters: if
//     this count is high, v6_idea2's grid genuinely missed candidates
//     that v6_idea2_v2 captures.
//
// Env knobs:
//   UBERETC1_WORST_PCT      (default 5.0)   — top X% of blocks get pass 2.
//   UBERETC1_WORST_RADIUS   (default 2)     — cube radius for sub-block opt.
//   UBERETC1_TOPK           (default 8)     — K (per sub-block + global).
//
// Usage:
//   basisu_etc1_tool_v6_v2 encode in.png out.bin
//   basisu_etc1_tool_v6_v2 decode in.bin out.png

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

// Decode a single ETC1 block to its 16 reconstructed RGB pixels.
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

// Y-MSE (BT.601 luma) on a reconstructed 4x4 block vs source.
static double block_y_mse(const color_rgba src[16], const color_rgba dec[16])
{
    double acc = 0.0;
    for (int i = 0; i < 16; ++i) {
        double d = y601(src[i]) - y601(dec[i]);
        acc += d * d;
    }
    return acc / 16.0;
}

// Build a 4x4 etc_block from (flip, diff, e0, e1) where e0/e1 are top-K
// entries (per-sub-block: color_unscaled, inten_table, selectors[8]).
// Returns false if diff-mode color5 delta is out of range.
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

// Encode one 4x4 block at full Uber cluster_fit with perceptual YCbCr.
// (Identical to v3_corners_perc when wide_radius_override <= 0.)
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

// One-shot encode of one (flip, diff) at given radius, returns the v6-style
// MSE-best block for that param slice (used by the diversity stat).
static bool encode_block_one_mode(const color_rgba pixels[16],
                                  int flip_force, int diff_force,
                                  int wide_radius, etc_block &out_block,
                                  uint64_t &out_err)
{
    int saved = g_uberetc1_wide_corner_radius;
    g_uberetc1_wide_corner_radius = wide_radius;

    bool ok = false;
    etc_block blk; memset(&blk, 0, sizeof(blk));
    uint64_t err = 0;
    int flip = flip_force, diff = diff_force;

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
    opt0.init(p0, r0);
    if (opt0.compute()) {
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
        opt1.init(p1, r1);
        if (opt1.compute()) {
            err = r0.m_error + r1.m_error;
            blk.set_flip_bit(flip != 0); blk.set_diff_bit(diff != 0);
            bool color_ok = true;
            if (diff) {
                if (!blk.set_block_color5_check(r0.m_block_color_unscaled, r1.m_block_color_unscaled))
                    color_ok = false;
            } else {
                blk.set_block_color4(r0.m_block_color_unscaled, r1.m_block_color_unscaled);
            }
            if (color_ok) {
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
                ok = true;
            }
        }
    }
    g_uberetc1_wide_corner_radius = saved;
    if (ok) { out_block = blk; out_err = err; }
    return ok;
}

// Run the optimizer on a single sub-block and drain top-K entries.
// On success, dst is filled with up to K entries (size = dst.size()).
// flag wide_radius forwards to the cluster-fit corner search.
// constrain_color5 != nullptr enables diff-mode constraint.
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

// ----- MS-SSIM_Y on a small window (unchanged from v6_idea2) -----
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
        const double worst_pct = env_double("UBERETC1_WORST_PCT", 5.0);
        const int    radius    = env_int("UBERETC1_WORST_RADIUS", 2);
        const int    K         = std::max(1, env_int("UBERETC1_TOPK", 8));
        fprintf(stderr, "v6_idea2_v2 worst_pct=%.2f radius=%d K=%d (real top-K + msssim rerank)\n",
                worst_pct, radius, K);

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

        // ---- Pass 1: identical to v6_idea2 / v5_idea4 / v3_corners_perc ----
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
        fprintf(stderr, "v6_idea2_v2 pass1 %dx%d in %.3f s\n",
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

        // ---- Pass 2: identify worst N% by Y-MSE ----
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
        std::atomic<int> n_pass1_kept{0};
        std::atomic<int> n_pass2_kept{0};
        std::atomic<int> n_msssim_diverged_from_pool_top1{0};
        std::atomic<int> n_y_mse_regressed{0};
        std::atomic<long long> n_candidates_total{0};
        std::atomic<int> n_pool_drained{0};            // worst blocks where pool was non-empty
        std::atomic<int> n_winner_outside_v6_grid{0};  // diversity vs v6_idea2's 9-slice grid
        std::atomic<int> n_winner_pass1{0};
        std::atomic<long long> sum_pool_size{0};

        #pragma omp parallel for schedule(dynamic)
        for (size_t k = 0; k < worst_idx.size(); ++k) {
            int idx = worst_idx[k];
            int by = idx / bw, bx = idx % bw;
            const color_rgba *src = src_pix[idx].data();

            // Build per-(flip,diff) sub-block pixels.
            // pool[]: candidate full blocks with their summed perceptual error.
            struct PoolEnt { etc_block blk; uint64_t perc_err; double y_mse; };
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
                                           /*constrain_color5=*/nullptr, buf0))
                        continue;
                    if (buf0.empty()) continue;

                    if (!diff) {
                        // Sub-block 1 is independent of sub-block 0.
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
                                pe.y_mse = block_y_mse(src, dec);
                                pool.push_back(pe);
                            }
                    } else {
                        // Diff mode: sub-block 1 is constrained by sub-block 0's color.
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
                                pe.y_mse = block_y_mse(src, dec);
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

            // Add pass-1 result as candidate K+1 (always last, with its decoded y_mse).
            {
                color_rgba dec[16]; decode_block(blocks[idx], dec);
                PoolEnt pe;
                pe.blk = blocks[idx];
                pe.perc_err = ~0ULL;  // sentinel: not directly comparable
                pe.y_mse = block_y_mse(src, dec);
                pool.push_back(pe);
            }
            const int pass1_idx = (int)pool.size() - 1;
            n_candidates_total.fetch_add((long long)pool.size(), std::memory_order_relaxed);

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

            int ms_best = -1; double best_ms = -1e30;
            for (int c = 0; c < (int)pool.size(); ++c) {
                color_rgba dec[16]; decode_block(pool[c].blk, dec);
                std::vector<double> cand_win = base_win;
                for (int yy = 0; yy < 4; ++yy)
                    for (int xx = 0; xx < 4; ++xx)
                        cand_win[(size_t)(cy0+yy)*win_w + (cx0+xx)] = y601(dec[yy*4+xx]);
                double ms = msssim_y(orig_win, win_w, win_h, cand_win);
                if (ms > best_ms) { best_ms = ms; ms_best = c; }
            }
            if (ms_best < 0) continue;

            // pool[0] is the MSE-best of the K-pool (after partial_sort by perc_err).
            // pass1 is at pass1_idx.
            if (ms_best != 0 && ms_best != pass1_idx)
                n_msssim_diverged_from_pool_top1.fetch_add(1, std::memory_order_relaxed);

            bool was_pass1 = (ms_best == pass1_idx);
            if (was_pass1) {
                n_pass1_kept.fetch_add(1, std::memory_order_relaxed);
                n_winner_pass1.fetch_add(1, std::memory_order_relaxed);
            } else {
                n_pass2_kept.fetch_add(1, std::memory_order_relaxed);
            }

            // y_mse regression vs pass-1.
            double pass1_y_mse = pool[pass1_idx].y_mse;
            if (pool[ms_best].y_mse > pass1_y_mse + 1e-9)
                n_y_mse_regressed.fetch_add(1, std::memory_order_relaxed);

            // ---- Diversity vs v6_idea2's 9-slice grid ----
            // Encode the 8 grid candidates (flip×diff×radius∈{1,2}) and
            // compare bitstreams with the chosen winner.  If winner doesn't
            // match any of those 8, nor pass-1, it's a candidate v6_idea2
            // would have missed.
            if (!was_pass1) {
                int radii[2] = {1, std::max(2, radius)};
                if (radii[0] == radii[1]) radii[1] = radii[0] + 1;
                bool matched = false;
                for (int fl = 0; fl < 2 && !matched; ++fl)
                    for (int df = 0; df < 2 && !matched; ++df)
                        for (int ri = 0; ri < 2 && !matched; ++ri) {
                            int rr = radii[ri];
                            etc_block gk; uint64_t ge = 0;
                            if (encode_block_one_mode(src, fl, df, rr, gk, ge)) {
                                if (gk.m_uint64 == pool[ms_best].blk.m_uint64) {
                                    matched = true;
                                }
                            }
                        }
                // also pass-1 (different from v6 grid? we already excluded was_pass1)
                if (!matched && blocks[idx].m_uint64 == pool[ms_best].blk.m_uint64)
                    matched = true;
                if (!matched)
                    n_winner_outside_v6_grid.fetch_add(1, std::memory_order_relaxed);
            }

            blocks[idx] = pool[ms_best].blk;
            block_err[idx] = pool[ms_best].y_mse;
        }
        auto t2 = std::chrono::steady_clock::now();

        fprintf(stderr,
                "v6_idea2_v2 pass2 %zu worst blocks (>=%.4f Y-MSE), pool_drained=%d, "
                "candidates_total=%lld (avg %.2f/blk), avg_pool_size=%.2f, "
                "kept_pass1=%d (winner=pass1), kept_pass2=%d, "
                "msssim_diverged_from_pool_top1=%d, y_mse_regressed_vs_pass1=%d, "
                "winner_outside_v6_grid=%d, in %.3f s\n",
                worst_idx.size(), thresh,
                n_pool_drained.load(),
                n_candidates_total.load(),
                worst_idx.empty() ? 0.0 : (double)n_candidates_total.load() / (double)worst_idx.size(),
                n_pool_drained.load() ? (double)sum_pool_size.load() / (double)n_pool_drained.load() : 0.0,
                n_pass1_kept.load(), n_pass2_kept.load(),
                n_msssim_diverged_from_pool_top1.load(),
                n_y_mse_regressed.load(),
                n_winner_outside_v6_grid.load(),
                std::chrono::duration<double>(t2-t1).count());
        fprintf(stderr, "v6_idea2_v2 total encode in %.3f s\n",
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
