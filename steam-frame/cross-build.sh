#!/bin/bash
# steam-frame/cross-build.sh: builds the Steam Frame program (build/aarch64/lasrecomp, VR mode in, no game code) and the
# libraries it carries on an x86-64 desktop, with clang's --target against an aarch64 glibc 2.39 root filesystem. The same
# result as .github/workflows/portable.yml + steam-frame/portable-deps.sh, which build on an arm64 runner; what
# steam-frame/build.sh packs (AF_PORTABLE_DIR=build/aarch64/portable).
#   steam-frame/cross-build.sh [--deps-only] [--check]
#   --deps-only   stop after SDL3, FFmpeg and the headers
#   --check       only the checks on an existing build/aarch64/portable
# Environment:
#   SYSROOT        the target's root filesystem, glibc 2.39 (default $HOME/frame-build/primedframe/sysroot-portable)
#   RUN_SYSROOT    a root filesystem with the system libraries (Vulkan, shaderc) to start the program in under qemu
#                  (default $HOME/frame-build/sysroot, the Frame's own)
#   JOBS           build jobs (default 8, at most 8)
#   AF_CROSS_LOCK  lock file (default $HOME/frame-build/build-af-desk.lock); one cross build at a time per lock
# Output: build/aarch64/lasrecomp, build/aarch64/portable/{lasrecomp,lib/} (the layout of the portable build's artifact),
# the libraries in build/linux/aarch64/deps. Never builds the game code: that needs the player's game files (make gen).
# nice 19. Refuses to run while ~/.cache/headset-in-use exists (exit 75).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
DEPS_ONLY=0; CHECK_ONLY=0
for a in "$@"; do
    case $a in
        --deps-only) DEPS_ONLY=1 ;;
        --check) CHECK_ONLY=1 ;;
        -h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "cross-build: unknown argument $a (--help)" >&2; exit 2 ;;
    esac
done
[ "$(uname -m)" = aarch64 ] && { echo "cross-build: this machine is aarch64: use steam-frame/portable-deps.sh and make" >&2; exit 2; }
[ -e "${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}" ] && { echo "the headset is in use: not building" >&2; exit 75; }
SYSROOT=${SYSROOT:-$HOME/frame-build/primedframe/sysroot-portable}
JOBS=${JOBS:-8}; [ "$JOBS" -gt 8 ] && JOBS=8
LOCK=${AF_CROSS_LOCK:-$HOME/frame-build/build-af-desk.lock}
TRIPLE=aarch64-linux-gnu
P=$ROOT/build/linux/aarch64/deps          # SDL3, FFmpeg, OpenXR and VMA headers for the target
S=$ROOT/build/linux/src
W=$ROOT/build/linux/aarch64/deps-build
OUT=$ROOT/build/aarch64/portable
HOST_DEPS=$ROOT/build/linux/$(uname -m)/deps
need() { command -v "$1" > /dev/null || { echo "cross-build: no $1" >&2; exit 1; }; }
for t in clang clang++ ld.lld cmake ninja make objdump readelf nice flock curl sha256sum; do need $t; done
[ -f "$SYSROOT/usr/include/stdio.h" ] && [ -e "$SYSROOT/usr/lib/libc.so.6" ] || { echo "cross-build: no aarch64 root filesystem in $SYSROOT (SYSROOT=)" >&2; exit 1; }
LIBC=$(strings -a "$SYSROOT/usr/lib/libc.so.6" | sed -n 's/.*GNU C Library.*release version \([0-9.]*\)\..*/\1/p' | head -n 1)
[ "$LIBC" = 2.39 ] || { echo "cross-build: $SYSROOT has glibc $LIBC, the release needs 2.39 (check-bundle allows no newer symbols)" >&2; exit 1; }

check() {  # the checks on build/aarch64/portable
    local bin=$OUT/lasrecomp max
    [ -x "$bin" ] || { echo "cross-build: no $bin" >&2; return 1; }
    max=$(objdump -p "$bin" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1)
    echo "glibc symbols up to $max"
    file -b "$bin" | grep -q 'ARM aarch64' || { echo "not an aarch64 binary" >&2; return 1; }
    grep -aq LARECOMP_XR_HZ "$bin" || { echo "no VR mode in $bin" >&2; return 1; }
    grep -aqF -- --build-game-code "$bin" || { echo "no --build-game-code in $bin" >&2; return 1; }
    [ "$(nm "$bin" | grep -c ' f_70' || true)" = 0 ] || { echo "game code linked into $bin" >&2; return 1; }
    ALLOW_MISSING_HOST=1 steam-frame/tools/check-bundle.sh "$OUT" | tail -n 8
    # it starts (under qemu, on the Frame's own root filesystem: the system libraries it opens are there) and knows its flags
    local rt=${RUN_SYSROOT:-$HOME/frame-build/sysroot} e rc=0
    if command -v qemu-aarch64-static > /dev/null && [ -d "$rt" ]; then
        e=$(mktemp -d)
        (cd "$e" && LARECOMP_DATA_ROOT=$e QEMU_LD_PREFIX=$rt timeout 60 qemu-aarch64-static -L "$rt" "$bin" --game-code-status > /dev/null 2>&1) || rc=$?
        rm -rf "$e"
        echo "qemu start (--game-code-status, no game files): exit $rc (0 or 1 is a start)"
        [ "$rc" -le 1 ] || return 1
    else
        echo "no qemu-aarch64-static or RUN_SYSROOT: the start was not tried"
    fi
}
if [ $CHECK_ONLY = 1 ]; then check; exit $?; fi

mkdir -p "$(dirname "$LOCK")"
exec 9> "$LOCK"
flock -n 9 || { echo "cross-build: $LOCK is held by another build" >&2; exit 75; }
mkdir -p "$S" "$P/include" "$W"

# ---- headers (the same for every arch): the OpenXR SDK's and the Vulkan Memory Allocator's, from the host's deps
[ -f "$HOST_DEPS/include/openxr/openxr.h" ] || tools/fetch_openxr.sh
[ -f "$HOST_DEPS/include/vma/vk_mem_alloc.h" ] || steam-frame/fetch-vma.sh
[ -f "$P/include/openxr/openxr.h" ] || cp -a "$HOST_DEPS/include/openxr" "$P/include/"
[ -f "$P/include/vma/vk_mem_alloc.h" ] || cp -a "$HOST_DEPS/include/vma" "$P/include/"

# ---- the toolchain: clang --target, the root filesystem, lld, the host's cmake / ninja
TC=$W/toolchain.cmake
cat > "$TC" << EOT
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT "$SYSROOT")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET $TRIPLE)
set(CMAKE_CXX_COMPILER_TARGET $TRIPLE)
set(CMAKE_LINKER_TYPE LLD)
set(CMAKE_FIND_ROOT_PATH "$SYSROOT")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOT
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig PKG_CONFIG_PATH=
fetch() {  # url sha256 file
    [ -f "$S/$3" ] || { curl -fsSL -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | sha256sum -c --quiet
}
unpack() { rm -rf "$W/$2" && mkdir -p "$W/$2" && tar -xf "$S/$1" -C "$W/$2" --strip-components 1; }
# (the pins of steam-frame/portable-deps.sh)
fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz \
      7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68 SDL3-3.4.16.tar.gz
fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz

# (the root filesystem has no libXss: SDL3's X11 screensaver inhibit is left out, nothing else differs from the arm64 runner's build)
SDL_EXTRA=
[ -e "$SYSROOT/usr/lib/libXss.so" ] || SDL_EXTRA=-DSDL_X11_XSCRNSAVER=OFF
if [ ! -f "$P/lib/libSDL3.so" ]; then
    echo "== SDL3"
    unpack SDL3-3.4.16.tar.gz sdl
    nice -n 19 cmake -S "$W/sdl" -B "$W/sdl/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF -DSDL_RPATH=OFF $SDL_EXTRA > "$W/sdl.log" 2>&1 || { tail -n 30 "$W/sdl.log" >&2; exit 1; }
    nice -n 19 cmake --build "$W/sdl/build" -j"$JOBS" >> "$W/sdl.log" 2>&1 && cmake --install "$W/sdl/build" >> "$W/sdl.log" 2>&1 \
        || { tail -n 30 "$W/sdl.log" >&2; exit 1; }
fi
if [ ! -f "$P/lib/libavcodec.so" ]; then
    echo "== FFmpeg"
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    # (FFmpeg's libraries carry their configure line: the compilers are wrappers found by name and the prefix is a made-up one, installed
    # through DESTDIR, so no path of this machine is in them)
    mkdir -p "$W/bin"
    for t in cc:clang cxx:clang++; do
        printf '#!/bin/sh\nexec %s --target=%s --sysroot=%s "$@"\n' "${t#*:}" "$TRIPLE" "$SYSROOT" > "$W/bin/af-${t%%:*}"
        chmod +x "$W/bin/af-${t%%:*}"
    done
    printf '#!/bin/sh\nexec clang --target=%s --sysroot=%s -fuse-ld=lld "$@"\n' "$TRIPLE" "$SYSROOT" > "$W/bin/af-ld"
    chmod +x "$W/bin/af-ld"
    rm -rf "$W/ffmpeg-dest"
    (cd "$W/ffmpeg" && PATH=$W/bin:$PATH ./configure --prefix=/deps --enable-cross-compile --arch=aarch64 --target-os=linux \
        --cc=af-cc --cxx=af-cxx --ld=af-ld --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm --strip=llvm-strip --pkg-config=false \
        --enable-shared --disable-static --enable-pic \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        && PATH=$W/bin:$PATH nice -n 19 make -j"$JOBS" && make install DESTDIR="$W/ffmpeg-dest") > "$W/ffmpeg.log" 2>&1 \
        || { tail -n 30 "$W/ffmpeg.log" >&2; exit 1; }
    cp -a "$W/ffmpeg-dest/deps/." "$P/"
fi
grep -qa 'GPL version 3\|--enable-gpl' "$P/lib/libavcodec.so" && { echo "FFmpeg is not LGPL" >&2; exit 1; }
echo "deps: $(cd "$P/lib" && ls ./*.so | tr '\n' ' ')"
[ $DEPS_ONLY = 1 ] && exit 0

# ---- the program. Ubuntu names shaderc libshaderc.so.1, SteamOS libshaderc_shared.so.1 with the same C API: linked against a
# stub with that soname and the functions shaders.c calls, so the program needs the system's own at run time
echo "== lasrecomp"
mkdir -p build/stub
for f in $(grep -rhoE 'shaderc_[a-z_0-9]+\(' runtime --include=*.c --include=*.cpp | tr -d '(' | sort -u); do echo "void $f(void) {}"; done > build/stub/shaderc.c
clang --target=$TRIPLE --sysroot="$SYSROOT" -fuse-ld=lld -shared -fPIC -o build/stub/libshaderc_shared.so.1 \
    -Wl,-soname,libshaderc_shared.so.1 build/stub/shaderc.c
# (PATHMAP: the root filesystem's header paths in the debug info are named as "sysroot" too, as the checkout is ".")
nice -n 19 make -j"$JOBS" ARCH=aarch64 SYSROOT="$SYSROOT" DEPS="$P" SHADERC="$ROOT/build/stub/libshaderc_shared.so.1" \
    PATHMAP="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$SYSROOT=/sysroot" build/aarch64/lasrecomp > build/aarch64.log 2>&1 || { tail -n 30 build/aarch64.log >&2; exit 1; }

# ---- collect, as portable.yml does
rm -rf "$OUT" && mkdir -p "$OUT/lib"
cp build/aarch64/lasrecomp "$OUT/lasrecomp"
for l in libSDL3 libavformat libavcodec libavutil libswscale; do
    for f in "$P/lib/$l".so.*; do [ -L "$f" ] || cp "$f" "$OUT/lib/"; done
done
for f in "$OUT"/lib/*; do
    n=$(objdump -p "$f" | awk '$1 == "SONAME" { print $2 }')
    [ "$n" = "$(basename "$f")" ] || mv "$f" "$OUT/lib/$n"
done
# The build's RUNPATH names this tree's deps/lib; the release finds its libraries through $ORIGIN/lib (steam-frame/build.sh does it
# with patchelf, which a desktop may lack). The new text is shorter: written over the old one in .dynstr, the rest blanked.
python3 -I - "$OUT/lasrecomp" '$ORIGIN/lib' << 'PY'
import subprocess, sys
path, new = sys.argv[1], sys.argv[2].encode()
dyn = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
old = [l.split("[")[1].rstrip("]").encode() for l in dyn.splitlines() if "(RUNPATH)" in l or "(RPATH)" in l]
if not old: sys.exit("no RUNPATH in " + path)
data = bytearray(open(path, "rb").read())
sec = [l.split() for l in subprocess.run(["readelf", "-S", "-W", path], capture_output=True, text=True).stdout.splitlines() if ".dynstr" in l]
off, size = int(sec[0][-7], 16), int(sec[0][-6], 16)   # (Off, Size of .dynstr)
for o in old:
    i = data.find(o + b"\0", off, off + size)
    if i < 0 or len(new) > len(o): sys.exit("cannot rewrite the RUNPATH of " + path)
    data[i:i + len(o)] = new + b"\0" * (len(o) - len(new))
open(path, "wb").write(data)
PY
ls -l "$OUT" "$OUT/lib"
check
