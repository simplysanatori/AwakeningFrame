#!/bin/bash
# Prints what the Vulkan driver offers, as the renderer asks for it (tools/vkprobe/vkprobe.c).
#   tools/vkprobe/run.sh            this machine's GPU
#   tools/vkprobe/run.sh --frame    the Steam Frame's Turnip: an Arm64 build run by qemu-aarch64-static against the driver
#                                   copied from the Frame and Mesa's no-op msm shim (a desktop with that setup: FRAME_SHIM,
#                                   FRAME_SYSROOT). The shim draws nothing; this only reads what the driver reports.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${VKPROBE_OUT:-$HERE/../../build/vkprobe}
mkdir -p "$OUT"
if [ "${1:-}" != --frame ]; then
    nice -n 10 clang -O1 -o "$OUT/vkprobe" "$HERE/vkprobe.c" -lvulkan
    exec "$OUT/vkprobe"
fi
SHIM=${FRAME_SHIM:-$HOME/frame-build/turnip-shim}
SYSROOT=${FRAME_SYSROOT:-$HOME/frame-build/sysroot}
[ -f "$SHIM/env.sh" ] && [ -d "$SYSROOT" ] || { echo "vkprobe: no Turnip shim in $SHIM or sysroot in $SYSROOT (FRAME_SHIM, FRAME_SYSROOT)" >&2; exit 1; }
command -v qemu-aarch64-static >/dev/null || { echo "vkprobe: no qemu-aarch64-static" >&2; exit 1; }
nice -n 10 clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld -O1 -o "$OUT/vkprobe-aarch64" "$HERE/vkprobe.c" -lvulkan
. "$SHIM/env.sh"
export QEMU_LD_PREFIX=$SYSROOT
timeout 120 qemu-aarch64-static -L "$SYSROOT" "$OUT/vkprobe-aarch64" 2>&1 | grep -v '^MESA: warning'
