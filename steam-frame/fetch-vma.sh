#!/bin/bash
# steam-frame/fetch-vma.sh: Vulkan Memory Allocator 3.3.0 (the version tools/linux/build_deps.sh pins) as a header-only
# DEPS prefix for distributions that don't package it (SteamOS). Build with: make DEPS=$PWD/build/linux/<arch>/deps
set -euo pipefail
cd "$(dirname "$0")/.."
P=build/linux/$(uname -m | sed s/arm64/aarch64/)/deps
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
[ -f "$P/include/vma/vk_mem_alloc.h" ] && { echo "$P/include/vma/vk_mem_alloc.h exists"; exit 0; }
curl -fsSL -o "$T/vma.tar.gz" https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
echo "c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e  $T/vma.tar.gz" | sha256sum -c --quiet
mkdir -p "$P/include/vma"
tar -xzf "$T/vma.tar.gz" -C "$T" --strip-components 1 VulkanMemoryAllocator-3.3.0/include/vk_mem_alloc.h
cp "$T/include/vk_mem_alloc.h" "$P/include/vma/"
echo "$P/include/vma/vk_mem_alloc.h"
