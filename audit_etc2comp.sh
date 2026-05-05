#!/bin/bash
# Audit etc2comp for enhancement features (especially block archetypes).
SRC=/home/romain/etc1_bench/encoders/etc2comp/EtcLib
OUT=/home/romain/etc1_bench/audit_etc2comp.txt
{
echo "=========================================="
echo " etc2comp — enhancement audit"
echo "=========================================="
echo "Source root: $SRC"
ls $SRC/Etc/*.{h,cpp} 2>/dev/null | head
echo

echo "--- A. Block archetype classification ---"
echo "(this is etc2comp's headline feature)"
grep -nrE "archetype|block_type|class.*Block|EncodingType|classify" $SRC --include="*.h" --include="*.cpp" 2>/dev/null | head -20
echo
echo "  Per-block-type encoding paths:"
ls $SRC/EtcCodec*.h 2>/dev/null
echo
echo "  Effort parameter implementation:"
grep -nrE "effort|m_fEffort|Effort\b" $SRC --include="*.h" --include="*.cpp" 2>/dev/null | head -15

echo
echo "--- B. Error metrics ---"
grep -nrE "ErrorMetric|errormetric|REC709|RGBA|NUMERIC|NORMALXYZ" $SRC --include="*.h" --include="*.cpp" 2>/dev/null | head -20
echo
echo "  rec709 luma weights:"
grep -nrE "0\.299|0\.587|0\.114|0\.2126|0\.7152|0\.0722" $SRC --include="*.h" --include="*.cpp" 2>/dev/null | head -10

echo
echo "--- C. ETC1-specific path ---"
grep -nrE "ETC1\b|EncodeETC1|EncodingTypeETC1" $SRC --include="*.h" --include="*.cpp" 2>/dev/null | head -15

echo
echo "--- D. YCoCg ---"
grep -nrE "YCoCg|YCOCG|ycocg|yco_cg" $SRC --include="*.h" --include="*.cpp" 2>/dev/null | head -10

echo
echo "--- E. Joint sub-block / iterative refinement ---"
grep -nrE "iterat|refine|refinement|TryEncode" $SRC --include="*.h" --include="*.cpp" 2>/dev/null | head -20

echo
echo "--- F. SSIM ---"
grep -nrE "ssim|SSIM" $SRC --include="*.h" --include="*.cpp" 2>/dev/null | head -5

echo
echo "--- G. Branch-and-bound ---"
grep -nrE "branch.*bound|exhaust|brute" $SRC --include="*.h" --include="*.cpp" 2>/dev/null | head -10

echo
echo "--- H. Dithering ---"
grep -nrE "dither" $SRC --include="*.h" --include="*.cpp" 2>/dev/null | head -5

echo
echo "--- I. Saliency / attention ---"
grep -nrE "saliency|attention|ROI|weight.*map|importance" $SRC --include="*.h" --include="*.cpp" 2>/dev/null | head -5

echo
echo "Audit complete."
} > $OUT 2>&1
wc -l $OUT
