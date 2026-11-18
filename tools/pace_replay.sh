#!/usr/bin/env bash
# A LARECOMP_LOG_PACE=1 session log replayed through the frame pacer (tools/pace_replay.c says what it prints).
# tools/pace_replay.sh <log> [--as-logged] [--rates] [-q]
set -euo pipefail
cd "$(dirname "$0")/.."
exe=$(mktemp -t pace-replay.XXXXXX)
trap 'rm -f "$exe"' EXIT
${CC:-clang} -std=gnu11 -O1 -DPACE_TEST -Iruntime runtime/nvn/pace.c tools/pace_replay.c -lm -o "$exe"
env -u LARECOMP_PACE "$exe" "$@" 2>&1
