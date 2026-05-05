// basisu_etc1_tool_v6.cpp — Idea #2 stacked on Idea #4 (MS-SSIM top-K rerank
// on worst-N blocks). Builds on v5_idea4.
//
// Pipeline:
//   Pass 1 (every block): cluster_fit + Uber + perceptual YCbCr +
//     try-all-corners (8 corners) — identical to basisu_v3_corners_perc /
//     v5_idea4 pass 1. Records per-block reconstructed Y-MSE.
//
//   Pass 2 (worst UBERETC1_WORST_PCT% of blocks by pass-1 Y-MSE):
//     a) Generate K candidates per worst block by encoding it 8 different
//        ways: cross-product of {flip in {0,1}} × {diff in {0,1}} × {wide
//        corner radius in {1,2}}. Each (flip, diff, radius) tuple commits
//        to one (flip, diff) and runs cluster_fit at radius=R, yielding the
//        MSE-best block for that parameter slice. = 8 candidates.
//     b) Add the pass-1 encoded block as the 9th candidate.
//     c) Rerank these 9 candidates by MS-SSIM_Y on a 12×12 (3×3 block)
//        window centered on the candidate, using already-pass-1-encoded
//        neighbors as context. Boundary blocks shrink the window naturally.
//     d) Pick the candidate with highest MS-SSIM_Y.
//
// Notes:
//   - Multi-scale SSIM evaluated at scales 1, 1/2, 1/4 with 2x2 average
//     downsampling between scales, geometrically combined.
//   - Pass-1 ETC1 decode is block-independent; neighbor decodes are stable.
//     We compute the pass-1 decoded Y plane once and reuse.
//   - GPU validation (gpu_validate.py) must show 0 mean abs diff vs SW
//     decoder — the rerank only mutates a few worst-block bitstreams.
//
// Env knobs:
//   UBERETC1_WORST_PCT      (default 5.0)   — top X% of blocks get pass 2.
//   UBERETC1_WORST_RADIUS   (default 2)     — primary cube radius (also
//                                              candidate generation uses {1,2}).
//
// Usage:
//   basisu_etc1_tool_v6 encode in.png out.bin
//   basisu_etc1_tool_v6 decode in.bin out.png

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

// Encode one 4x4 block at full Uber cluster_fit with perceptual YCbCr.
// (Matches v5: when wide_radius_override < 0, identical to basisu_v3_corners_perc.)
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

// Encode one 4x4 block restricted to a single (flip_force, diff_force) tuple,
// at the given wide corner radius. Returns false if the (flip, diff) couldn't
// produce a valid block (rare — diff color5 constraint failure).
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

    int flip = flip_force;
    int diff = diff_force;

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

// ----- MS-SSIM_Y on a small window -----
// Standard SSIM with C1, C2 from the reference paper, on luminance values in
// 0..255.  Computes mean SSIM over the window (no Gaussian — uniform mean).
static double ssim_y(const std::vector<double> &a,
                     const std::vector<double> &b)
{
    if (a.empty()) return 1.0;
    const size_t n = a.size();
    double mu_a = 0, mu_b = 0;
    for (size_t i = 0; i < n; ++i) { mu_a += a[i]; mu_b += b[i]; }
    mu_a /= n; mu_b /= n;
    double var_a = 0, var_b = 0, cov = 0;
    for (size_t i = 0; i < n; ++i) {
        double da = a[i] - mu_a, db = b[i] - mu_b;
        var_a += da*da;
        var_b += db*db;
        cov   += da*db;
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

// 2x2 box-filter downsample of an HxW Y plane into a (H/2)x(W/2) plane.
// Crops the last row/col if the dim is odd.
static void downsample_2x_y(const std::vector<double> &in, int W, int H,
                            std::vector<double> &out, int &outW, int &outH)
{
    outW = W / 2; outH = H / 2;
    out.assign((size_t)outW * (size_t)outH, 0.0);
    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            double s =
                in[(2*y + 0)*W + 2*x + 0] +
                in[(2*y + 0)*W + 2*x + 1] +
                in[(2*y + 1)*W + 2*x + 0] +
                in[(2*y + 1)*W + 2*x + 1];
            out[y*outW + x] = s * 0.25;
        }
    }
}

// Multi-scale SSIM_Y. 3 scales: full, /2, /4. Combined as geometric mean.
// If a downsampled scale would be smaller than 2x2, that scale is skipped.
static double msssim_y(const std::vector<double> &orig_y, int W, int H,
                       const std::vector<double> &cand_y)
{
    double prod = 1.0;
    int n_scales = 0;

    // Scale 1: full
    {
        double s = ssim_y(orig_y, cand_y);
        if (s < 0) s = 0;  // SSIM in [-1,1]; clamp for geo-mean
        prod *= std::max(s, 1e-6);
        n_scales++;
    }
    // Scale 2: /2
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
        // Scale 3: /4
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
    const char *e = std::getenv(name);
    if (!e) return defv;
    return std::strtod(e, nullptr);
}
static int env_int(const char *name, int defv) {
    const char *e = std::getenv(name);
    if (!e) return defv;
    return std::atoi(e);
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s encode|decode in out\n", argv[0]); return 1; }
    basisu_encoder_init();

    std::string mode = argv[1];
    if (mode == "encode") {
        const double worst_pct = env_double("UBERETC1_WORST_PCT", 5.0);
        const int    radius    = env_int("UBERETC1_WORST_RADIUS", 2);
        fprintf(stderr, "v6_idea2 worst_pct=%.2f primary_radius=%d (msssim rerank stacked on idea#4)\n",
                worst_pct, radius);

        int w, h, n;
        unsigned char *img = stbi_load(argv[2], &w, &h, &n, 4);
        if (!img) { fprintf(stderr, "load fail\n"); return 1; }
        if ((w & 3) || (h & 3)) { fprintf(stderr, "bad dims\n"); return 1; }
        const int bw = w / 4, bh = h / 4;
        const int nblk = bw * bh;
        std::vector<etc_block> blocks(nblk);
        std::vector<double>    block_err(nblk, 0.0);

        // Cache source pixels per block for re-use in pass 2.
        std::vector<std::array<color_rgba, 16>> src_pix(nblk);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                color_rgba *px = src_pix[by * bw + bx].data();
                for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                    const uint8_t *p = &img[((by*4 + y)*w + (bx*4 + x))*4];
                    px[y*4+x] = color_rgba(p[0], p[1], p[2], 255);
                }
            }

        // ---- Pass 1: standard Uber cluster_fit + try-all-corners + perceptual ----
        auto t0 = std::chrono::steady_clock::now();
        #pragma omp parallel for schedule(dynamic) collapse(2)
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                int idx = by * bw + bx;
                encode_block_full_etc1(src_pix[idx].data(), blocks[idx], /*wide_radius_override=*/-1);
                color_rgba dec[16]; decode_block(blocks[idx], dec);
                block_err[idx] = block_y_mse(src_pix[idx].data(), dec);
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        fprintf(stderr, "v6_idea2 pass1 %dx%d in %.3f s\n",
                w, h, std::chrono::duration<double>(t1-t0).count());

        // ---- Build pass-1 decoded Y plane (block-grid) ----
        // Y stored as a (h x w) plane of doubles (BT.601 from pass-1 decode).
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
        // Source Y plane.
        std::vector<double> src_y((size_t)w * (size_t)h, 0.0);
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) {
                const uint8_t *p = &img[(yy*w + xx)*4];
                src_y[(size_t)yy*w + xx] =
                    0.299*p[0] + 0.587*p[1] + 0.114*p[2];
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
            for (int i = 0; i < nblk; ++i) {
                if (block_err[i] >= thresh) worst_idx.push_back(i);
            }
        }

        // Counters for reporting.
        std::atomic<int> n_pass1_kept{0};
        std::atomic<int> n_pass2_kept{0};        // any wider candidate replaced pass 1
        std::atomic<int> n_msssim_diverged{0};   // msssim picked a non-MSE-best candidate
        std::atomic<int> n_y_mse_regressed{0};   // chosen candidate has worse Y-MSE than pass-1
        std::atomic<int> n_candidates_total{0};

        // Candidate parameter grid: 4 (flip,diff) × 2 radii ∈ {1, primary_radius}.
        // If primary_radius == 1 we collapse to {1, 2}.
        int radii[2] = {1, std::max(2, radius)};
        if (radii[0] == radii[1]) radii[1] = radii[0] + 1;

        #pragma omp parallel for schedule(dynamic)
        for (size_t k = 0; k < worst_idx.size(); ++k) {
            int idx = worst_idx[k];
            int by = idx / bw, bx = idx % bw;

            // Build candidate list:
            //   - For each (flip, diff, radius) generate one candidate (8 total)
            //   - Plus the pass-1 result as candidate #9.
            constexpr int MAX_CANDS = 9;
            etc_block cand_blk[MAX_CANDS];
            uint64_t  cand_perc_err[MAX_CANDS];   // sub-block perceptual err sum (for ranking top-K by MSE)
            double    cand_y_mse[MAX_CANDS];
            bool      cand_valid[MAX_CANDS] = {false};
            int       n_cands = 0;

            for (int flip = 0; flip < 2; ++flip) {
                for (int diff = 0; diff < 2; ++diff) {
                    for (int ri = 0; ri < 2; ++ri) {
                        int rr = radii[ri];
                        etc_block bk; uint64_t err = 0;
                        if (encode_block_one_mode(src_pix[idx].data(),
                                                  flip, diff, rr, bk, err))
                        {
                            color_rgba dec[16]; decode_block(bk, dec);
                            cand_blk[n_cands] = bk;
                            cand_perc_err[n_cands] = err;
                            cand_y_mse[n_cands] = block_y_mse(src_pix[idx].data(), dec);
                            cand_valid[n_cands] = true;
                            n_cands++;
                        }
                    }
                }
            }
            // Pass-1 result as candidate K+1.
            {
                color_rgba dec[16]; decode_block(blocks[idx], dec);
                cand_blk[n_cands] = blocks[idx];
                // We don't have a stored perceptual err for pass 1; use Y-MSE as proxy
                // (only the rerank happens here, perc_err is only for sorting top-K).
                cand_perc_err[n_cands] = (uint64_t)(block_y_mse(src_pix[idx].data(), dec) * 16.0 + 0.5);
                cand_y_mse[n_cands] = block_y_mse(src_pix[idx].data(), dec);
                cand_valid[n_cands] = true;
                n_cands++;
            }
            n_candidates_total.fetch_add(n_cands, std::memory_order_relaxed);

            // Identify the MSE-best candidate (by reconstructed Y-MSE, the metric
            // we ultimately optimize against the bench).
            int mse_best = -1; double best_mse = 1e30;
            for (int c = 0; c < n_cands; ++c) {
                if (cand_valid[c] && cand_y_mse[c] < best_mse) {
                    best_mse = cand_y_mse[c]; mse_best = c;
                }
            }

            // ---- MS-SSIM rerank ----
            // 3x3-block window at (bx, by). Crop to neighbors that exist.
            int win_x0 = (bx > 0)   ? (bx - 1)*4 : bx*4;
            int win_y0 = (by > 0)   ? (by - 1)*4 : by*4;
            int win_x1 = (bx+1 < bw) ? (bx + 2)*4 : (bx+1)*4;
            int win_y1 = (by+1 < bh) ? (by + 2)*4 : (by+1)*4;
            int win_w  = win_x1 - win_x0;
            int win_h  = win_y1 - win_y0;

            // Original Y in window.
            std::vector<double> orig_win((size_t)win_w * (size_t)win_h);
            for (int yy = 0; yy < win_h; ++yy)
                for (int xx = 0; xx < win_w; ++xx)
                    orig_win[(size_t)yy*win_w + xx] = src_y[(size_t)(win_y0+yy)*w + (win_x0+xx)];

            // Pass-1 Y for whole window (will later overwrite center 4x4 with
            // each candidate's decoded Y).
            std::vector<double> base_win((size_t)win_w * (size_t)win_h);
            for (int yy = 0; yy < win_h; ++yy)
                for (int xx = 0; xx < win_w; ++xx)
                    base_win[(size_t)yy*win_w + xx] = pass1_y[(size_t)(win_y0+yy)*w + (win_x0+xx)];

            // Center of this candidate's block within the window.
            int cx0 = bx*4 - win_x0;
            int cy0 = by*4 - win_y0;

            int ms_best = -1; double best_ms = -1e30;
            for (int c = 0; c < n_cands; ++c) {
                if (!cand_valid[c]) continue;
                color_rgba dec[16]; decode_block(cand_blk[c], dec);
                std::vector<double> cand_win = base_win;  // copy
                for (int yy = 0; yy < 4; ++yy)
                    for (int xx = 0; xx < 4; ++xx)
                        cand_win[(size_t)(cy0+yy)*win_w + (cx0+xx)] = y601(dec[yy*4+xx]);
                double ms = msssim_y(orig_win, win_w, win_h, cand_win);
                if (ms > best_ms) { best_ms = ms; ms_best = c; }
            }

            // Decision: pick MS-SSIM winner. Always commit (pass-1 is one of the candidates).
            if (ms_best >= 0) {
                if (ms_best != mse_best) n_msssim_diverged.fetch_add(1, std::memory_order_relaxed);
                bool was_pass1 = (ms_best == n_cands - 1);
                if (was_pass1) n_pass1_kept.fetch_add(1, std::memory_order_relaxed);
                else           n_pass2_kept.fetch_add(1, std::memory_order_relaxed);

                // If MS-SSIM picked a candidate with worse Y-MSE than pass-1, count it.
                double pass1_y_mse = cand_y_mse[n_cands - 1];
                if (cand_y_mse[ms_best] > pass1_y_mse + 1e-9)
                    n_y_mse_regressed.fetch_add(1, std::memory_order_relaxed);

                blocks[idx] = cand_blk[ms_best];
                block_err[idx] = cand_y_mse[ms_best];
            }
        }
        auto t2 = std::chrono::steady_clock::now();

        fprintf(stderr,
                "v6_idea2 pass2 %zu worst blocks (>=%.4f Y-MSE), candidates_total=%d "
                "(avg %.1f/blk), kept_pass1=%d, kept_pass2=%d, "
                "msssim_diverged_from_mse=%d, y_mse_regressed_vs_pass1=%d, in %.3f s\n",
                worst_idx.size(), thresh, n_candidates_total.load(),
                worst_idx.empty() ? 0.0 : (double)n_candidates_total.load() / (double)worst_idx.size(),
                n_pass1_kept.load(), n_pass2_kept.load(),
                n_msssim_diverged.load(), n_y_mse_regressed.load(),
                std::chrono::duration<double>(t2-t1).count());
        fprintf(stderr, "v6_idea2 total encode in %.3f s\n",
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
