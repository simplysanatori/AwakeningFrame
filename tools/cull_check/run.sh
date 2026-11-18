#!/bin/bash
# One VR session under the mock runtime for the cull check (docs/af/STREAM.md "Checking it"):
#   tools/cull_check/run.sh <out dir> <last frame> [VAR=value ...]
# From a scratch copy of the test save in build/cull_check/root (its cache is kept), 120 Hz, 1728 x 1728 eyes, dynamic
# resolution off, the cull log on; the log goes to <out>/game.log, eye dumps (LARECOMP_STEREO_DUMP_AT) to <out>.
# Waits for ~/.cache/headset-in-use, ~/.cache/af-bench-lock and any other lasrecomp, then holds the lock while it runs.
# BINTREE=<checkout>: run that checkout's build; ROOT=<dir>: another scratch root; TMO=<s>: give up after (300).
set -uo pipefail
TREE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd); BINTREE=${BINTREE:-$TREE}
O=$1 LAST=$2; shift 2
LOCK=${AF_BENCH_LOCK:-$HOME/.cache/af-bench-lock}
SAVE=${LARECOMP_SMOKE_SAVE:-$HOME/.local/share/lasrecomp-smoketest/save}
mkdir -p "$O" && O=$(cd "$O" && pwd)
t0=$(date +%s)
while [ -e "${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}" ] || pgrep -x lasrecomp >/dev/null || ! ( set -o noclobber; echo "cull_check $$" > "$LOCK" ) 2>/dev/null; do
    [ $(( $(date +%s) - t0 )) -gt "${WAIT_MAX:-3600}" ] && { echo "cull_check: still busy" >&2; exit 75; }
    sleep 3
done
trap '[ "$(cat "$LOCK" 2>/dev/null)" = "cull_check $$" ] && rm -f "$LOCK"' EXIT
ROOT=${ROOT:-$TREE/build/cull_check/root}
mkdir -p "$ROOT/build" "$ROOT/cache"
ln -sfn "$(readlink -f "$TREE/extracted")" "$ROOT/extracted"
rm -rf "$ROOT/save" "$ROOT/vr_rig.ini" && cp -R "$SAVE" "$ROOT/save"
printf 'aspect = unlocked\nfullscreen = 0\nshow_fps = 0\nresolution = match\naspect_asked = 1\nmenu_tip_seen = 1\n' > "$ROOT/settings.ini"
LOGIN=$(for f in $(seq 300 60 780); do printf '%d:L:12,%d:R:12,%d:A:6,' $f $f $((f + 30)); done)  # (the smoke tests' login)
cd "$ROOT" || exit 1
env -u LARECOMP_XR_YAW ENABLE_GAMESCOPE_WSI=0 SDL_VIDEODRIVER=offscreen LARECOMP_NO_LAUNCHER=1 LARECOMP_NO_AUDIO=1 LARECOMP_NO_UPDATE_CHECK=1 \
    LARECOMP_SETTINGS="$ROOT/settings.ini" LARECOMP_SAVE_DIR="$ROOT/save" LARECOMP_BLUR_REMOVAL=1 LARECOMP_CHEATS=hearts \
    LARECOMP_INPUT="${LOGIN%,}" LARECOMP_VR_RIG_SAVE=0 LARECOMP_LOG_PACE=1 LARECOMP_FPS_INTERVAL=2 \
    LARECOMP_WINDOW_SHOT="$LAST" LARECOMP_SHOT_PATH="$O/window_last.png" LARECOMP_EXIT_AFTER_SHOT=1 \
    XR_RUNTIME_JSON="${XRMOCK_JSON:-$TREE/build/xrmock/runtime.json}" LARECOMP_XR=1 LARECOMP_XR_HZ=120 MOCK_XR_HZ=120 MOCK_XR_EYE_SIZE=1728x1728 \
    MOCK_XR_SECONDS=0 LARECOMP_STEREO_DUMP_DIR="$O" LARECOMP_LOG_CULL=1 LARECOMP_VR_DYNRES=0 "$@" \
    timeout "${TMO:-300}" nice -n 5 "$BINTREE/build/lasrecomp" extracted > "$O/game.log" 2>&1
echo "cull_check: exit $? in $(( $(date +%s) - t0 )) s ($O)"
