#!/bin/bash
# Audit basis_universal's ETC1 path for enhancement features.
SRC=/home/romain/etc1_bench/encoders/basis_universal/encoder
OUT=/home/romain/etc1_bench/audit_basisu.txt
{
echo "=========================================="
echo " basis_universal — enhancement audit"
echo "=========================================="
echo "Source root: $SRC"
echo "Files scanned: basisu_etc.{h,cpp}, basisu_enc.{h,cpp}, basisu_frontend.cpp"
FILES="$SRC/basisu_etc.h $SRC/basisu_etc.cpp $SRC/basisu_enc.h $SRC/basisu_enc.cpp $SRC/basisu_frontend.cpp"
echo

echo "--- A. Adaptive per-block effort ---"
echo "Look for: classifying blocks before encode, branching on flat vs edge, etc."
grep -nE "block.*archetype|classify_block|block_type|flat.*block|solid.*color|pack_etc1_block_solid" $FILES 2>/dev/null | head -20
echo
echo "  pack_etc1_block_solid_color: defined?"
grep -nE "pack_etc1_block_solid_color" $FILES | head -5
echo "  -> Used as a fast path? Check call sites:"
grep -rnE "pack_etc1_block_solid_color\(" $SRC/.. 2>/dev/null | grep -v "^Binary" | head -10

echo
echo "--- B. Cluster-fit distribution count (165) ---"
grep -nE "BASISU_ETC1_CLUSTER_FIT_ORDER_TABLE_SIZE|g_cluster_fit_order_tab|total_perms_to_try|compute_internal_cluster_fit\(" $FILES | head -15
echo "  Per quality level, count passed in:"
grep -B1 -A1 "compute_internal_cluster_fit" $SRC/basisu_etc.cpp | head -25

echo
echo "--- C. Try-all 8 quantization corners ---"
echo "Looking for explicit corner search around LS solution:"
grep -nE "for.*ddr|for.*ddg|for.*ddb|corner|round.*5|round.*4|\+0\.5" $SRC/basisu_etc.cpp | head -15
echo "(Our patch already lands in compute_internal_cluster_fit; verify it's there:)"
grep -n "try_all_corners\|ddr\|ddg\|ddb" $SRC/basisu_etc.cpp | head -10

echo
echo "--- D. YCoCg color-space metric ---"
grep -nE "YCoCg|YCOCG|ycocg|yco_cg|Co_Cg" $FILES | head -10
echo "  current perceptual metric weights (BT.601-ish):"
grep -nE "delta_l|delta_cr|delta_cb|14 \* dr|45 \* dg" $SRC/basisu_enc.h | head -10

echo
echo "--- E. Joint sub-block optimization in diff mode ---"
grep -nE "constrain_against_base_color5|joint.*sub.*block|sub_block.*joint|iterate.*sub" $FILES | head -10
echo "  is the diff constraint applied to BOTH sub-blocks (joint) or only sub1?"
grep -nA3 "m_constrain_against_base_color5" $SRC/basisu_etc.cpp | head -20

echo
echo "--- F. MS-SSIM / SSIM as encode cost or final selection ---"
grep -nE "ms_ssim|MS_SSIM|MS-SSIM|ssim|SSIM|structural_sim" $FILES | head -10

echo
echo "--- G. Branch-and-bound exhaustive base-color search ---"
grep -nE "branch.*bound|exhaustive|brute.*force|enumerate.*all" $FILES | head -10
echo "  any 'for r=0..15' / 'for g=0..15' style enumeration?"
grep -nE "for.*r.*<.*16|for.*g.*<.*16|for.*r.*<.*32" $SRC/basisu_etc.cpp | head -10

echo
echo "--- H. Refinement / iterative alternation ---"
grep -nE "refine_solution|iterative.*refinement|alternat|max_refinement_trials" $FILES | head -15

echo
echo "--- I. Perceptual saliency / attention maps ---"
grep -nE "saliency|attention|ROI|importance.*map" $FILES | head -5

echo
echo "--- J. Dithering / pre-processing ---"
grep -nE "dither|floyd.steinberg|error_diffus" $FILES | head -10
echo
echo "Audit complete."
} > $OUT 2>&1
wc -l $OUT
