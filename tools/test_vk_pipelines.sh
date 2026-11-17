#!/bin/sh
# GPU test of the Vulkan shader path (tools/test_vk_pipelines.c): fuzzed Maxwell programs -> GLSL -> the
# backend's compile/reflection/layouts -> real pipelines on the device, under the validation layer if present.
# usage: tools/test_vk_pipelines.sh [count] [seed]      (Windows / MSYS2 ucrt64, Linux; tools/ir3_check.sh runs it on the
# Steam Frame's Turnip compiler under qemu)
set -e
cd "$(dirname "$0")/.."
OUT=build/test_vk_pipelines
mkdir -p "$OUT"
CC=${CC:-clang}
VK=-lvulkan-1; INC=
case "$(uname -s)" in Linux) VK=-lvulkan; [ -f /usr/include/vk_mem_alloc.h ] || INC="-I$(ls -d build/linux/*/deps/include/vma | head -n 1)" ;; esac
$CC -O2 -w -Iruntime -Iruntime/nvn $INC -D_FILE_OFFSET_BITS=64 tools/test_vk_pipelines.c runtime/nvn/maxwell/translate.c \
    runtime/nvn/vulkan/shaders.c -o "$OUT/test_vk_pipelines" $VK -lshaderc_shared
# the MSYS2 validation layer manifest lives next to its DLL
for d in /ucrt64/bin /c/msys64/ucrt64/bin; do
  [ -f "$d/VkLayer_khronos_validation.json" ] && export VK_ADD_LAYER_PATH="$(cygpath -w "$d")"
done
"$OUT/test_vk_pipelines" runtime/nvn/maxwell/translate.c "${1:-2000}" "${2:-1}"
