#!/bin/bash
# Builds xrvkprobe: tools/xrvkprobe/build.sh [out dir] (default ~/.cache/af-xrcore). OpenXR headers: OPENXR_INCLUDE,
# else the deps folder tools/fetch_openxr.sh fills.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${1:-$HOME/.cache/af-xrcore}
INC=${OPENXR_INCLUDE:-$REPO/build/linux/aarch64/deps/include}
[ -f "$INC/openxr/openxr.h" ] || { echo "no OpenXR headers in $INC: tools/fetch_openxr.sh" >&2; exit 1; }
mkdir -p "$OUT"
clang -O1 -g -Wall -Wextra -o "$OUT/xrvkprobe" "$HERE/xrvkprobe.c" -I "$INC" -lvulkan -ldl
echo "$OUT/xrvkprobe"
