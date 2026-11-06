#!/bin/bash
# mv_check: does multiview stereo (LARECOMP_MV=1, docs/af/STEREO.md "Multiview") draw what the double-execution stereo
# (v1) draws? Headless VR under the xrmock mock runtime, a static head pose, a warp to each spot, and at a fixed game
# frame the stereo dump (LARECOMP_STEREO_DUMP_AT: eye0 / eye1 / hud PNGs). Two sessions are compared per eye by
# tools/mv_check.py: the share of pixels where any channel differs by more than 24/255.
#
# tools/mv_check.sh [BIN=<lasrecomp>] [--mode mv|passes] [--tag NAME] [--only TEXT] [--limit PCT] [--no-noise]
#                   [--tree DIR] [--dwell FRAMES] [--eye WxH] [--pose SPEC] [--vvl] [--wait] [--wait-max S] [--plan]
#   BIN=PATH        the binary to test (default <tree>/build/lasrecomp; its folder holds lasgame.so); also --bin PATH.
#                   The game files come from --tree (default: this checkout, else the binary's own checkout)
#   --mode mv       (default) base = LARECOMP_MV unset, cand = LARECOMP_MV=1
#   --mode passes   base = LARECOMP_MV=1 LARECOMP_MV_PASSES=0 (layered storage, double execution), cand = LARECOMP_MV=1
#   --no-noise      skip the third session. By default a second base session measures the noise: the game is not
#                   deterministic from run to run (two v1 runs differ by 0 - 18 % of the pixels: butterflies, grass,
#                   NPCs, Link's place in the forest), so the verdict is the share of pixels over the threshold
#                   OUTSIDE those that moved between the two base runs (dilated), at most --limit (1.5 %). Without a
#                   second run the raw share is held to --limit, which a lively spot (Mabe, forest) fails on noise alone
#   --only TEXT     only the spots whose name contains TEXT (case-insensitive)
#   --tag NAME      output folder name (default: timestamp): build/mvcheck/<tag>/ of the tree running it
#   --dwell N       game frames from a warp to its dump (420; the game runs at 30 fps at 90 Hz)
#   --vvl           run the sessions under the validation layer (tools/vvl/vvl.sh)
#   --wait          wait while ~/.cache/headset-in-use exists (default: refuse, exit 75)
#   --wait-max S    give up waiting for the headset / bench lock / another lasrecomp after S seconds (3600, exit 75)
#   --plan          print the spots and sessions and exit
# Output (build/mvcheck/<tag>/): results.md, base/ cand/ [noise/] (eyes/*.png, run.log), heat/ (base | cand | diff),
# spots.tsv, sessions.tsv. Prints a pass/fail table; exit 0 all pass, 1 a FAIL, 2 usage, 75 could not run.
# Each session holds ~/.cache/af-bench-lock while it runs (released after; a session is ~2.5 minutes), never runs beside
# another lasrecomp, and waits for ~/.cache/headset-in-use to clear. Environment: AF_HEADSET_FLAG, AF_BENCH_LOCK, AF_BENCH_PRIORITY,
# MV_CHECK_PYTHON (a Python with PIL), XRMOCK_JSON, VR_TOUR_SAVE (the test save folder holding save_0/).
set -uo pipefail
SELF=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TREE=$(cd "$SELF/.." && pwd)
SAVE=${VR_TOUR_SAVE:-$HOME/.local/share/lasrecomp-smoketest/save}
HEADSET=${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}
LOCK=${AF_BENCH_LOCK:-$HOME/.cache/af-bench-lock}
PRIORITY=${AF_BENCH_PRIORITY:-$HOME/.cache/af-bench-priority}   # a run with priority on the lock: don't take the lock while this exists
BIN= MODE=mv TAG= ONLY= LIMIT=1.5 NOISE=1 DWELL=420 EYE=1680x1760 POSE=static:0,1.6,0,0,-45 VVL=0 WAIT_HEADSET=0 WAIT_MAX=3600 PLAN=0 GTREE=
fail() { echo "mv_check: $*" >&2; exit 2; }
while [ $# -gt 0 ]; do
    case $1 in
        BIN=*) BIN=${1#BIN=} ;;
        --bin) shift; BIN=${1:?} ;;
        --tree) shift; GTREE=${1:?} ;;
        --mode) shift; MODE=${1:?}; case $MODE in mv|passes) ;; *) fail "--mode mv|passes" ;; esac ;;
        --tag) shift; TAG=${1:?} ;;
        --only) shift; ONLY=${1:?} ;;
        --limit) shift; LIMIT=${1:?} ;;
        --no-noise) NOISE=0 ;;
        --dwell) shift; DWELL=${1:?} ;;
        --eye) shift; EYE=${1:?} ;;
        --pose) shift; POSE=${1:?} ;;
        --vvl) VVL=1 ;;
        --wait) WAIT_HEADSET=1 ;;
        --wait-max) shift; WAIT_MAX=${1:?} ;;
        --plan) PLAN=1 ;;
        -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) fail "unknown argument $1 (--help)" ;;
    esac
    shift
done
[ -n "$BIN" ] || BIN=$TREE/build/lasrecomp
BIN=$(readlink -f "$BIN")
[ -x "$BIN" ] || fail "no binary $BIN (make -j6 DEPS=... or BIN=<path>)"
[ -f "$(dirname "$BIN")/lasgame.so" ] || fail "no lasgame.so next to $BIN"
if [ -z "$GTREE" ]; then   # where the game files are: this checkout, else the binary's (<tree>/build/lasrecomp)
    GTREE=$TREE; [ -e "$GTREE/extracted/exefs/main" ] || GTREE=$(cd "$(dirname "$BIN")/.." && pwd)
fi
[ -e "$GTREE/extracted/romfs" ] && [ -e "$GTREE/extracted/exefs/main" ] || fail "no extracted/ game files in $GTREE (--tree DIR)"
[ -d "$SAVE/save_0" ] || fail "no test save in $SAVE (VR_TOUR_SAVE=<folder holding save_0/>)"

# name|level:locator|extra (menu: Home (Guide) opens the settings menu on the HUD quad, and it stays open)
SPOTS=(
    "Mabe Village|Field:Field_11C|"
    "Mysterious Forest|Field:Field_06C|"
    "Marin and Tarin house|MarinTarinHouse:MarinTarinHouse_01A|"
    "Tail Cave 04B|Lv01TailCave:Lv01TailCave_04B|"
    "Koholint beach (Field_09B)|Field:Field_09B|"
    "Menu open (Mabe Village)|Field:Field_11C|menu"
)
if [ -n "$ONLY" ]; then
    SEL=()
    for s in "${SPOTS[@]}"; do [ -n "$(echo "${s%%|*}" | grep -i -F -- "$ONLY")" ] && SEL+=("$s"); done
    [ ${#SEL[@]} -gt 0 ] || fail "no spot matches '$ONLY'"
    SPOTS=("${SEL[@]}")
fi
START=900 STEP=$((DWELL + 120))
# sessions: label|env words (base first)
if [ $MODE = mv ]; then
    BASE_ENV=(-u LARECOMP_MV -u LARECOMP_MV_PASSES);                 BASE_TXT="v1: LARECOMP_MV unset"
    CAND_ENV=(-u LARECOMP_MV_PASSES LARECOMP_MV=1);                  CAND_TXT="LARECOMP_MV=1"
else
    BASE_ENV=(-u LARECOMP_MV_PASSES LARECOMP_MV=1 LARECOMP_MV_PASSES=0); BASE_TXT="LARECOMP_MV=1 LARECOMP_MV_PASSES=0"
    CAND_ENV=(-u LARECOMP_MV_PASSES LARECOMP_MV=1);                  CAND_TXT="LARECOMP_MV=1 (passes on)"
fi
if [ $PLAN = 1 ]; then
    echo "mv_check plan: bin $BIN, game files $GTREE, mode $MODE, eye $EYE, pose $POSE, limit $LIMIT%"
    i=0; for s in "${SPOTS[@]}"; do IFS='|' read -r n l x <<< "$s"; printf '  spot %d: %-28s %-40s warp frame %d, dump frame %d\n' $i "$n" "$l" $((START + i * STEP)) $((START + i * STEP + DWELL)); i=$((i + 1)); done
    echo "  base: $BASE_TXT"; echo "  cand: $CAND_TXT"; [ $NOISE = 1 ] && echo "  noise: $BASE_TXT again"
    exit 0
fi

PY=
for c in "${MV_CHECK_PYTHON:-}" "$HOME/Games/AwakeningFrame/venv-tools/bin/python" "$TREE/tools/venv/bin/python" python3; do
    [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && "$c" -I -c 'import PIL' 2>/dev/null && { PY=$c; break; }
done
[ -n "$PY" ] || fail "no Python with PIL (MV_CHECK_PYTHON=<python>)"

MOCK_JSON=${XRMOCK_JSON:-}
if [ -z "$MOCK_JSON" ]; then
    for w in "$TREE" "$GTREE" $HOME/Games/AwakeningFrame/wt/perf; do
        [ -f "$w/build/xrmock/runtime.json" ] && [ -f "$w/build/xrmock/libxrmock.so" ] && { MOCK_JSON=$w/build/xrmock/runtime.json; break; }
    done
fi
[ -f "$MOCK_JSON" ] || fail "no xrmock (tools/xrmock/build.sh, or XRMOCK_JSON=<runtime.json>)"
if [ -e "$HEADSET" ] && [ $WAIT_HEADSET = 0 ]; then
    echo "mv_check: $HEADSET exists (headset in use): not running (--wait to wait for it)" >&2; exit 75
fi

[ -n "$TAG" ] || TAG=$(date +%Y%m%d-%H%M%S)
OUT=$TREE/build/mvcheck/$TAG
ROOT=$TREE/build/mvcheck/root     # scratch data root: extracted/ -> the game files, cache/ kept between checks, a fresh save per session
rm -rf "$OUT"; mkdir -p "$OUT" "$ROOT/build"
ln -sfn "$(readlink -f "$GTREE/extracted")" "$ROOT/extracted"
if [ ! -d "$ROOT/cache" ]; then
    for w in "$GTREE" "$TREE" $HOME/Games/AwakeningFrame/wt/mv; do [ -d "$w/cache" ] && { cp -R "$w/cache" "$ROOT/cache"; break; }; done
    mkdir -p "$ROOT/cache"
fi
printf 'aspect = unlocked\nfullscreen = 0\nshow_fps = 0\nresolution = match\naspect_asked = 1\nmenu_tip_seen = 1\n' > "$ROOT/settings.ini"

# warps, dumps and the input (the same for every session: the dump frames are the comparison keys)
LOGIN=$(for f in $(seq 300 60 780); do printf '%d:L:12,%d:R:12,%d:A:6,' $f $f $((f + 30)); done)
WARPS= DUMPS= EXTRA=
: > "$OUT/spots.tsv"
i=0
for s in "${SPOTS[@]}"; do
    IFS='|' read -r n l x <<< "$s"
    base=$((START + i * STEP)); WARPS+="$base:$l,"; DUMPS+="$((base + DWELL)),"
    [ "$x" = menu ] && EXTRA+="$((base + 120)):sdlpad:guide:2,"
    printf '%d\t%s\t%d\n' $i "$n" $((base + DWELL)) >> "$OUT/spots.tsv"; i=$((i + 1))
done
LAST=$((START + i * STEP + 30))
INPUT=${LOGIN%,}; HOSTINPUT=${EXTRA%,}   # (the menu's events are host events: LARECOMP_HOST_INPUT, as tools/menu_focus_sweep.sh)

# ---- the machine: the headset, the bench lock, other games
CHILD=0 HELD=0
release() { if [ $HELD = 1 ] && [ -f "$LOCK" ] && [ "$(cut -d' ' -f2 "$LOCK" 2>/dev/null)" = "$$" ]; then rm -f "$LOCK"; fi; HELD=0; }
cleanup() { if [ "$CHILD" != 0 ]; then pkill -TERM -P "$CHILD" 2>/dev/null; kill "$CHILD" 2>/dev/null; fi; release; }
trap cleanup EXIT
trap 'echo "mv_check: interrupted" >&2; exit 130' INT TERM
DEADLINE=$(( $(date +%s) + WAIT_MAX ))
busy_wait() {   # <what> <test command...>: sleep while the test succeeds
    local what=$1; shift; local said=0
    while "$@"; do
        [ $said = 0 ] && echo "mv_check: waiting for $what"; said=1
        [ "$(date +%s)" -gt "$DEADLINE" ] && { echo "mv_check: still waiting for $what after ${WAIT_MAX}s: giving up" >&2; exit 75; }
        sleep 5
    done
}
headset_on() { [ -e "$HEADSET" ]; }
other_game() { pgrep -x lasrecomp >/dev/null 2>&1; }
take_lock() {
    busy_wait "the headset (~/.cache/headset-in-use)" headset_on
    busy_wait "the bench priority flag ($PRIORITY)" test -e "$PRIORITY"
    local said=0   # (polls fast: other batch scripts retake the lock within a second of its release)
    until ( set -o noclobber; echo "mvtools $$" > "$LOCK" ) 2>/dev/null; do
        [ $said = 0 ] && echo "mv_check: waiting for the bench lock ($LOCK: $(cut -d' ' -f1 "$LOCK" 2>/dev/null))"; said=1
        [ -e "$PRIORITY" ] && busy_wait "the bench priority flag ($PRIORITY)" test -e "$PRIORITY"
        [ "$(date +%s)" -gt "$DEADLINE" ] && { echo "mv_check: still waiting for the bench lock after ${WAIT_MAX}s: giving up" >&2; exit 75; }
        sleep 0.2
    done
    HELD=1
    busy_wait "another lasrecomp to end" other_game
    headset_on && { echo "mv_check: the headset went on: stopping" >&2; exit 75; }
}

run_session() {   # <label> <env words...>   (a session: one game, every spot)
    local label=$1; shift
    local S=$OUT/$label; mkdir -p "$S/eyes"
    rm -rf "$ROOT/save" && cp -R "$SAVE" "$ROOT/save"
    local runner=() vv=()
    [ $VVL = 1 ] && { runner=("$TREE/tools/vvl/vvl.sh"); vv=(VVL_NO_WSI=1); }
    take_lock
    echo "mv_check: session $label: $*  (${#SPOTS[@]} spots)"
    local t0; t0=$(date +%s)
    (
        cd "$ROOT" || exit 99
        env -u LARECOMP_XR_QUAD -u LARECOMP_XR_EYE_SCALE -u LARECOMP_XR_YAW -u LARECOMP_XR_PREDICT -u LARECOMP_STEREO -u LARECOMP_XR_LAYOUT "$@" \
            ENABLE_GAMESCOPE_WSI=0 SDL_VIDEODRIVER=offscreen LARECOMP_NO_LAUNCHER=1 LARECOMP_NO_AUDIO=1 LARECOMP_NO_UPDATE_CHECK=1 \
            LARECOMP_SETTINGS="$ROOT/settings.ini" LARECOMP_SAVE_DIR="$ROOT/save" LARECOMP_BLUR_REMOVAL=1 LARECOMP_CHEATS=hearts \
            LARECOMP_SYNC_SHADERS=1 LARECOMP_INPUT="$INPUT" LARECOMP_HOST_INPUT="$HOSTINPUT" LARECOMP_WARP_AT="${WARPS%,}" \
            LARECOMP_STEREO_DUMP_AT="${DUMPS%,}" LARECOMP_STEREO_DUMP_DIR="$S/eyes" \
            LARECOMP_LOG_PACE=1 LARECOMP_FPS_INTERVAL=2 LARECOMP_WINDOW_SHOT=$LAST LARECOMP_SHOT_PATH="$ROOT/window_last.png" LARECOMP_EXIT_AFTER_SHOT=1 \
            XR_RUNTIME_JSON="$MOCK_JSON" LARECOMP_XR=1 LARECOMP_XR_HZ=90 MOCK_XR_HZ=90 MOCK_XR_EYE_SIZE="$EYE" \
            MOCK_XR_POSE="$POSE" MOCK_XR_SECONDS=0 "${vv[@]}" \
            timeout 570 nice -n 5 "${runner[@]}" "$BIN" extracted > "$S/run.log" 2>&1
        echo "rc=$?" >> "$S/run.log"
    ) &
    CHILD=$!
    wait "$CHILD"; CHILD=0
    echo "mv_check: session $label ended after $(( $(date +%s) - t0 )) s ($(tail -1 "$S/run.log"), $(ls "$S/eyes" | wc -l) dump files)"
    release
}

: > "$OUT/sessions.tsv"
run_session base "${BASE_ENV[@]}";  printf 'base\t%s\n' "$BASE_TXT" >> "$OUT/sessions.tsv"
run_session cand "${CAND_ENV[@]}";  printf 'cand\t%s\n' "$CAND_TXT" >> "$OUT/sessions.tsv"
if [ $NOISE = 1 ]; then run_session noise "${BASE_ENV[@]}"; printf 'noise\t%s\n' "$BASE_TXT" >> "$OUT/sessions.tsv"; fi
"$PY" -I "$SELF/mv_check.py" "$OUT" --limit "$LIMIT" --mode "$MODE: $BASE_TXT vs $CAND_TXT (bin $BIN)"
exit $?
