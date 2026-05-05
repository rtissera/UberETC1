# UberETC1

High-quality ETC1 encoder research with GPU-decode validation.

This repo grew out of a deep-research investigation into the best-possible
ETC1 (Ericsson Texture Compression 1) encode quality, ignoring encode time.
It contains:

1. **A literature/encoder survey** — `RESEARCH_REPORT.md`. Catalogs every open
   ETC1 encoder, summarizes their algorithms, tabulates published benchmarks,
   distinguishes ETC1 vs ETC1S, and proposes a concrete enhancement pipeline.

2. **A reproducible 1920×1080 benchmark** — `bench.py` + harnesses. Encodes
   real Batocera/Recalbox/RetroPie 1080p PNG theme backgrounds with five
   open-source encoders, decodes back to PNG, and measures PSNR/SSIM with
   neutral skimage code.

3. **C++ harnesses** for direct ETC1 encoding using:
   - `rg_etc1_tool.cpp` — rg-etc1 cHighQuality (full ETC1)
   - `basisu_etc1_tool.cpp` — basis_universal `etc1_optimizer` driving full
     ETC1 (both flip orientations × diff/individual modes), Uber quality,
     RGB-domain MSE
   - `basisu_etc1_tool_v2.cpp` — same but with weighted YCbCr perceptual metric

4. **Patched basis_universal** (`patches/0001-try-all-corners.patch`)
   — adds an 8-corner quantization search around the cluster-fit
   least-squares solution (§5.2 step 3 of the research report).

5. **GPU-decode validator** — `gl_decode.c`, a headless EGL+GLES2 program
   that uploads our raw ETC1 stream as a `GL_OES_compressed_ETC1_RGB8_texture`,
   renders it, and reads back. Used to prove our bitstreams are hardware-valid
   *and* that AMD's hardware decoder agrees bit-exactly with basis_universal's
   software decoder (`mean_abs_diff = 0.000000` on every test).

## Results (1920×1080, 6 themed backgrounds, RGB PSNR / Y-PSNR / Y-SSIM)

| Encoder | PSNR_RGB (dB) | PSNR_Y (dB) | SSIM_Y | Encode time (s) | Notes |
|---|---:|---:|---:|---:|---|
| etcpak | 35.879 | 39.718 | 0.9670 | 0.04 | heuristic, fastest |
| basisu_full (RGB metric, Uber + cluster fit) | 39.016 | 42.047 | 0.9879 | 3.44 | our driver |
| etc2comp -effort 100 | 39.109 | 42.080 | 0.9894 | 39.49 | Google, archived |
| **rg_etc1 cHighQuality** | **39.229** | **42.197** | **0.9895** | 19.20 | best RGB |
| basisu_v3_corners (try-all-corners) | 39.015 | 42.045 | 0.9879 | 5.04 | Uber already exhaustive — no gain |
| basisu_full_perc (perceptual YCbCr) | 37.983 | 43.378 | 0.9919 | 1.15 | best Y-PSNR (≤v3) |
| **basisu_v3_corners_perc** | 37.688 | **43.509** | **0.9920** | 6.31 | **best perceptual quality overall** |

### Key findings

- For **pure RGB MSE**, rg_etc1 cHighQuality wins at 39.23 dB (~+0.12 dB over
  etc2comp e100, ~3.3 dB over etcpak), at 19 s/image with 32 OpenMP threads.
- The `try-all-corners` patch on top of Uber-quality cluster fit gives **no
  measurable RGB gain** because Uber already runs all 165 selector
  distributions plus iterative refinement — the 8 cube corners around the LS
  solution are already explored by the existing inner-loop.
- Switching cluster fit to **weighted YCbCr (basisu's `m_perceptual=true`)**
  gives **+1.2 to +1.3 dB Y-PSNR** at 30× lower wall time than rg_etc1
  (with multithreading). This is the biggest single quality lever in the
  pipeline.
- Combining perceptual mode + try-all-corners adds another **+0.13 dB Y-PSNR**
  and +0.0001 SSIM_Y on top — small but consistent.
- **GPU-decode of every produced bitstream matches the software decoder
  bit-exactly** (mean abs diff 0.000000 across all encoders/images),
  confirming the bitstreams are hardware-conforming.

## Test images

Six fully-opaque 1920×1080 PNGs sampled from public Batocera/Recalbox/
RetroPie theme repositories. They are not redistributed in this repo;
the `bench.py` script expects them under `test_images/`. Sources:

- pulse/MIX1.png, MIX5.png — `complicatiion/batocera_pulse_theme`
- gamelist_doom.png, gamelist_sonic2.png, gamelist_final_fantasy_vii.png — `SamYStudiO/es-theme-next-slide`
- all.png — `SamYStudiO/es-theme-next-pixel`

## Reproducing

Prereqs: cmake, g++ with OpenMP, libegl-dev, libgles-dev, ImageMagick,
Python 3 with skimage/PIL/numpy.

```bash
# Clone upstream encoders into encoders/
# (ETCPACK / Ericsson reference dropped — too slow, non-OSI license, no
# quality advantage over rg_etc1 / basisu cluster fit on real content.)
git clone https://github.com/BinomialLLC/basis_universal.git encoders/basis_universal
git clone https://github.com/wolfpld/etcpak.git encoders/etcpak
git clone https://github.com/google/etc2comp.git encoders/etc2comp
git clone https://github.com/richgel999/rg-etc1.git encoders/rg-etc1
curl -sL https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -o encoders/stb_image.h
curl -sL https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h -o encoders/stb_image_write.h

# Apply our patches
patch -p0 -d encoders/basis_universal < patches/0001-try-all-corners.patch

# Build encoders (etcpak needs CMake 3.20+; lower minimum if needed)
mkdir -p build/{etcpak,etc2comp,basisu}
(cd build/etc2comp && cmake ../../encoders/etc2comp -DCMAKE_BUILD_TYPE=Release && make -j)
(cd build/etcpak && cmake ../../encoders/etcpak -DCMAKE_BUILD_TYPE=Release && make -j)
(cd build/basisu && cmake ../../encoders/basis_universal -DCMAKE_BUILD_TYPE=Release && make -j)

# Build our drivers
g++ -O3 -fopenmp -Iencoders -Iencoders encoders/rg_etc1_tool.cpp encoders/rg-etc1/rg_etc1.cpp -o build/rg_etc1_tool
g++ -O3 -fopenmp -std=c++17 -Iencoders encoders/basisu_etc1_tool.cpp build/basisu/libbasisu_encoder.a -lpthread -o build/basisu_etc1_tool
g++ -O3 -fopenmp -std=c++17 -Iencoders encoders/basisu_etc1_tool_v2.cpp build/basisu/libbasisu_encoder.a -lpthread -o build/basisu_etc1_tool_v2

# GPU validator
gcc -O2 -Iencoders encoders/gl_decode.c -lEGL -lGLESv2 -o build/gl_decode

# Place 6 fully-opaque 1920x1080 PNGs in test_images/, then:
python3 -m venv venv && venv/bin/pip install scikit-image numpy pillow
venv/bin/python3 bench.py
```

## License

The harness code (`*.cpp`, `*.c`, `*.py` we wrote) is released under MIT.
All upstream encoders retain their own licenses (see each repo).

## See also

- `RESEARCH_REPORT.md` — the full encoder survey + algorithmic catalog
  + recommended enhancement pipeline (§5.2 1–8).
- `patches/0001-try-all-corners.patch` — the basis_universal modification.
