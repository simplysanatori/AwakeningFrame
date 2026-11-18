#!/bin/bash
# The view cases at one spot: tools/cull_check/sweep.sh <out dir> <Field locator> [VAR=value ...]
# One session: the 32 cases of cases.py (table yaw 0 / 90 / 180 / 270, tilt 0 / 15 / 30 / 50, the head seated and leaning low),
# 4.5 s each, both eye images every 100 game frames, the cull log every 10. Then report.py <test> <reference> <base>:
#   the test as is; the reference with LARECOMP_VR_CULL_TABLE_UNITS=52,42 LARECOMP_VR_CULL_EYES=0 (every resident room);
#   the base with LARECOMP_VR_CULL=camera (the game camera's fit).
# CASE_START=<s>: when the cases begin (24; later when a scripted walk comes first), CASE_REPS=<n>: the list n times.
set -uo pipefail
H=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
O=$1 LOC=$2; shift 2
mkdir -p "$O"
python3 "$H/cases.py" "${CASE_REPS:-1}" > "$O/pose.txt"
LAST=${LAST:-7000}
TMO=${TMO:-230} "$H/run.sh" "$O" "$LAST" LARECOMP_WARP_AT="900:Field:$LOC" LARECOMP_VR_TILT=30 LARECOMP_VR_DISTANCE=0.8 \
    MOCK_XR_POSE="file:$O/pose.txt" LARECOMP_STEREO_DUMP_AT="$(seq -s, 1250 100 "$LAST")" LARECOMP_LOG_CULL=10 "$@"
rm -f "$O"/stereo_*_hud.png "$O"/stereo_*_window.png
