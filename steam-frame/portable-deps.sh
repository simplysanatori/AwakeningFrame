#!/bin/bash
# steam-frame/portable-deps.sh: the libraries a portable release carries in lib/, built from pinned sources into
# build/linux/aarch64/deps (run by .github/workflows/portable.yml; works on any aarch64 Linux): SDL3 and an LGPL-only
# FFmpeg with just the demuxers and decoders the game's movies need (the configuration of tools/linux/build_deps.sh),
# and the OpenXR / Vulkan Memory Allocator headers. The libraries need nothing but glibc.
set -euo pipefail
cd "$(dirname "$0")/.."
P=$PWD/build/linux/aarch64/deps
S=$PWD/build/linux/src
W=$PWD/build/linux/aarch64/deps-build
mkdir -p "$S" "$P/include" "$W"
JOBS=$(nproc)
fetch() {  # url sha256 file
    [ -f "$S/$3" ] || { curl -fsSL -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | sha256sum -c --quiet
}
unpack() { rm -rf "$W/$2" && mkdir -p "$W/$2" && tar -xf "$S/$1" -C "$W/$2" --strip-components 1; }
fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz \
      7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68 SDL3-3.4.16.tar.gz
fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz
if [ ! -f "$P/lib/libSDL3.so" ]; then
    unpack SDL3-3.4.16.tar.gz sdl
    cmake -S "$W/sdl" -B "$W/sdl/build" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF -DSDL_RPATH=OFF
    cmake --build "$W/sdl/build" -j"$JOBS" && cmake --install "$W/sdl/build"
fi
if [ ! -f "$P/lib/libavcodec.so" ]; then
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    (cd "$W/ffmpeg" && ./configure --prefix="$P" --cc=clang --cxx=clang++ \
        --enable-shared --disable-static --enable-pic \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        && make -j"$JOBS" && make install)
fi
grep -qa 'GPL version 3\|--enable-gpl' "$P/lib/libavcodec.so" && { echo "FFmpeg is not LGPL"; exit 1; }
[ -f "$P/include/vma/vk_mem_alloc.h" ] || steam-frame/fetch-vma.sh
[ -f "$P/include/openxr/openxr.h" ] || tools/fetch_openxr.sh
echo "deps: $(cd "$P/lib" && ls *.so | tr '\n' ' ')"
