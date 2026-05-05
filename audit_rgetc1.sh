#!/bin/bash
# Audit rg_etc1 for enhancement features.
SRC=/home/romain/etc1_bench/encoders/rg-etc1
OUT=/home/romain/etc1_bench/audit_rgetc1.txt
{
echo "=========================================="
echo " rg_etc1 — enhancement audit"
echo "=========================================="
FILES="$SRC/rg_etc1.h $SRC/rg_etc1.cpp"
echo "Files: $FILES"
echo "LoC: $(wc -l $FILES | tail -1)"
echo

echo "--- A. Adaptive per-block effort ---"
grep -nE "block.*type|flat|solid|classify|archetype|fast.*path|early.*out" $FILES | head -20

echo
echo "--- B. Cluster-fit / total-ordering enumeration ---"
grep -nE "cluster.*fit|total.*ordering|selector.*distribution|perm" $FILES | head -10

echo
echo "--- C. Base-color search strategy ---"
echo "  3D neighborhood scan (lattice search):"
grep -nE "scan_delta|neighborhood|lattice|3.?d.*scan" $FILES | head -15
echo "  Refinement loop:"
grep -nE "refinement|refine|iterat" $FILES | head -10

echo
echo "--- D. YCoCg or other non-RGB metric ---"
grep -nE "YCoCg|YCOCG|ycocg|YCbCr|perceptual|luma|luminance" $FILES | head -10
echo "  (header explicitly states 'does not currently support perceptual':)"
grep -n "perceptual" $SRC/rg_etc1.h

echo
echo "--- E. Joint sub-block optimization ---"
grep -nE "constrain|joint|both.*sub|two.*sub" $FILES | head -10

echo
echo "--- F. SSIM / MS-SSIM ---"
grep -nE "ssim|SSIM|structural" $FILES | head -5

echo
echo "--- G. Branch-and-bound / exhaustive ---"
grep -nE "branch.*bound|exhaust|brute" $FILES | head -5
echo "  full base-color enumeration (cTotalEtc1Limit / 32 / 16 loops)?"
grep -nE "cTotalEtc1Limit|for.*<.*32|for.*<.*16|cluster_fit_quality" $FILES | head -10

echo
echo "--- H. Quality enum and trials counts ---"
grep -nE "etc1_quality|cLowQuality|cMediumQuality|cHighQuality|trials|num_passes" $FILES | head -20

echo
echo "--- I. Dithering ---"
grep -nE "dither|m_dithering" $FILES | head -10

echo
echo "--- J. Special cases / edge handling ---"
grep -nE "special.*case|edge.*case|all_pixels_same|degenerat" $FILES | head -10

echo
echo "Audit complete."
} > $OUT 2>&1
wc -l $OUT
