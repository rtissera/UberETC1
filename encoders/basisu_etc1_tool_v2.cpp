// basisu_etc1_tool.cpp — Full ETC1 encoder using basis_universal's
// etc1_optimizer with cluster_fit + cETCQualityUber, evaluating BOTH flip
// orientations and BOTH (color4 individual) and (color5 differential) modes.
// This is the closest available approximation to "basislib SOTA full ETC1".
//
// Usage:
//   basisu_etc1_tool encode in.png out.bin
//   basisu_etc1_tool decode in.bin out.png

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "basis_universal/encoder/basisu_etc.h"
#include "basis_universal/encoder/basisu_enc.h"

using namespace basisu;

static void encode_block_full_etc1(const color_rgba pixels[16], etc_block &out_block)
{
    // Try each (flip, diff) combination. For each flip, split block into two
    // sub-blocks of 8 pixels (either 2x4 or 4x2 layout).
    // Use cluster_fit + cETCQualityUber.

    uint64_t best_err = ~0ULL;
    etc_block best_blk;

    for (int flip = 0; flip < 2; ++flip) {
        for (int diff = 0; diff < 2; ++diff) {
            // Layout sub-blocks
            color_rgba sub[2][8];
            for (int s = 0; s < 2; ++s) {
                for (int i = 0; i < 8; ++i) {
                    int x, y;
                    if (flip) {
                        // 4 wide x 2 tall sub-blocks: top half = sub0
                        x = i & 3;
                        y = (i >> 2) + s * 2;
                    } else {
                        // 2 wide x 4 tall sub-blocks: left half = sub0
                        x = (i >> 2) + s * 2;
                        y = i & 3;
                    }
                    sub[s][i] = pixels[y * 4 + x];
                }
            }

            // Optimize sub-block 0 first (unconstrained).
            etc1_optimizer opt0;
            etc1_optimizer::params p0;
            etc1_optimizer::results r0;
            uint8_t sel0[8];

            p0.m_num_src_pixels = 8;
            p0.m_pSrc_pixels = sub[0];
            p0.m_quality = cETCQualityUber;
            p0.m_perceptual = true; // RGB-domain MSE for fair comparison
            p0.m_cluster_fit = true;
            p0.m_use_color4 = !diff;
            p0.m_refinement = true;

            r0.m_pSelectors = sel0;
            r0.m_n = 8;

            opt0.init(p0, r0);
            if (!opt0.compute()) continue;

            // Sub-block 1: if diff mode, must be within ±3 of sub0 base color (5-bit).
            etc1_optimizer opt1;
            etc1_optimizer::params p1;
            etc1_optimizer::results r1;
            uint8_t sel1[8];

            p1.m_num_src_pixels = 8;
            p1.m_pSrc_pixels = sub[1];
            p1.m_quality = cETCQualityUber;
            p1.m_perceptual = true;
            p1.m_cluster_fit = true;
            p1.m_use_color4 = !diff;
            p1.m_refinement = true;
            if (diff) {
                p1.m_constrain_against_base_color5 = true;
                p1.m_base_color5 = r0.m_block_color_unscaled;
            }
            r1.m_pSelectors = sel1;
            r1.m_n = 8;

            opt1.init(p1, r1);
            if (!opt1.compute()) continue;

            uint64_t err = r0.m_error + r1.m_error;
            if (err < best_err) {
                best_err = err;
                etc_block blk;
                memset(&blk, 0, sizeof(blk));
                blk.set_flip_bit(flip != 0);
                blk.set_diff_bit(diff != 0);

                if (diff) {
                    if (!blk.set_block_color5_check(r0.m_block_color_unscaled, r1.m_block_color_unscaled)) {
                        // delta out of range — shouldn't happen with constrain, but skip
                        continue;
                    }
                } else {
                    blk.set_block_color4(r0.m_block_color_unscaled, r1.m_block_color_unscaled);
                }
                blk.set_inten_table(0, r0.m_block_inten_table);
                blk.set_inten_table(1, r1.m_block_inten_table);

                // Place selectors back into 4x4 layout
                for (int s = 0; s < 2; ++s) {
                    const uint8_t *sel = (s == 0) ? sel0 : sel1;
                    for (int i = 0; i < 8; ++i) {
                        int x, y;
                        if (flip) {
                            x = i & 3;
                            y = (i >> 2) + s * 2;
                        } else {
                            x = (i >> 2) + s * 2;
                            y = i & 3;
                        }
                        blk.set_selector(x, y, sel[i]);
                    }
                }
                best_blk = blk;
            }
        }
    }

    out_block = best_blk;
}

static void decode_block(const etc_block &blk, color_rgba out[16])
{
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            uint32_t s = blk.get_selector(x, y);
            out[y*4+x] = blk.get_selector_color(x, y, s);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s encode|decode in out\n", argv[0]); return 1; }

    basisu_encoder_init();

    std::string mode = argv[1];

    if (mode == "encode") {
        int w, h, n;
        unsigned char *img = stbi_load(argv[2], &w, &h, &n, 4);
        if (!img) { fprintf(stderr, "load fail\n"); return 1; }
        if ((w & 3) || (h & 3)) { fprintf(stderr, "bad dims\n"); return 1; }
        int bw = w/4, bh = h/4;
        std::vector<etc_block> blocks(bw * bh);

        auto t0 = std::chrono::steady_clock::now();
        #pragma omp parallel for schedule(dynamic) collapse(2)
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                color_rgba px[16];
                for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                    const uint8_t *p = &img[((by*4 + y)*w + (bx*4 + x))*4];
                    px[y*4+x] = color_rgba(p[0], p[1], p[2], 255);
                }
                encode_block_full_etc1(px, blocks[by*bw + bx]);
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        fprintf(stderr, "basisu_full_etc1_perceptual encode %dx%d in %.3f s\n",
                w, h, std::chrono::duration<double>(t1-t0).count());

        FILE *f = fopen(argv[3], "wb");
        int32_t hdr[2] = {w, h};
        fwrite(hdr, sizeof(hdr), 1, f);
        fwrite(blocks.data(), sizeof(etc_block), blocks.size(), f);
        fclose(f);
        stbi_image_free(img);
    } else if (mode == "decode") {
        FILE *f = fopen(argv[2], "rb");
        int32_t hdr[2];
        size_t r = fread(hdr, sizeof(hdr), 1, f); (void)r;
        int w = hdr[0], h = hdr[1];
        int bw = w/4, bh = h/4;
        std::vector<etc_block> blocks(bw * bh);
        r = fread(blocks.data(), sizeof(etc_block), blocks.size(), f); (void)r;
        fclose(f);
        std::vector<uint8_t> img(w * h * 3);
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                color_rgba out[16];
                decode_block(blocks[by*bw + bx], out);
                for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                    uint8_t *p = &img[((by*4 + y)*w + (bx*4 + x))*3];
                    p[0] = out[y*4+x].r;
                    p[1] = out[y*4+x].g;
                    p[2] = out[y*4+x].b;
                }
            }
        }
        stbi_write_png(argv[3], w, h, 3, img.data(), w*3);
    }
    return 0;
}
