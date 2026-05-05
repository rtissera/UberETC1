// rg_etc1_tool.cpp — encode a PNG to ETC1 raw stream, decode back to PNG.
// Uses rg-etc1 (cHighQuality) as the full-ETC1 reference encoder.
//
// Usage:
//   rg_etc1_tool encode in.png out.bin            (writes width,height,raw blocks)
//   rg_etc1_tool decode in.bin out.png

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "rg-etc1/rg_etc1.h"

static void encode_block(const uint8_t* px, void* out)
{
    // rg_etc1 expects RGBA 32bpp. Our buffer is RGB. Expand.
    uint8_t blk[16 * 4];
    for (int i = 0; i < 16; ++i) {
        blk[i*4+0] = px[i*3+0];
        blk[i*4+1] = px[i*3+1];
        blk[i*4+2] = px[i*3+2];
        blk[i*4+3] = 255;
    }
    rg_etc1::etc1_pack_params p;
    p.m_quality = rg_etc1::cHighQuality;
    p.m_dithering = false;
    rg_etc1::pack_etc1_block(out, (unsigned int*)blk, p);
}

static void decode_block(const void* in, uint8_t* out_rgb)
{
    uint32_t blk[16];
    rg_etc1::unpack_etc1_block(in, blk, false);
    for (int i = 0; i < 16; ++i) {
        out_rgb[i*3+0] = (blk[i] >> 0) & 0xFF;
        out_rgb[i*3+1] = (blk[i] >> 8) & 0xFF;
        out_rgb[i*3+2] = (blk[i] >> 16) & 0xFF;
    }
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s encode|decode in out\n", argv[0]);
        return 1;
    }
    rg_etc1::pack_etc1_block_init();

    std::string mode = argv[1];
    if (mode == "encode") {
        int w, h, n;
        unsigned char* img = stbi_load(argv[2], &w, &h, &n, 3);
        if (!img) { fprintf(stderr, "load fail\n"); return 1; }
        if ((w & 3) || (h & 3)) {
            fprintf(stderr, "image dims must be /4: %dx%d\n", w, h);
            return 1;
        }
        int bw = w/4, bh = h/4;
        std::vector<uint8_t> blocks(bw * bh * 8);
        auto t0 = std::chrono::steady_clock::now();
        #pragma omp parallel for schedule(dynamic) collapse(2)
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                uint8_t blk[16 * 3];
                for (int y = 0; y < 4; ++y) {
                    memcpy(&blk[y*4*3], &img[((by*4+y)*w + bx*4)*3], 4*3);
                }
                encode_block(blk, &blocks[(by*bw + bx)*8]);
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1-t0).count();
        fprintf(stderr, "rg_etc1 encode: %dx%d in %.3f s\n", w, h, secs);
        FILE* f = fopen(argv[3], "wb");
        int32_t hdr[2] = { w, h };
        fwrite(hdr, sizeof(hdr), 1, f);
        fwrite(blocks.data(), 1, blocks.size(), f);
        fclose(f);
        stbi_image_free(img);
    } else if (mode == "decode") {
        FILE* f = fopen(argv[2], "rb");
        int32_t hdr[2]; fread(hdr, sizeof(hdr), 1, f);
        int w = hdr[0], h = hdr[1];
        int bw = w/4, bh = h/4;
        std::vector<uint8_t> blocks(bw * bh * 8);
        fread(blocks.data(), 1, blocks.size(), f);
        fclose(f);
        std::vector<uint8_t> img(w * h * 3);
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                uint8_t blk[16*3];
                decode_block(&blocks[(by*bw + bx)*8], blk);
                for (int y = 0; y < 4; ++y) {
                    memcpy(&img[((by*4+y)*w + bx*4)*3], &blk[y*4*3], 4*3);
                }
            }
        }
        stbi_write_png(argv[3], w, h, 3, img.data(), w*3);
    }
    return 0;
}
