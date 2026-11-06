#!/bin/bash
# Installs AwakeningFrame on the Steam Frame: builds LAS Recomp (with its VR mode) from a source tree,
# copies the program and the launch wrapper to ~/Games/AwakeningFrame/app, and adds an "AwakeningFrame"
# entry to the Steam library, set to 120 Hz in SteamVR, with placeholder art. Steam must be running.
#   ./install.sh [--src DIR] [--no-build] [--skip-steam] [--no-art] [--warm]
#     --src DIR     the source tree to build and install from (default ~/Games/AwakeningFrame/src, or
#                   AWAKENINGFRAME_SRC). It needs the game code: build/lasgame.so, or generated/ (or the game
#                   files, which this script then uses to run `make gen`, minutes).
#     --no-build    install what the tree has already built (also when the build is up to date: make is cheap)
#     --skip-steam  leave Steam's library and SteamVR's settings alone (Steam need not be running)
#     --no-art      no library art
#     --warm        after installing, run tools/warm_shaders.sh: a headless tour of every spot of the game (VR under the
#                   mock runtime, then flat) that fills the shader caches, so the first play has no compile hitches
#                   (docs/af/WARM.md). Optional, long (the first time most of an hour); needs a test save
#                   (~/.local/share/lasrecomp-smoketest/save, or LARECOMP_SMOKE_SAVE) and waits while the headset is in
#                   use. A failed or interrupted warm-up changes nothing else: run tools/warm_shaders.sh again.
#                   Without it the program prepares the shaders it already knows at each start (seconds once warm).
# From an unpacked release (a top-level lasrecomp and lasgame.so next to this script: how Frametanium runs it)
# the program is copied as it is and nothing is built.
# The game files are a separate step (steam-frame/prepare-game.sh) and stay in ~/.local/share/lasrecomp
# (the data folder): they, and the keys, are never copied anywhere else.
#
# For a trial install (tests, packaging checks) these environment variables redirect it:
#   AWAKENINGFRAME_APP=DIR        where the program goes (default ~/Games/AwakeningFrame/app)
#   LARECOMP_DATA_ROOT=DIR        the game's data folder (default ~/.local/share/lasrecomp)
#   AWAKENINGFRAME_SKIP_STEAM=1   as --skip-steam
#   AF_STEAM_DIR=DIR              Steam's folder, for the art files (default ~/.local/share/Steam)
#   AF_CEF_PORT=N                 Steam's debugging port (default 8080)
#   AF_VRCMD=FILE                 SteamVR's vrcmd (default /opt/steamvr/bin/linuxarm64/vrcmd)
#   AF_HEADSET_FLAG=FILE          the headset-in-use marker (default ~/.cache/headset-in-use)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$HERE/steam-frame
[ -d "$TOOLS" ] || TOOLS=$HERE
DEST=${AWAKENINGFRAME_APP:-$HOME/Games/AwakeningFrame/app}
DATA=${LARECOMP_DATA_ROOT:-$HOME/.local/share/lasrecomp}
SRC=${AWAKENINGFRAME_SRC:-$HOME/Games/AwakeningFrame/src}
SKIP_STEAM=${AWAKENINGFRAME_SKIP_STEAM:-}
BUILD=1
ART=1
WARM=0
FLAG=${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}
VRCMD=${AF_VRCMD:-/opt/steamvr/bin/linuxarm64/vrcmd}
CEFPORT=${AF_CEF_PORT:-8080}
STEAM=${AF_STEAM_DIR:-$HOME/.local/share/Steam}
NAME="AwakeningFrame"
export PATH=$HOME/.local/bin:$PATH

while [ $# -gt 0 ]; do
  case "$1" in
    --src) SRC=$2; shift 2 ;;
    --src=*) SRC=${1#--src=}; shift ;;
    --no-build) BUILD=0; shift ;;
    --skip-steam) SKIP_STEAM=1; shift ;;
    --no-art) ART=0; shift ;;
    --warm) WARM=1; shift ;;
    -h | --help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "usage: $0 [--src DIR] [--no-build] [--skip-steam] [--no-art] [--warm]" >&2; exit 2 ;;
  esac
done

headset_check() {
  if [ -e "$FLAG" ]; then
    echo "$FLAG exists (the headset is in use): not building. Run this again when it is gone." >&2
    exit 75
  fi
}

if [ -z "$SKIP_STEAM" ]; then
  curl -s --max-time 3 "127.0.0.1:$CEFPORT/json" > /dev/null || {
    echo "Steam's debugging port is not reachable. Make sure Steam is running (on SteamOS it is on by default)." >&2
    exit 1
  }
fi
if pgrep -f "^$DEST/lasrecomp" > /dev/null 2>&1; then
  echo "The game is running from $DEST. Quit it and run install.sh again." >&2
  exit 1
fi

# ---- where the program comes from
UNPACKED=
[ -x "$HERE/lasrecomp" ] && [ -f "$HERE/lasgame.so" ] && UNPACKED=$HERE
if [ -n "$UNPACKED" ]; then
  FROM=$UNPACKED
  echo "installing the unpacked release in $UNPACKED"
else
  [ -f "$SRC/Makefile" ] && [ -d "$SRC/runtime" ] || { echo "$SRC is not a LAS Recomp source tree (--src DIR)" >&2; exit 1; }
  SRC=$(cd "$SRC" && pwd)
  FROM=$SRC/build
  if [ "$BUILD" = 1 ]; then
    headset_check
    DEPS=$SRC/build/linux/aarch64/deps
    cd "$SRC"
    echo "building in $SRC ($(git rev-parse --short HEAD 2> /dev/null) on $(git branch --show-current 2> /dev/null))"
    # The VR mode needs the OpenXR headers; the VMA header is the renderer's.
    if [ ! -f "$DEPS/include/openxr/openxr.h" ] && [ -x tools/fetch_openxr.sh ]; then tools/fetch_openxr.sh; fi
    if [ ! -f "$DEPS/include/vma/vk_mem_alloc.h" ] && [ -x steam-frame/fetch-vma.sh ]; then steam-frame/fetch-vma.sh; fi
    MK=(make -j"$(nproc)" DEPS="$DEPS")
    command -v ccache > /dev/null && MK+=(CC="ccache clang" CXX="ccache clang++")
    # The game code: made from the game files by `make gen` (minutes), once.
    if [ ! -f build/lasgame.so ] && [ ! -f generated/main/main_build_id.txt ]; then
      [ -f "$DATA/extracted/exefs/main" ] || { echo "no game code yet and no game files in $DATA/extracted (steam-frame/prepare-game.sh first)" >&2; exit 1; }
      [ -x tools/venv/bin/python ] || { echo "tools/venv is missing: python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn" >&2; exit 1; }
      [ -e extracted ] || ln -s "$DATA/extracted" extracted   # (git-ignored; the generator reads extracted/exefs)
      headset_check
      echo "make gen (the game code, a few minutes)"
      nice -n 10 make gen DEPS="$DEPS"
      headset_check
    fi
    nice -n 10 "${MK[@]}"
    cd "$HERE"
  fi
  [ -x "$FROM/lasrecomp" ] || { echo "$FROM/lasrecomp is not built" >&2; exit 1; }
  [ -f "$FROM/lasgame.so" ] || { echo "$FROM/lasgame.so is not built (make gen, then make)" >&2; exit 1; }
fi
grep -q LARECOMP_XR_HZ "$FROM/lasrecomp" || { echo "$FROM/lasrecomp has no VR mode (built without the OpenXR headers: tools/fetch_openxr.sh, then make)" >&2; exit 1; }

# ---- the program; each file is put in place in one step
mkdir -p "$DEST"
for f in lasrecomp lasgame.so lasgame.so.id; do
  [ -f "$FROM/$f" ] || continue
  cp "$FROM/$f" "$DEST/$f.new" && mv "$DEST/$f.new" "$DEST/$f"
done
chmod +x "$DEST/lasrecomp"
# The bundled fonts, next to the program (the game looks for them by name there; without DejaVuSans.ttf it stops with
# "no font for the game's text"). From a release: top-level *.ttf; from a source tree: third_party/fonts/*/.
for f in "$FROM"/*.ttf "$FROM"/../third_party/fonts/*/*.ttf "$SRC"/third_party/fonts/*/*.ttf; do
  [ -f "$f" ] && cp -n "$f" "$DEST/" 2> /dev/null
done
cp "$TOOLS/awakeningframe.sh" "$DEST/awakeningframe.sh"
chmod +x "$DEST/awakeningframe.sh"
cp "$TOOLS/steam-cef.py" "$TOOLS/set-art.py" "$DEST/"
for f in "$UNPACKED/vrtiming.py" "$SRC/tools/vrtiming.py" "$HERE/tools/vrtiming.py"; do
  [ -f "$f" ] && { cp "$f" "$DEST/vrtiming.py"; break; }
done
if [ -d "$TOOLS/art" ]; then
  mkdir -p "$DEST/art"
  cp "$TOOLS"/art/*.png "$DEST/art/" 2> /dev/null || true
fi
echo "installed the program in $DEST"
[ -f "$DATA/extracted/exefs/main" ] || echo "note: no game files in $DATA/extracted yet (steam-frame/prepare-game.sh)"

# ---- the Steam library entry and SteamVR's per-app settings
APPID=none
if [ -z "$SKIP_STEAM" ]; then
  CEF="python3 -I $TOOLS/steam-cef.py"
  EXISTING=$($CEF "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$NAME' && a.app_type === 1073741824) return a.appid; } return 0; })()" 2> /dev/null || echo 0)
  if [ -n "$EXISTING" ] && [ "$EXISTING" != "0" ]; then
    APPID=$EXISTING
    echo "Steam already has a '$NAME' shortcut (appid $APPID), updating it"
    $CEF "(() => { SteamClient.Apps.SetShortcutExe($APPID, '\"$DEST/awakeningframe.sh\"'); SteamClient.Apps.SetShortcutStartDir($APPID, '\"$DEST\"'); return 1; })()" > /dev/null
  else
    APPID=$($CEF "(async () => { const id = await SteamClient.Apps.AddShortcut('$NAME', '$DEST/awakeningframe.sh', '$DEST', ''); SteamClient.Apps.SetShortcutName(id, '$NAME'); return id; })()")
  fi
  # 120 Hz: each 60 fps game frame is shown for exactly two refreshes. No throttling to every other refresh,
  # no motion smoothing (the wrapper sets the same keys at every launch, as SteamVR drops them at times).
  if [ -x "$VRCMD" ]; then
    for kv in preferredRefreshRate=120 framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2; do
      "$VRCMD" --set-settings-int "steam.app.$APPID.${kv%=*}" "${kv#*=}" > /dev/null 2>&1 ||
        echo "warning: could not set SteamVR's ${kv%=*} for the game (is SteamVR running? the launch wrapper sets it again at every start)"
    done
  fi
  # Library art (placeholders, steam-frame/art): the grid folder of the Steam user that has shortcuts, and live.
  if [ "$ART" = 1 ] && [ -d "$DEST/art" ]; then
    GRID=
    for cfg in "$STEAM"/userdata/*/config; do
      [ -d "$cfg" ] || continue
      case "$cfg" in */userdata/0/config) continue ;; esac
      GRID=$cfg/grid
      [ -f "$cfg/shortcuts.vdf" ] && break
    done
    ARGS=(--appid "$APPID" --art "$DEST/art" --live --icon-dest "$DEST/art/icon.png")
    [ -n "$GRID" ] && ARGS+=(--grid "$GRID")
    python3 -I "$TOOLS/set-art.py" "${ARGS[@]}" || echo "warning: could not set the library art"
  fi
fi

# ---- optional: warm the shader caches (tools/warm_shaders.sh), last, so a long run never holds up the rest
if [ "$WARM" = 1 ]; then
  WARMSH=
  for f in "$HERE/tools/warm_shaders.sh" "$HERE/warm_shaders.sh"; do [ -x "$f" ] && { WARMSH=$f; break; }; done
  if [ -z "$WARMSH" ]; then
    echo "warm: warm_shaders.sh is not here, skipping"
  else
    echo "warm: touring the game's spots to fill the shader caches (waits while the headset is in use; Ctrl-C leaves the install as it is)"
    AWAKENINGFRAME_SRC="$SRC" LARECOMP_DATA_ROOT="$DATA" "$WARMSH" --app "$DEST" --root "$DATA" || echo "warm: not finished (run $WARMSH again; the install itself is done)"
  fi
fi

cat << MSG

Installed in $DEST.
$([ -z "$SKIP_STEAM" ] && echo "Steam library entry \"$NAME\" (appid $APPID) runs $DEST/awakeningframe.sh.")
Game files: steam-frame/prepare-game.sh <your game> <prod.keys>  (once; they live in $DATA/extracted)
Launch it from the library with SteamVR running. For a flat window to check things: $DEST/awakeningframe.sh --flat
Logs: $(dirname "$DEST")/logs/run-<time>.log (and xr_timing-, lasrecomp-, vrtiming-<time>.log), the game's own in $DATA
MSG
