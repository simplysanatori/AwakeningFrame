#!/bin/bash
# Fetches the Khronos OpenXR SDK (Apache-2.0 / MIT, pinned and checksummed), and builds
# the loader for this machine (native Linux aarch64, no sudo).  Installs
#   $PREFIX/include/openxr/*.h     (the headers lasrecomp and the mock are compiled against)
#   $PREFIX/lib/libopenxr_loader.so
# PREFIX defaults to build/linux/aarch64/deps of the checkout (the game's DEPS dir); set
# OPENXR_PREFIX to put it elsewhere.  The source tarball is kept in ~/.cache/af-openxr.
# Builds with nice and -j2.  Refuses to build while ~/.cache/headset-in-use exists.
#   tools/fetch_openxr.sh
set -euo pipefail
VERSION=1.1.63
SHA256=5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f
URL=https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-$VERSION.tar.gz

REPO=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${OPENXR_PREFIX:-$REPO/build/linux/aarch64/deps}
CACHE=${AF_OPENXR_CACHE:-$HOME/.cache/af-openxr}

if [ -f "$PREFIX/include/openxr/openxr.h" ] && [ -f "$PREFIX/lib/libopenxr_loader.so" ] \
   && grep -q "XR_CURRENT_API_VERSION XR_MAKE_VERSION(1, 1, ${VERSION##*.})" "$PREFIX/include/openxr/openxr.h"; then
  echo "OpenXR $VERSION already in $PREFIX"; exit 0
fi
[ -e "$HOME/.cache/headset-in-use" ] && { echo "fetch_openxr: headset in use, not building" >&2; exit 75; }

mkdir -p "$CACHE" "$PREFIX"
TAR=$CACHE/OpenXR-SDK-$VERSION.tar.gz
if [ ! -f "$TAR" ]; then
  echo "fetching OpenXR SDK $VERSION"
  curl -fsSL --retry 3 -o "$TAR.part" "$URL"
  mv "$TAR.part" "$TAR"
fi
echo "$SHA256  $TAR" | sha256sum -c - || { echo "fetch_openxr: checksum mismatch, removing $TAR" >&2; rm -f "$TAR"; exit 1; }

W=$(mktemp -d "$CACHE/build.XXXXXX")
trap 'rm -rf "$W"' EXIT
tar -xzf "$TAR" -C "$W"
SRC=$(echo "$W"/OpenXR-SDK-*)
nice -n 10 cmake -G Ninja -S "$SRC" -B "$W/b" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
  -DDYNAMIC_LOADER=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > "$W/cmake.log" 2>&1 \
  || { tail -20 "$W/cmake.log" >&2; exit 1; }
nice -n 10 ninja -j2 -C "$W/b" install > "$W/ninja.log" 2>&1 || { tail -20 "$W/ninja.log" >&2; exit 1; }
test -f "$PREFIX/lib/libopenxr_loader.so" -a -f "$PREFIX/include/openxr/openxr.h"
echo "OpenXR $VERSION installed in $PREFIX"
