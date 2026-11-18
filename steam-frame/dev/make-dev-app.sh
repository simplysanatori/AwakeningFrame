#!/bin/bash
# Builds a test app folder for the Steam Frame on this desktop: the Arm64 program and the Arm64 game code, cross-built
# against the glibc 2.39 root filesystem the release uses, packed like the release's app folder, stamped "dev" and the commit.
#   steam-frame/dev/make-dev-app.sh [--out DIR] [--no-build] [--regen] [--no-kit]
#   --out DIR    where the app folders go (default ~/Games/AwakeningFrame-dev; the app is DIR/app-<commit>)
#   --no-build   pack what build/aarch64 already holds (no program build, no game code step)
#   --regen      make the game code's C again even if it looks current (make ARCH=aarch64 gen)
#   --no-kit     leave DIR's scripts (run.sh, ...) and art as they are
# The game code is made from the owner's game files (extracted/ in the tree, a link to the data folder): it, generated-aarch64/
# and build/aarch64/ are git-ignored and the app folder lives outside the repository. Never copy an app folder into a release.
# Builds hold the lock AF_CROSS_LOCK (default ~/frame-build/build-af-dev.lock), nice 19, at most 8 jobs (JOBS).
# Environment: SYSROOT (the glibc 2.39 root filesystem, as steam-frame/cross-build.sh), AF_HEADSET_FLAG.
# Next: steam-frame/dev/install-to-frame.sh DIR/app-<commit> (with the owner's OK).
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=$HOME/Games/AwakeningFrame-dev; BUILD=1; REGEN=0; KIT=1
while [ $# -gt 0 ]; do
    case $1 in
        --out) OUT=${2:?}; shift ;;
        --no-build) BUILD=0 ;;
        --regen) REGEN=1 ;;
        --no-kit) KIT=0 ;;
        -h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "make-dev-app: unknown argument $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done
[ "$(uname -m)" = aarch64 ] && { echo "make-dev-app: this machine is aarch64: build there with make" >&2; exit 2; }
[ -e "${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}" ] && { echo "the headset is in use: not building" >&2; exit 75; }
export SYSROOT=${SYSROOT:-$HOME/frame-build/primedframe/sysroot-portable}
export AF_CROSS_LOCK=${AF_CROSS_LOCK:-$HOME/frame-build/build-af-dev.lock}
JOBS=${JOBS:-8}; [ "$JOBS" -gt 8 ] && JOBS=8
mkdir -p "$(dirname "$AF_CROSS_LOCK")"
COMMIT=$(git rev-parse --short=9 HEAD)
DIRTY=; [ -n "$(git status --porcelain --untracked-files=no)" ] && DIRTY=-dirty
BASE=$(head -n 1 VERSION)
STAMP=$BASE-dev-$COMMIT$DIRTY
APP=$OUT/app-$COMMIT$DIRTY
P=$ROOT/build/linux/aarch64/deps
B=build/aarch64
under_lock() { flock "$AF_CROSS_LOCK" nice -n 19 "$@"; }
MK=(make -j"$JOBS" ARCH=aarch64 SYSROOT="$SYSROOT" DEPS="$P" SHADERC="$ROOT/build/stub/libshaderc_shared.so.1"
    PATHMAP="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$SYSROOT=/sysroot")

if [ $BUILD = 1 ]; then
    [ -e extracted/exefs/main ] || { echo "no game files: extracted/ here must lead to the data folder's" >&2; exit 1; }
    # ---- the program (steam-frame/cross-build.sh: SDL3, FFmpeg, the program, RUNPATH, check-bundle, a start under qemu); the stamp is
    # in main.o only, so it is built again now and once more after (the next plain build then has no stamp)
    rm -f $B/rt/main.o $B/lasrecomp
    AF_BUILD_STAMP=$STAMP JOBS=$JOBS steam-frame/cross-build.sh
    rm -f $B/rt/main.o
    # ---- the game code: C from the game files for Arm64 (generated-aarch64/), then the library
    GENID=$(git rev-parse HEAD:tools):$(git rev-parse HEAD:runtime/game_addresses):$(stat -c '%s:%Y' extracted/exefs/main extracted/exefs/sdk | tr '\n' :)
    if [ $REGEN = 1 ] || [ "$(cat generated-aarch64/.gen-id 2> /dev/null)" != "$GENID" ] || [ ! -f generated-aarch64/main/main_build_id.txt ]; then
        echo "== make ARCH=aarch64 gen (the game code's C, a few minutes)"
        under_lock make ARCH=aarch64 SYSROOT="$SYSROOT" gen > $B/gen.log 2>&1 || { tail -n 20 $B/gen.log >&2; exit 1; }
        echo "$GENID" > generated-aarch64/.gen-id
    fi
    echo "== lasgame.so (clang -O1, $JOBS jobs; the first time about 25 minutes)"
    under_lock "${MK[@]}" $B/lasgame.so > $B/lasgame.log 2>&1 || { tail -n 20 $B/lasgame.log >&2; exit 1; }
fi
[ -x $B/portable/lasrecomp ] && [ -f $B/lasgame.so ] && [ -f $B/lasgame.so.id ] || { echo "no $B/portable/lasrecomp or $B/lasgame.so: run without --no-build" >&2; exit 1; }
grep -aqF "$BASE-dev-$COMMIT" $B/portable/lasrecomp || echo "note: the program holds no stamp for this commit (a --no-build pack of an older build)"

# ---- the app folder, like the release's: program, lib/, launcher, fonts, game code + id
rm -rf "$APP.new" && mkdir -p "$APP.new/lib" "$APP.new/steam-frame"
cp $B/portable/lasrecomp "$APP.new/lasrecomp"
llvm-strip --strip-debug "$APP.new/lasrecomp"   # (symbols stay: crash backtraces name functions)
cp $B/portable/lib/*.so* "$APP.new/lib/"
cp $B/lasgame.so $B/lasgame.so.id "$APP.new/"
cp steam-frame/awakeningframe.sh "$APP.new/"
cp steam-frame/steam-cef.py steam-frame/set-art.py "$APP.new/"
cp tools/vrtiming.py "$APP.new/"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf \
   third_party/fonts/DejaVu/DejaVuSans.ttf third_party/fonts/DejaVu/DejaVuSans-Bold.ttf "$APP.new/"
cp third_party/fonts/Baloo2/OFL.txt "$APP.new/Baloo2-OFL.txt"
cp third_party/fonts/BIZUDPGothic/OFL.txt "$APP.new/BIZUDPGothic-OFL.txt"
cp third_party/fonts/DejaVu/LICENSE "$APP.new/DejaVu-LICENSE.txt"
cp LICENSE THIRD_PARTY_NOTICES.md "$APP.new/"
rmdir "$APP.new/steam-frame"
echo "$STAMP" > "$APP.new/VERSION"
echo "dev $COMMIT$DIRTY $(date -u +%Y-%m-%dT%H:%MZ)" > "$APP.new/.af-install"
chmod +x "$APP.new/lasrecomp" "$APP.new/awakeningframe.sh"
steam-frame/tools/check-bundle.sh "$APP.new" | tail -n 2
# nothing of this machine in the program, the libraries or the game code (the folder name of this tree, the home folder)
if grep -rlaF -e "$ROOT" -e "$HOME/" -e "/home/$(id -un)/" "$APP.new" > "$APP.new.hits" 2> /dev/null; then
    echo "make-dev-app: paths of this machine in: $(tr '\n' ' ' < "$APP.new.hits")" >&2; rm -f "$APP.new.hits"; exit 1
fi
rm -f "$APP.new.hits"
rm -rf "$APP" && mv "$APP.new" "$APP"
echo "app folder: $APP ($(du -sh "$APP" | cut -f1)), $STAMP, game code id $(cat "$APP/lasgame.so.id")"

# ---- the kit around it: the launcher, the scripts for the headset, the art, the mock runtime for the bench
if [ $KIT = 1 ]; then
    for f in run.sh setup-dev-home.sh install-to-frame.sh collect-logs.sh frame-bench.sh add-dev-shortcut.py make-dev-art.sh session.env.example; do
        cp steam-frame/dev/$f "$OUT/$f"
    done
    chmod +x "$OUT"/*.sh "$OUT"/add-dev-shortcut.py
    cp steam-frame/steam-cef.py steam-frame/set-art.py "$OUT/"
    steam-frame/dev/make-dev-art.sh "$OUT/art-dev" > /dev/null
    mkdir -p "$OUT/bench"
    clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld -O1 -w -shared -fPIC -fvisibility=hidden \
        -o "$OUT/bench/libxrmock.so" tools/xrmock/xrmock.c tools/xrmock/xrmock_vk.c -I "$(dirname "$(ls -d build/linux/*/deps/include/openxr | head -n 1)")" \
        -I tools/xrmock -lm -lz -lpthread
    llvm-strip --strip-debug "$OUT/bench/libxrmock.so"
    cp tools/xrmock/runtime.json.in "$OUT/bench/"
    echo "kit: $OUT (scripts, art-dev/, bench/libxrmock.so)"
fi
echo "next: steam-frame/dev/install-to-frame.sh $APP   (with the owner's OK)"
