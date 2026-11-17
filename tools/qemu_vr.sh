#!/bin/bash
# The Arm64 program in VR mode against the mock runtime, on a desktop: qemu-aarch64-static runs it with the Turnip driver copied
# from the Frame (Mesa's no-op msm shim underneath: nothing is drawn, only what the driver reports and compiles is real).
#   tools/qemu_vr.sh [--bin PATH] [--seconds N] [--data DIR] [--profile]
#   --bin PATH      the program (default build/aarch64/portable/lasrecomp, from steam-frame/cross-build.sh)
#   --seconds N     length of the mock session (default 8; a start screen only without game files)
#   --data DIR      the data folder (default build/aarch64/qemu-run/data, emptied first: pass the folder with extracted/ and a test
#                   save copy to run the game; it needs the game files, which this machine only has when the owner has put them here)
#   --profile       source tools/frame_profile.env first (the Frame's switches are what the driver already picks; useful to see the
#                   lines agree)
# Builds, once, an Arm64 OpenXR loader and the Arm64 mock runtime in build/aarch64/xr (tools/fetch_openxr.sh's source tarball,
# headers from the host's deps). Holds ~/.cache/af-bench-lock while it runs and refuses while ~/.cache/headset-in-use exists.
# Under qemu there is no userfaultfd and no placed memory mapping, so the guest heap falls back to copies; always under timeout.
# Environment: FRAME_SHIM (default $HOME/frame-build/turnip-shim), FRAME_SYSROOT ($HOME/frame-build/sysroot), QEMU_VR_TIMEOUT (seconds, default 300).
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
BIN=build/aarch64/portable/lasrecomp; SECS=8; DATA=build/aarch64/qemu-run/data; PROFILE=0
while [ $# -gt 0 ]; do
    case $1 in
        --bin) BIN=${2:?}; shift ;;
        --seconds) SECS=${2:?}; shift ;;
        --data) DATA=${2:?}; shift ;;
        --profile) PROFILE=1 ;;
        -h | --help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "qemu_vr: unknown argument $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done
SHIM=${FRAME_SHIM:-$HOME/frame-build/turnip-shim}
SYSROOT=${FRAME_SYSROOT:-$HOME/frame-build/sysroot}
fail() { echo "qemu_vr: $*" >&2; exit 1; }
[ -f "$SHIM/env.sh" ] && [ -d "$SYSROOT" ] || fail "no Turnip shim in $SHIM or sysroot in $SYSROOT (FRAME_SHIM, FRAME_SYSROOT)"
command -v qemu-aarch64-static > /dev/null || fail "no qemu-aarch64-static"
[ -x "$BIN" ] || fail "no $BIN (steam-frame/cross-build.sh)"
[ -e "${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}" ] && { echo "qemu_vr: the headset is in use: not running" >&2; exit 75; }
HDR=$(ls -d build/linux/*/deps/include/openxr | head -n 1 | sed 's#/openxr$##')
[ -n "$HDR" ] || fail "no OpenXR headers (tools/fetch_openxr.sh)"
X=$ROOT/build/aarch64/xr
if [ ! -f "$X/prefix/lib/libopenxr_loader.so.1" ]; then
    TAR=${AF_OPENXR_CACHE:-$HOME/.cache/af-openxr}/OpenXR-SDK-1.1.63.tar.gz
    [ -f "$TAR" ] || { tools/fetch_openxr.sh > /dev/null || exit 1; }
    rm -rf "$X/src" && mkdir -p "$X/src" && tar -xzf "$TAR" -C "$X/src" --strip-components 1
    cat > "$X/toolchain.cmake" << EOT
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT "$SYSROOT")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_LINKER_TYPE LLD)
EOT
    nice -n 19 cmake -S "$X/src" -B "$X/b" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$X/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$X/prefix" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF \
        -DBUILD_CONFORMANCE_TESTS=OFF -DDYNAMIC_LOADER=ON > "$X/loader.log" 2>&1 \
        && nice -n 19 ninja -C "$X/b" install >> "$X/loader.log" 2>&1 || { tail -n 20 "$X/loader.log" >&2; exit 1; }
fi
M=tools/xrmock
if [ ! -f "$X/mock/libxrmock.so" ] || [ -n "$(find $M -newer "$X/mock/libxrmock.so" -name '*.[ch]' | head -n 1)" ]; then
    mkdir -p "$X/mock"
    nice -n 19 clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld -O1 -g -w -shared -fPIC -fvisibility=hidden \
        -o "$X/mock/libxrmock.so" $M/xrmock.c $M/xrmock_vk.c -I "$HDR" -I $M -lm -lz -lpthread || exit 1
fi
sed "s#@LIB@#$X/mock/libxrmock.so#" $M/runtime.json.in > "$X/mock/runtime.json"

LOCK=${AF_BENCH_LOCK:-$HOME/.cache/af-bench-lock}
mkdir -p "$(dirname "$LOCK")"
( set -o noclobber; echo "qemu_vr $$" > "$LOCK" ) 2> /dev/null || { echo "qemu_vr: $LOCK is held by: $(cat "$LOCK")" >&2; exit 75; }
trap 'rm -f "$LOCK"' EXIT
case $DATA in build/aarch64/qemu-run/data) rm -rf "$DATA" ;; esac
mkdir -p "$DATA"; DATA=$(cd "$DATA" && pwd)
[ $PROFILE = 1 ] && { set -a; . tools/frame_profile.env; set +a; }
. "$SHIM/env.sh"
export QEMU_LD_PREFIX=$SYSROOT
LOG=$ROOT/build/aarch64/qemu-run/stdout.log
mkdir -p "$(dirname "$LOG")"
rc=0
(cd "$DATA" && XR_RUNTIME_JSON=$X/mock/runtime.json LARECOMP_XR_LOADER=$X/prefix/lib/libopenxr_loader.so.1 LARECOMP_XR=1 \
    LARECOMP_DATA_ROOT=$DATA LARECOMP_NO_AUDIO=1 MOCK_XR_SECONDS="$SECS" MOCK_XR_HZ=120 MOCK_XR_EYE_SIZE=1728x1728 \
    MOCK_XR_POSE=static:0,1.6,0,0,-45 SDL_VIDEODRIVER=offscreen \
    timeout "${QEMU_VR_TIMEOUT:-300}" nice -n 10 qemu-aarch64-static -L "$SYSROOT" "$ROOT/$BIN" "$@") > "$LOG" 2>&1 || rc=$?
echo "exit $rc (124: timed out); logs: $DATA/lasrecomp.log, $LOG"
grep -E '^\[vk\] (using|lean|structured|constant|guest heap|shader global|heap|lazy)|^\[fov\]|^\[mv\]|xrmock: summary' "$DATA/lasrecomp.log" | cut -c1-200
exit $rc
