#!/bin/bash
# The Steam Frame's shader compiler on a desktop: tools/test_vk_pipelines.c (fuzzed Maxwell programs -> GLSL -> SPIR-V -> real
# pipelines) built for Arm64 and run by qemu-aarch64-static against the Turnip driver copied from the Frame, which runs on
# Mesa's no-op msm shim (no GPU work happens; pipelines are really compiled by ir3). No game files needed.
#   tools/ir3_check.sh [count] [seed]       (default 400 programs, seed 1)
# Runs with tools/frame_profile.env's shader switches (structured shaders, push constants), as the Frame translates. Exit 0:
# no failed compile and no validation message. Environment: FRAME_SHIM (default $HOME/frame-build/turnip-shim),
# FRAME_SYSROOT (the Frame's root filesystem, $HOME/frame-build/sysroot), IR3_TIMEOUT (seconds, default 900).
# The shim can spin on anything that waits for the GPU: always under timeout.
set -euo pipefail
cd "$(dirname "$0")/.."
SHIM=${FRAME_SHIM:-$HOME/frame-build/turnip-shim}
SYSROOT=${FRAME_SYSROOT:-$HOME/frame-build/sysroot}
[ -f "$SHIM/env.sh" ] && [ -d "$SYSROOT" ] || { echo "ir3_check: no Turnip shim in $SHIM or sysroot in $SYSROOT (FRAME_SHIM, FRAME_SYSROOT)" >&2; exit 1; }
command -v qemu-aarch64-static > /dev/null || { echo "ir3_check: no qemu-aarch64-static" >&2; exit 1; }
VMA=$(ls -d build/linux/*/deps/include/vma 2> /dev/null | head -n 1 || true)
[ -n "$VMA" ] || [ -f /usr/include/vk_mem_alloc.h ] || { echo "ir3_check: no vk_mem_alloc.h (steam-frame/fetch-vma.sh)" >&2; exit 1; }
mkdir -p build/ir3
nice -n 10 clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld -O2 -w -Iruntime -Iruntime/nvn ${VMA:+-I$VMA} \
    -D_FILE_OFFSET_BITS=64 tools/test_vk_pipelines.c runtime/nvn/maxwell/translate.c runtime/nvn/vulkan/shaders.c \
    -o build/ir3/test_vk_pipelines-aarch64 -lvulkan -lshaderc_shared
set -a; . tools/frame_profile.env; set +a
. "$SHIM/env.sh"
export QEMU_LD_PREFIX=$SYSROOT
rc=0
timeout "${IR3_TIMEOUT:-900}" nice -n 10 qemu-aarch64-static -L "$SYSROOT" build/ir3/test_vk_pipelines-aarch64 \
    runtime/nvn/maxwell/translate.c "${1:-400}" "${2:-1}" > build/ir3/run.log 2>&1 || rc=$?
grep -E '^device:|failures' build/ir3/run.log
[ $rc = 124 ] && echo "ir3_check: timed out" >&2
[ $rc = 0 ] && echo "ir3_check: ok" || echo "ir3_check: failed (exit $rc), see build/ir3/run.log" >&2
exit $rc
