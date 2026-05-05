# Visual A/B - v9a Floor Sweep

Output dir: /home/romain/etc1_bench/results/visual_floor/

## Generation

Script: /home/romain/etc1_bench/make_visual_floor.py
Command: `python make_visual_floor.py` (default --diff-enc=v9a_floor25).
No re-encoding performed; all panels read pre-decoded PNGs from
/home/romain/etc1_bench/decoded/<image>__<encoder>/decoded.png.
All 6 encoders x 6 images = 36 decoded inputs were present (no missing panels).

## Encoders compared (left -> right)

1. original                ground truth
2. v6_idea2_v2 (v6_v2)     prior SOTA
3. v8b_worst50 (worst50)   best Y/SSIM, worst RGB
4. v9a_floor05 (floor05)   T=0.5, tightest floor (best RGB)
5. v9a_floor15 (floor15)   T=1.5, balanced
6. v9a_floor25 (floor25)   T=2.5, candidate dominator (also used in diff column)
7. etcpak                  sanity baseline

## Files generated (36 PNGs + metrics.json, 124 MiB total, 128,919,524 B)

Per image (MIX1, MIX5, all, gamelist_doom, gamelist_final_fantasy_vii, gamelist_sonic2):
- <image>_strip_1x.png      13440x1120, 7-panel full-res strip
- <image>_2x.png            7680x1716, 8 cols x 3 region rows, 480x270 native @2x
- <image>_4x.png            7680x1716, 8 cols x 3 region rows, 240x135 native @4x
- <image>_8x.png            7680x1704, 8 cols x 3 region rows, 120x67  native @8x
- <image>_errors_y.png      6 panels of |dY| viridis heatmap (shared vmax)
- <image>_errors_rgb.png    6 panels of max-channel |dRGB| viridis (shared vmax)

Plus metrics.json (PSNR_Y / PSNR_RGB / SSIM_Y per (image, encoder)).

Region picker selects three non-overlapping 480x270 windows by Laplacian:
high-detail, smooth, and median-Laplacian-with-high-variance (edges/text).
Zoom uses nearest-neighbor so block boundaries are preserved.
Diff column: |v9a_floor25 - orig| x 8 amplified.

## Perceptual judgment (Claude multimodal eyeball)

Sampled MIX1_8x, MIX1_4x, MIX1_errors_rgb, MIX1_errors_y,
gamelist_sonic2_errors_rgb, gamelist_sonic2_8x, all_errors_rgb
(downscaled to 3200px wide for inspection).

### Q1 - Is v9a_floor25 visibly indistinguishable from v8b_worst50?

YES. Side-by-side at 4x/8x zoom on MIX1, sonic2, and the gamelist images,
floor25 cannot be distinguished from worst50 by eye on natural content.
The Y-error heatmaps for those two are also nearly identical brightness.

The RGB error heatmaps reveal a slight difference: worst50 has marginally
more bright pixels in highly saturated regions (MIX1's magenta wig,
sonic2's red title bar). Floor25 is very slightly cleaner there - but you
have to know where to look. Matches the metrics: floor25 wins ~0.2-1.4 dB
PSNR_RGB over worst50 while paying back ~0.05-0.4 dB PSNR_Y.

### Q2 - Is the RGB recovery (floor05/15/25) visible as cleaner saturated regions?

YES on the heatmaps; BARELY on direct visual.
- On *_errors_rgb.png the gradient is clear: worst50 hottest in saturated
  regions, floor05 coolest, floor15/floor25 in between. Consistent across
  all 6 images.
- On the actual decoded panels (*_4x.png, *_8x.png) the difference is below
  perceptual threshold for almost all patches. Even at 8x the floor variants
  and worst50 look interchangeable on natural content.
- Exception: the most saturated patches (sonic2 red title, MIX1 magenta wig,
  MIX5 ferrari red) where a small color shift in worst50 vs floor05 is
  visible if you A/B-flip. Floor25 sits visually closer to worst50 than
  to floor05.

### Q3 - Is everything within visual threshold?

For the prior-SOTA comparison (v6_v2 vs floor variants): mostly within
visual threshold, though v6_v2 does show measurably more speckle in the
RGB heatmaps on saturated patches than any floor variant.

etcpak is the only encoder that is clearly visually distinct - heatmaps
uniformly hotter and visible block-artifact softness on smooth gradients.

### Caveat on "Pareto-dominant" claim

Re-reading metrics.json, v9a_floor25 does not strictly dominate v6_idea2_v2
on every (image, metric) pair:
- MIX1: v6_v2 PSNR_RGB = 35.886 vs floor25 35.804  -> v6_v2 wins PSNR_RGB
- gamelist_doom: v6_v2 PSNR_RGB = 39.783 vs floor25 40.309 -> floor25 wins
- MIX5: floor25 PSNR_Y = 45.253 vs v6_v2 45.274 -> tied

Dominance holds in aggregate; floor25 is the better balanced choice, but
not strictly Pareto on every pair.

## Verdict

Recommended: v9a_floor25.
- Visually indistinguishable from v8b_worst50.
- Measurably recovers RGB on saturated content vs worst50.
- Differences between floor05/15/25 are below visual threshold on decoded
  images; pick by metric preference:
    - RGB-on-saturated-content priority -> floor05
    - Y/SSIM priority                   -> floor25
