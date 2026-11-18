#!/bin/bash
# Cost at the stock view: tools/cull_check/cost.sh <out dir> [VAR=value ...]
# One session, no dumps: the default table (tilt 30, yaw 0, 0.8 m) and a still head looking at its centre, at Mabe's
# centre, the woods' edge and the forest, 1500 game frames each. cost.py <dir> ... reads the draws per game frame and
# the game and render threads' times; name the directories <group>_<n> for the spread over repeats.
set -uo pipefail
H=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
O=$1; shift
TMO=200 "$H/run.sh" "$O" 5400 LARECOMP_WARP_AT=900:Field:Field_11C,2400:Field:Field_09C_a,3900:Field:Field_06C \
    MOCK_XR_POSE=static:0,1.6,0,0,-15 LARECOMP_LOG_CULL=60 "$@"
