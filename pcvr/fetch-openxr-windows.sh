#!/bin/bash
# pcvr/fetch-openxr-windows.sh (MSYS2 UCRT64 shell, in the checkout): OpenXR for the Windows PC VR zip (make dist
# PCVR=1, docs/af/PCVR.md). Khronos' release build of the loader and the headers, from the OpenXR.Loader NuGet package
# of the OpenXR SDK release (the same version as tools/fetch_openxr.sh), and the SDK's licence from its source; both
# pinned by SHA-256. Installs into build/deps-win:
#   include/openxr/*.h          (what the program is compiled against; the Makefile's XR switch looks for openxr.h)
#   bin/openxr_loader.dll       (x64; it imports only KERNEL32 and ADVAPI32, so it needs no MSVC runtime)
#   share/openxr/LICENSE, jsoncpp-LICENSE   (the loader's and its JSON reader's; shipped next to the dll)
# The downloads are kept in build/deps-win/src.
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=1.1.63
PKG_SHA256=4e5a50a8807ef66f25180ff224e7d8150b594aa8ee4b07590f9ade55a8e98703
PKG_URL=https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-$VERSION/OpenXR.Loader.$VERSION.nupkg
SRC_SHA256=5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f
SRC_URL=https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-$VERSION.tar.gz
D=build/deps-win
mkdir -p "$D/src" "$D/include/openxr" "$D/bin" "$D/share/openxr"

fetch() {  # url sha256 file
    [ -f "$D/src/$3" ] || { curl -fsSL --retry 3 -o "$D/src/$3.part" "$1" && mv "$D/src/$3.part" "$D/src/$3"; }
    echo "$2  $D/src/$3" | sha256sum -c --quiet || { echo "fetch-openxr-windows: checksum mismatch: $D/src/$3" >&2; rm -f "$D/src/$3"; exit 1; }
}
fetch "$PKG_URL" "$PKG_SHA256" "OpenXR.Loader.$VERSION.nupkg"
fetch "$SRC_URL" "$SRC_SHA256" "OpenXR-SDK-$VERSION.tar.gz"

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
if command -v unzip >/dev/null; then unzip -q "$D/src/OpenXR.Loader.$VERSION.nupkg" -d "$W"
else /c/Windows/System32/tar.exe -xf "$D/src/OpenXR.Loader.$VERSION.nupkg" -C "$W"; fi  # (a NuGet package is a zip)
cp "$W"/include/openxr/*.h "$D/include/openxr/"
cp "$W/native/x64/release/bin/openxr_loader.dll" "$D/bin/"
tar -xzf "$D/src/OpenXR-SDK-$VERSION.tar.gz" -O "OpenXR-SDK-release-$VERSION/LICENSE" > "$D/share/openxr/LICENSE"
tar -xzf "$D/src/OpenXR-SDK-$VERSION.tar.gz" -O "OpenXR-SDK-release-$VERSION/src/external/jsoncpp/LICENSE" \
    > "$D/share/openxr/jsoncpp-LICENSE"  # (the JSON reader the loader is built with)
grep -q "XR_CURRENT_API_VERSION XR_MAKE_VERSION(1, 1, ${VERSION##*.})" "$D/include/openxr/openxr.h"
echo "OpenXR $VERSION (headers, openxr_loader.dll) in $D"
