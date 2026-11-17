#!/bin/bash
# The Steam Frame's shader compiler on a desktop: tools/test_vk_pipelines.c (fuzzed Maxwell programs -> GLSL -> SPIR-V -> real
# pipelines) built for Arm64 and run by qemu-aarch64-static against the Turnip driver copied from the Frame, which runs on
# Mesa's no-op msm shim (no GPU work happens; pipelines are really compiled by ir3). No game files needed.
#   tools/ir3_check.sh [count] [seed]       (default 1000 programs, seed 1)
# Two runs, each with tools/frame_profile.env's shader switches (structured shaders, push constants, c1 inlining) and one of the
# Frame's two ways to reach the game's memory: the heap as driver memory in 128 MB chunks ("chunks": TT_GMEM_LAZY, whole), or
# copies ("copies": TT_GMEM_MIRROR). Failures are counted where they happen: "before the driver" (the translator's GLSL did not
# compile in shaderc, or its layout was refused) and "in the driver" (vkCreate*Pipelines failed or the layer said something).
# What it does not show: the programs are fuzzed, not the game's; the validation layer is not on under qemu; the real
# constants of c1 are not known (no inlining); each stage gets one push-constant range of 256 bytes, not exec.c's split.
# Exit 0: no failure of either kind. Environment: FRAME_SHIM (default $HOME/frame-build/turnip-shim), FRAME_SYSROOT (the Frame's
# root filesystem, $HOME/frame-build/sysroot), IR3_TIMEOUT (seconds a run may take, default 900). The shim can spin on anything
# that waits for the GPU: always under timeout.
set -uo pipefail
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
    -o build/ir3/test_vk_pipelines-aarch64 -lvulkan -lshaderc_shared || exit 1
COUNT=${1:-1000}; SEED=${2:-1}
bad=0
printf '%-8s %-10s %-9s %-8s %-18s %s\n' mode graphics compute skipped 'before the driver' 'in the driver'
for mode in chunks copies; do
    case $mode in
        chunks) M=(TT_GMEM_CHUNK_SHIFT=27 TT_GMEM_LAZY=1 TT_GMEM_WHOLE=1) ;;
        copies) M=(TT_GMEM_MIRROR=1) ;;
    esac
    L=build/ir3/run-$mode.log
    (
        set -a; . tools/frame_profile.env; set +a
        . "$SHIM/env.sh"
        export QEMU_LD_PREFIX=$SYSROOT
        exec env "${M[@]}" timeout "${IR3_TIMEOUT:-900}" nice -n 10 qemu-aarch64-static -L "$SYSROOT" \
            build/ir3/test_vk_pipelines-aarch64 runtime/nvn/maxwell/translate.c "$COUNT" "$SEED"
    ) > "$L" 2>&1
    rc=$?
    sum=$(grep -E '^[0-9]+ graphics pairs' "$L" || true)
    if [ -z "$sum" ]; then echo "$mode: no result (exit $rc, timed out: $([ $rc = 124 ] && echo yes || echo no)), see $L" >&2; bad=1; continue; fi
    pre=$(grep -cE '^FAIL (compute )?compile / layout' "$L" || true)
    drv=$(grep -cE '^FAIL (graphics|compute) pipeline' "$L" || true)
    val=$(sed -n 's/.* \([0-9]*\) validation messages$/\1/p' <<< "$sum")
    printf '%-8s %-10s %-9s %-8s %-18s %s\n' "$mode" "$(cut -d' ' -f1 <<< "$sum")" "$(awk '{print $4}' <<< "$sum")" \
        "$(awk '{print $7}' <<< "$sum")" "$pre" "$((drv + val))"
    [ "$pre" != 0 ] && grep -E "^(vs|fs|cs):[0-9]+: error" "$L" | sort | uniq -c | sed 's/^/    /' | head -n 4
    [ "$pre" != 0 ] || [ "$drv" != 0 ] || [ "$val" != 0 ] && bad=1
done
echo "(count $COUNT, seed $SEED; failures print up to 10 programs in build/ir3/run-<mode>.log; 'skipped' = the translator refused the fuzzed program)"
[ $bad = 0 ] && echo "ir3_check: ok" || echo "ir3_check: failures (above)" >&2
exit $bad
