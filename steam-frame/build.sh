#!/bin/bash
# Builds the Steam Frame (Linux arm64) release and packages it for the Releases page, in steam-frame/release/:
#   awakeningframe-<version>-linux-arm64.tar.gz                what Frametanium installs (install.sh at the top level)
#   AwakeningFrame-<version>-framedrop-linux-arm64.zip         the same files plus run.sh, for FrameDrop
#   awakeningframe.framedrop.json                              FrameDrop's manifest (framedrop.install/v1)
#   SHA256SUMS
#   steam-frame/build.sh [version]          (default: the VERSION file at the top of the tree)
# No game code and nothing made from the game is in it: the program (lasrecomp) holds none, and the game code is built
# on each Frame from the player's own game files, by the generator (lasgen/, tools/gen.py frozen by PyInstaller) and the
# compiler (zig/, tools/fetch_zig.sh) shipped with it. hactool (steam-frame/build-hactool.sh) extracts the player's
# NSP or XCI with their own keys. Before packing, the staged files are checked: no game files, game code, shader caches,
# logs or keys, no RUNPATH or path of this machine in the binaries, and steam-frame/privacy-check.sh on the archive.
# The program is built by .github/workflows/portable.yml (steam-frame/portable-deps.sh: SDL3 + FFmpeg); this only packs.
# Run it when the headset is not in use.
#   AF_PORTABLE_DIR=D  (required) the lasrecomp and lib/ that .github/workflows/portable.yml built on glibc 2.39 (its
#                      artifact): D/lasrecomp is packed instead of build/lasrecomp, D/lib/* go in lib/ with RUNPATH
#                      $ORIGIN. A release never carries a program built on the maker's own, newer, SteamOS. The staged
#                      tree must pass steam-frame/tools/check-bundle.sh (glibc <= 2.39, only allowlisted system libraries).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
VERSION=${1:-$(head -n 1 VERSION 2> /dev/null || echo 0.1.0)}
[ -e "${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}" ] && { echo "the headset is in use: not building" >&2; exit 75; }
DEPS=$ROOT/build/linux/aarch64/deps
OUTDIR=$ROOT/steam-frame/release
PKG=$ROOT/build/pkg
NAME=awakeningframe-$VERSION-linux-arm64
ZIPNAME=AwakeningFrame-$VERSION-framedrop-linux-arm64.zip
mkdir -p "$OUTDIR" "$PKG"

# ---- the program, without the game code (that is its own library, never part of a release)
PORT=${AF_PORTABLE_DIR:?AF_PORTABLE_DIR: the portable build (artifact of .github/workflows/portable.yml) is required}
[ -x "$PORT/lasrecomp" ] && ls "$PORT"/lib/libSDL3.so.0 > /dev/null || { echo "$PORT has no lasrecomp and lib/" >&2; exit 1; }
BIN=$PORT/lasrecomp
grep -aq LARECOMP_XR_HZ "$BIN" || { echo "$BIN has no VR mode" >&2; exit 1; }
grep -aqF -- --build-game-code "$BIN" || { echo "$BIN has no --build-game-code" >&2; exit 1; }
if [ "$(nm "$BIN" | grep -c ' f_70' || true)" != 0 ]; then echo "$BIN has game code linked in" >&2; exit 1; fi

# ---- the game code generator (PyInstaller, its own venv) and the compiler (zig, pinned)
V=$PKG/gen-venv
if [ ! -f "$V/.ok" ]; then
    rm -rf "$V" && python3 -m venv "$V"
    "$V/bin/pip" install -q capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5 pyinstaller==6.22.3
    touch "$V/.ok"
fi
"$V/bin/python" -m PyInstaller --noconfirm --clean --log-level WARN --distpath "$PKG/helper" --workpath "$PKG/pyinstaller" \
    tools/lasgen.spec
[ -x build/zig/zig ] && build/zig/zig version | grep -qx 0.17.0 || tools/fetch_zig.sh
[ -x "$PKG/hactool/hactool" ] || steam-frame/build-hactool.sh "$PKG/hactool"

# ---- stage: the top level of the archive (Frametanium runs its install.sh there)
S=$PKG/stage
rm -rf "$S" && mkdir -p "$S/steam-frame/art" "$S/gamelib-src" "$S/runtime/game_addresses"
cp "$BIN" "$S/lasrecomp"
strip --strip-debug "$S/lasrecomp"      # (symbols stay: crash backtraces name functions; debug info names this machine's folders)
# The build's rpath points into the build machine's tree; the libraries are found through RUNPATH $ORIGIN/lib instead.
# libraries are used. patchelf drops the entry but leaves its text in .dynstr: blanked too (all but a last "lib",
# which another name could share), so no path of this machine is left in the binary.
RP=$(readelf -d "$S/lasrecomp" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p; s/.*(RPATH).*\[\(.*\)\]/\1/p')
patchelf --remove-rpath "$S/lasrecomp"
if [ -n "$RP" ]; then
    python3 -I - "$S/lasrecomp" "$RP" << 'PY'
import subprocess, sys
path, rp = sys.argv[1], sys.argv[2].encode()
data = bytearray(open(path, "rb").read())
sec = [l.split() for l in subprocess.run(["readelf", "-S", "-W", path], capture_output=True, text=True).stdout.splitlines() if ".dynstr" in l]
off, size = int(sec[0][-7], 16), int(sec[0][-6], 16)   # (Off, Size of .dynstr)
n = 0
i = data.find(rp + b"\0", off, off + size)
while i >= 0:
    keep = 3 if rp.endswith(b"lib") else 0
    data[i:i + len(rp) - keep] = b"\0" * (len(rp) - keep)
    n += 1
    i = data.find(rp + b"\0", i + 1, off + size)
open(path, "wb").write(data)
print("blanked the old rpath in .dynstr (%d)" % n)
PY
fi
# the libraries it carries (SDL3, FFmpeg; the same soname files they were built with), found through RUNPATH $ORIGIN/lib; each
# one finds its siblings in its own folder
mkdir -p "$S/lib"
cp "$PORT"/lib/*.so* "$S/lib/"
for l in "$S"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$l"; done
strip --strip-unneeded "$S"/lib/*.so*
patchelf --set-rpath '$ORIGIN/lib' "$S/lasrecomp"
cp -a "$PKG/helper/lasgen" "$S/lasgen"
cp -a build/zig "$S/zig"
cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt "$S/gamelib-src/"
cp "$PKG/hactool/hactool" "$S/hactool"
cp "$PKG/hactool/hactool-LICENSE.txt" "$PKG/hactool/mbedtls-LICENSE.txt" "$S/"
cp build/zig/LICENSE "$S/zig-LICENSE.txt"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf \
   third_party/fonts/DejaVu/DejaVuSans.ttf third_party/fonts/DejaVu/DejaVuSans-Bold.ttf "$S/"
cp third_party/fonts/Baloo2/OFL.txt "$S/Baloo2-OFL.txt"
cp third_party/fonts/BIZUDPGothic/OFL.txt "$S/BIZUDPGothic-OFL.txt"
cp third_party/fonts/DejaVu/LICENSE "$S/DejaVu-LICENSE.txt"
cp docs/icon/icon_1024.png "$S/icon.png"
cp install.sh uninstall.sh LICENSE THIRD_PARTY_NOTICES.md README.md AGENTS.md "$S/"
mkdir -p "$S/docs"   # (what README.md links to)
cp docs/USER_GUIDE.md docs/CHANGELOG.md docs/CONTROLS.md docs/TECHNICAL.md "$S/docs/"
cp tools/vrtiming.py "$S/"
cp steam-frame/awakeningframe.sh steam-frame/setup-game.sh steam-frame/prepare-game.sh steam-frame/steam-cef.py \
   steam-frame/set-art.py "$S/steam-frame/"
cp steam-frame/art/*.png "$S/steam-frame/art/"
cp runtime/game_addresses/versions.h "$S/runtime/game_addresses/"   # (build IDs of the supported versions: prepare-game.sh)
echo "$VERSION" > "$S/VERSION"
chmod +x "$S/install.sh" "$S/uninstall.sh" "$S/lasrecomp" "$S/hactool" "$S"/steam-frame/*.sh

# ---- checks: nothing from the game, the player, or this machine
fail=0
bad() { echo "check: $*" >&2; fail=1; }
while IFS= read -r -d '' f; do
    n=${f#"$S"/}
    case "$n" in
        extracted/* | */extracted/* | generated* | */generated/* | *lasgame.* | cache/* | */cache/* | *.log | *prod.keys* | \
        *title.keys* | *.nsp | *.xci | *.nca | */exefs/* | */romfs/* | save/* | *.ini | *pipeline* | *.spv | *shader_list*)
            bad "not for a release: $n" ;;
    esac
done < <(find "$S" -mindepth 1 -print0)
readelf -d "$S/hactool" | grep -E 'RPATH|RUNPATH' > /dev/null && bad "hactool has an rpath"
readelf -d "$S/lasrecomp" | grep -F 'RUNPATH' | grep -qF '[$ORIGIN/lib]' || bad "lasrecomp's RUNPATH is not \$ORIGIN/lib"
readelf -d "$S/lasrecomp" | grep "NEEDED" | grep "libSDL3" > /dev/null || bad "lasrecomp's library list was damaged"
# nothing but glibc <= 2.39 and the libraries on steam-frame/tools/host-libs.allow (all of SteamOS Stable, Beta, Preview)
steam-frame/tools/check-bundle.sh "$S" > "$PKG/check-bundle.txt" 2>&1 || { tail -n 20 "$PKG/check-bundle.txt" >&2; bad "check-bundle failed"; }
tail -n 2 "$PKG/check-bundle.txt"
# (only a program that knows the flag, else it would start the game; in an empty scratch data root, from an empty folder)
if grep -aqF -- --game-code-status "$S/lasrecomp"; then
    E=$(mktemp -d)
    rc=0
    (cd "$E" && LARECOMP_DATA_ROOT=$E timeout 20 "$S/lasrecomp" --game-code-status > /dev/null 2>&1) || rc=$?
    rm -rf "$E"
    [ "$rc" -le 1 ] || bad "the staged lasrecomp does not run (exit $rc)"
else
    bad "the staged lasrecomp has no --game-code-status"
fi
if grep -rlaF -e "$ROOT" -e "$HOME/" -e "/home/$(id -un)/" "$S" > "$PKG/home-hits.txt" 2> /dev/null; then
    bad "paths of this machine in: $(tr '\n' ' ' < "$PKG/home-hits.txt")"
fi
# zig/ is the pinned upstream release as it is (tools/fetch_zig.sh checks its sha256, then only removes folders):
# checked file by file against that archive, it is left out of the text checks below (its sources hold their
# authors' addresses, network examples and the like, none of them ours)
ZT=$PKG/zig-pristine
rm -rf "$ZT" && mkdir -p "$ZT" && tar -xJf build/zig-dl/zig-aarch64-linux-0.17.0.tar.xz -C "$ZT"
zdiff=$(cd "$S/zig" && find . -type f | while IFS= read -r f; do cmp -s "$f" "$ZT/zig-aarch64-linux-0.17.0/$f" || echo "$f"; done | head -3)
[ -z "$zdiff" ] || bad "zig/ differs from the pinned release: $zdiff"
rm -rf "$ZT"
SCAN=$PKG/scan
rm -rf "$SCAN" && cp -al "$S" "$SCAN" && rm -rf "$SCAN/zig"
# (the Python packages' own metadata: their authors' addresses, from PyPI)
find "$SCAN/lasgen" -type d -name '*.dist-info' -prune -exec rm -rf {} +
PAT=${AF_PRIVACY_PATTERNS:-$HOME/.config/moonlight-xr-privacy-patterns.txt}
if [ -f "$PAT" ]; then   # (the maintainer's personal values: name, e-mail, SSID, ... one regular expression per line)
    while IFS= read -r p; do
        [ -n "$p" ] && [ "${p:0:1}" != "#" ] || continue
        grep -rlaiE -e "$p" "$SCAN" > /dev/null 2>&1 && bad "a personal pattern from $PAT matches (not shown)"
    done < "$PAT"
fi
steam-frame/privacy-check.sh "$SCAN" > "$PKG/privacy-check.txt" 2>&1 || bad "privacy-check: $(tail -n 1 "$PKG/privacy-check.txt") ($PKG/privacy-check.txt)"
EMAIL=$(git config user.email 2> /dev/null || true)
[ -n "$EMAIL" ] && grep -rlaiF -e "$EMAIL" "$S" > /dev/null 2>&1 && bad "the git e-mail address is in the files"
[ "$fail" = 0 ] || { echo "not packed: fix the above" >&2; exit 1; }

# ---- pack
TAR=$OUTDIR/$NAME.tar.gz
rm -f "$TAR"
tar --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00' --sort=name -czf "$TAR" -C "$S" .
python3 -I steam-frame/make-framedrop-zip.py "$TAR" --run-sh steam-frame/framedrop-run.sh --repo AwakeningFrame \
    --tag "v$VERSION" --name "AwakeningFrame" --zip-name "$ZIPNAME" --version "$VERSION" --out "$OUTDIR" > /dev/null
(cd "$OUTDIR" && sha256sum "$NAME.tar.gz" "$ZIPNAME" awakeningframe.framedrop.json > SHA256SUMS && cat SHA256SUMS)
du -h "$TAR" "$OUTDIR/$ZIPNAME" | sed 's/^/size /'
