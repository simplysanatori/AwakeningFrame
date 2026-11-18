#!/bin/bash
# The whole table from above: tools/cull_check/overview.sh <out dir> <Field locator> [VAR=value ...]
# The head 2 m above the table's centre looking straight down (north up), so both eyes see all of the table and the
# picture (frame 1850) shows what the culling lets through around its centre.
set -uo pipefail
H=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
O=$1 LOC=$2; shift 2
mkdir -p "$O"
printf '0.0 0 1.6 0 0 0 0\n15.0 0 1.6 0 0 0 0\n15.5 0 3.393 -0.773 0 -90 0\n' > "$O/pose.txt"
TMO=120 "$H/run.sh" "$O" 1900 LARECOMP_WARP_AT="900:Field:$LOC" LARECOMP_VR_TILT=5 LARECOMP_VR_DISTANCE=0.8 \
    MOCK_XR_POSE="file:$O/pose.txt" LARECOMP_STEREO_DUMP_AT=1850 LARECOMP_LOG_CULL=10 "$@"
rm -f "$O"/stereo_*_hud.png "$O"/stereo_*_window.png
