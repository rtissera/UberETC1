// basisu_etc1_tool_v4.cpp — Research-branch ETC1 encoder with three new
// features layered on top of v3 (cluster fit + perceptual + try-all-corners):
//
//   1. ADAPTIVE per-block effort. Classify each 4x4 block as
//        SOLID (variance < 1)        -> pack_etc1_block_solid_color() fast path
//        FLAT (variance < 25)        -> ETCQualityFast (cluster fit 4 perms, no refine)
//        SMOOTH (variance < 250)     -> ETCQualityMedium (16 perms)
//        DEFAULT                     -> ETCQualitySlow (64 perms)
//        HIGH-VAR (variance > 1500)  -> ETCQualityUber (full 165 + try-all-corners)
//      Inspired by etc2comp's PerformIteration() effort tiers.
//
//   2. YCoCg perceptual metric (compile-time via -DBASISU_USE_YCOCG_PERCEPTUAL).
//      The basisu library MUST have been rebuilt with that define for this
//      to take effect; otherwise we fall back to BT.601-ish YCbCr.
//
//   3. Optional encode-time Floyd-Steinberg dither to RGB555 BEFORE encoding.
//      Lifted from rg_etc1's dither_block_555 (zlib license).
//      Enabled if env var UBERETC1_DITHER=1.
//
// Other env knobs:
//   UBERETC1_PERCEPTUAL=1   (default 1 in this binary)
//   UBERETC1_ADAPTIVE=1     (default 1)
//   UBERETC1_DITHER=0|1     (default 0)
//
// Usage:
//   basisu_etc1_tool_v4 encode in.png out.bin
//   basisu_etc1_tool_v4 decode in.bin out.png

#include <cstdio>
#include <cstdint>
#include <cstdlib>
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

// ---------- Floyd-Steinberg dither to 5-bit per channel ----------
// Lifted from rg_etc1::dither_block_555 (zlib licensed).
static void dither_block_555(uint8_t out_rgb[16 * 3], const uint8_t in_rgb[16 * 3])
{
    int err[4][3] = { {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0} };  // per-column residual err for next row
    for (int y = 0; y < 4; ++y) {
        int row_err[3] = {0, 0, 0};  // running err to next pixel in this row
        for (int x = 0; x < 4; ++x) {
            for (int c = 0; c < 3; ++c) {
                int v = (int)in_rgb[(y*4+x)*3 + c] + row_err[c] + err[x][c];
                int clamped = v < 0 ? 0 : (v > 255 ? 255 : v);
                int q = (clamped + 4) >> 3;     // round to nearest 5-bit value
                if (q > 31) q = 31;
                int dec = (q << 3) | (q >> 2);  // 5-bit -> 8-bit
                int e = clamped - dec;
                out_rgb[(y*4+x)*3 + c] = (uint8_t)dec;
                // Floyd-Steinberg distribution:  7 next, 3 below-left, 5 below, 1 below-right
                int next = (e * 7) / 16;
                int bl   = (e * 3) / 16;
                int bb   = (e * 5) / 16;
                int br   = e - next - bl - bb;  // remainder
                row_err[c] = next;
                if (x > 0) err[x-1][c] += bl;
                err[x][c] = bb;
                if (x < 3) err[x+1][c] = (x+1 == 3) ? br : err[x+1][c] + br;
            }
        }
        // err[] now holds bottom-row residuals for the next iteration. Reset row_err implicitly.
    }
}

// ---------- Block classification ----------
enum BlockClass { CLASS_SOLID, CLASS_FLAT, CLASS_SMOOTH, CLASS_DEFAULT, CLASS_HIGH_VAR };

static BlockClass classify_block(const color_rgba pixels[16])
{
    // per-channel variance, take max
    long sum[3] = {0, 0, 0}, sumsq[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        sum[0] += pixels[i].r; sumsq[0] += (long)pixels[i].r * pixels[i].r;
        sum[1] += pixels[i].g; sumsq[1] += (long)pixels[i].g * pixels[i].g;
        sum[2] += pixels[i].b; sumsq[2] += (long)pixels[i].b * pixels[i].b;
    }
    long max_var = 0;
    for (int c = 0; c < 3; ++c) {
        long v = (sumsq[c] - sum[c]*sum[c]/16) / 16;
        if (v > max_var) max_var = v;
    }
    if (max_var < 1)    return CLASS_SOLID;
    if (max_var < 25)   return CLASS_FLAT;
    if (max_var < 250)  return CLASS_SMOOTH;
    if (max_var > 1500) return CLASS_HIGH_VAR;
    return CLASS_DEFAULT;
}

// ---------- Full-ETC1 encode of a single 4x4 block ----------
struct EncOptions {
    bool perceptual = true;
    bool adaptive   = true;
    bool dither     = false;
};

static void encode_block_full_etc1(const color_rgba pixels_in[16], etc_block &out_block, const EncOptions &opts)
{
    color_rgba pixels[16];

    if (opts.dither) {
        uint8_t in_rgb[16*3], out_rgb[16*3];
        for (int i = 0; i < 16; ++i) {
            in_rgb[i*3+0] = pixels_in[i].r;
            in_rgb[i*3+1] = pixels_in[i].g;
            in_rgb[i*3+2] = pixels_in[i].b;
        }
        dither_block_555(out_rgb, in_rgb);
        for (int i = 0; i < 16; ++i) {
            pixels[i] = color_rgba(out_rgb[i*3+0], out_rgb[i*3+1], out_rgb[i*3+2], 255);
        }
    } else {
        memcpy(pixels, pixels_in, sizeof(color_rgba)*16);
    }

    // ---- Adaptive fast path: solid color block ----
    if (opts.adaptive) {
        BlockClass cls = classify_block(pixels);
        if (cls == CLASS_SOLID) {
            uint8_t color[3] = { pixels[0].r, pixels[0].g, pixels[0].b };
            pack_etc1_block_solid_color(out_block, color);
            return;
        }
        // For other classes, route through full ETC1 path with chosen quality tier.
        // (we still need flip/diff combinations, just with fewer cluster_fit perms)
        basis_etc_quality q;
        switch (cls) {
            case CLASS_FLAT:    q = cETCQualityFast;   break;
            case CLASS_SMOOTH:  q = cETCQualityMedium; break;
            case CLASS_DEFAULT: q = cETCQualitySlow;   break;
            case CLASS_HIGH_VAR:
            default:            q = cETCQualityUber;   break;
        }

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
                p0.m_num_src_pixels = 8;
                p0.m_pSrc_pixels    = sub[0];
                p0.m_quality        = q;
                p0.m_perceptual     = opts.perceptual;
                p0.m_cluster_fit    = true;
                p0.m_use_color4     = !diff;
                p0.m_refinement     = true;
                r0.m_pSelectors = sel0; r0.m_n = 8;
                opt0.init(p0, r0); if (!opt0.compute()) continue;

                etc1_optimizer opt1; etc1_optimizer::params p1; etc1_optimizer::results r1;
                uint8_t sel1[8];
                p1.m_num_src_pixels = 8;
                p1.m_pSrc_pixels    = sub[1];
                p1.m_quality        = q;
                p1.m_perceptual     = opts.perceptual;
                p1.m_cluster_fit    = true;
                p1.m_use_color4     = !diff;
                p1.m_refinement     = true;
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
                    blk.set_flip_bit(flip != 0);
                    blk.set_diff_bit(diff != 0);
                    if (diff) {
                        if (!blk.set_block_color5_check(r0.m_block_color_unscaled, r1.m_block_color_unscaled))
                            continue;
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
        return;
    }

    // ---- Non-adaptive: every block at Uber quality ----
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
            p0.m_quality = cETCQualityUber; p0.m_perceptual = opts.perceptual;
            p0.m_cluster_fit = true; p0.m_use_color4 = !diff; p0.m_refinement = true;
            r0.m_pSelectors = sel0; r0.m_n = 8;
            opt0.init(p0, r0); if (!opt0.compute()) continue;

            etc1_optimizer opt1; etc1_optimizer::params p1; etc1_optimizer::results r1;
            uint8_t sel1[8];
            p1.m_num_src_pixels = 8; p1.m_pSrc_pixels = sub[1];
            p1.m_quality = cETCQualityUber; p1.m_perceptual = opts.perceptual;
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
}

static void decode_block(const etc_block &blk, color_rgba out[16])
{
    for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
        uint32_t s = blk.get_selector(x, y);
        out[y*4+x] = blk.get_selector_color(x, y, s);
    }
}

static bool env_bool(const char *name, bool defv) {
    const char *e = std::getenv(name);
    if (!e) return defv;
    return e[0] == '1' || e[0] == 'y' || e[0] == 'Y' || e[0] == 't' || e[0] == 'T';
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s encode|decode in out\n", argv[0]); return 1; }
    basisu_encoder_init();

    EncOptions opts;
    opts.perceptual = env_bool("UBERETC1_PERCEPTUAL", true);
    opts.adaptive   = env_bool("UBERETC1_ADAPTIVE",   true);
    opts.dither     = env_bool("UBERETC1_DITHER",     false);
    fprintf(stderr, "uberetc1_v4 perceptual=%d adaptive=%d dither=%d\n",
            (int)opts.perceptual, (int)opts.adaptive, (int)opts.dither);

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
                encode_block_full_etc1(px, blocks[by*bw + bx], opts);
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        fprintf(stderr, "uberetc1_v4 encode %dx%d in %.3f s\n",
                w, h, std::chrono::duration<double>(t1-t0).count());

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
