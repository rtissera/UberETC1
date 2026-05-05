// basisu_etc1_tool_v5.cpp — Idea #4: importance-driven worst-N re-encoding.
//
// Pass 1 (every block): cluster_fit + Uber + perceptual YCbCr + try-all-corners
//   (= identical to basisu_v3_corners_perc, the current SOTA baseline).
// Pass 2 (worst 5% of blocks by per-block reconstructed Y-MSE):
//   Re-encode with widened cluster_fit corner search:
//   g_uberetc1_wide_corner_radius = 2 → cube {-2..+2}^3 = 125 corners around
//   each of cluster_fit's 165 LS centers (vs 8 corners by default).
//   evaluate_solution_slow() already enumerates all 8 inten tables internally,
//   so this covers the spec'd 165 × 125 × 8 × 4 (flip,diff) candidate set.
//
//   For each worst block, the pass-2 result is kept iff it has lower Y-MSE
//   on the *decoded* block than pass 1 (so PSNR_Y can only go up or stay flat).
//
// Quality-only — encode time irrelevant. Does not affect non-worst blocks.
//
// Env knobs:
//   UBERETC1_WORST_PCT (default 5.0)   — top X% of blocks get pass 2.
//   UBERETC1_WORST_RADIUS (default 2)  — cube radius for the wide corner search.
//
// Usage:
//   basisu_etc1_tool_v5 encode in.png out.bin
//   basisu_etc1_tool_v5 decode in.bin out.png

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <atomic>

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

// Y-MSE (BT.601 luma) on a reconstructed 4x4 block vs source.
// This is the metric used to rank worst-N and to pick pass1 vs pass2,
// matching the bench's PSNR_Y measurement.
static double block_y_mse(const color_rgba src[16], const color_rgba dec[16])
{
    double acc = 0.0;
    for (int i = 0; i < 16; ++i) {
        double ay = 0.299 * src[i].r + 0.587 * src[i].g + 0.114 * src[i].b;
        double by = 0.299 * dec[i].r + 0.587 * dec[i].g + 0.114 * dec[i].b;
        double d = ay - by;
        acc += d * d;
    }
    return acc / 16.0;
}

// Encode one 4x4 block at full Uber cluster_fit with perceptual YCbCr.
// (Identical to basisu_v3_corners_perc when wide_radius_override <= 0.)
// If wide_radius_override >= 1, sets the thread-local widening for this call.
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
        fprintf(stderr, "v5_idea4 worst_pct=%.2f radius=%d\n", worst_pct, radius);

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
        fprintf(stderr, "v5_idea4 pass1 %dx%d in %.3f s\n",
                w, h, std::chrono::duration<double>(t1-t0).count());

        // ---- Pass 2: identify worst N% by Y-MSE, re-encode with wider corners ----
        std::vector<int> worst_idx;
        const double pct_clamped = std::max(0.0, std::min(100.0, worst_pct));
        const int target_count = (int)((pct_clamped / 100.0) * nblk + 0.5);
        double thresh = -1.0;  // sentinel for "no work"
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

        std::atomic<int> n_improved{0};
        double sum_old = 0.0, sum_new = 0.0;
        // Compute pass-1 totals over worst set for reporting.
        for (int i : worst_idx) sum_old += block_err[i];

        #pragma omp parallel for schedule(dynamic)
        for (size_t k = 0; k < worst_idx.size(); ++k) {
            int idx = worst_idx[k];
            etc_block alt;
            encode_block_full_etc1(src_pix[idx].data(), alt, /*wide_radius_override=*/radius);
            color_rgba dec[16]; decode_block(alt, dec);
            double new_err = block_y_mse(src_pix[idx].data(), dec);
            // Take the better of pass-1 and pass-2 (under Y-MSE).
            if (new_err < block_err[idx]) {
                blocks[idx] = alt;
                block_err[idx] = new_err;
                n_improved.fetch_add(1, std::memory_order_relaxed);
            }
        }
        auto t2 = std::chrono::steady_clock::now();
        for (int i : worst_idx) sum_new += block_err[i];

        fprintf(stderr,
                "v5_idea4 pass2 %zu worst blocks (>=%.4f Y-MSE), radius=%d, "
                "improved=%d, sum_Y_MSE worst-set: %.2f -> %.2f, in %.3f s\n",
                worst_idx.size(), thresh, radius,
                n_improved.load(), sum_old, sum_new,
                std::chrono::duration<double>(t2-t1).count());
        fprintf(stderr, "v5_idea4 total encode in %.3f s\n",
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
