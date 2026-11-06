#!/bin/bash
# VR tour: a headless regression check of the VR renderer (docs/af/TOUR.md). One command that runs VR mode under the
# xrmock mock OpenXR runtime from a copy of the test save, warps through the spots below at 90 and 120 Hz with a head
# sway and a scripted walk, and writes a compact report:
#   <tree>/build/vr_tour/<timestamp>/report.md   one row per spot (OK / SLOW / BROKEN) with fps, GPU / render CPU, pose
#                                                mismatches, validation errors, crashes, image checks
#                                      sheet.png contact sheet of every spot's eye pair (left | right) and HUD quad
#                                      spots/    the eye pair and HUD quad PNGs per spot (downscaled)
#                                      data.json what --compare reads;  raw/<session>/  the stamped logs, timing CSVs
#
# tools/vr_tour.sh [--quick] [--tree DIR] [--hz "90 120"] [--only TEXT] [--vvl] [--compare OLD_DIR]
#   --quick         3 spots (Mabe Village, Marin and Tarin house, Tail Cave 04B), 90 Hz only; under 3 minutes
#   --tree DIR      the source tree to check (default: this checkout); needs build/lasrecomp, build/lasgame.so, extracted/
#   --hz LIST       display refresh rates to run (default "90 120"); the screenshots come from the first
#   --only TEXT     only the spots whose name contains TEXT (case-insensitive)
#   --vvl           one more session under the Khronos validation layer (tools/vvl/vvl.sh): errors counted per spot
#   --compare OLD   after the run, compare with the report directory OLD (fps drops over 10 %, big image changes, a
#                   swapped eye order, a missing HUD); exit status 1 when something is flagged
#   --diff OLD NEW  compare two existing report directories (no run)
#   --dwell S       seconds per spot in the timing sessions (12; --quick 10)    --shot-dwell S  screenshot session (10)
#   --eye WxH       the mock's eye size (1680x1760, the Frame's default at 120 Hz)
#   --wait          wait while ~/.cache/headset-in-use exists (default: refuse, exit 75)
#   --wait-max S    give up waiting for the bench lock / another lasrecomp after S seconds (3600, exit 75)
#   --plan          print the spots and sessions and exit        --keep-raw   keep the full-size mock PNGs
#   --mv-check      instead of the tour: tools/mv_check.sh on this tree (v1 vs LARECOMP_MV=1 images; --only, --eye, --wait,
#                   --wait-max and --plan pass through; MV_CHECK_ARGS="--mode passes --noise" adds more); exit status of the check
#   --no-walk       no scripted walk (a walk that reaches a door starts a level change, which delays the next warp)
# Waits for ~/.cache/af-bench-lock and for any other lasrecomp, takes the lock while it runs (released on exit), uses nice.
# Environment: AF_HEADSET_FLAG / AF_BENCH_LOCK (other paths for the two flags, for tests), VR_TOUR_PYTHON=<python with PIL>, VR_TOUR_SAVE=<test save folder holding save_0/>, XRMOCK_JSON=<runtime.json>.
set -uo pipefail
SELF=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HELP=$SELF/vr_tour
TREE=$(cd "$SELF/.." && pwd)
SAVE=${VR_TOUR_SAVE:-$HOME/.local/share/lasrecomp-smoketest/save}
HEADSET=${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}
LOCK=${AF_BENCH_LOCK:-$HOME/.cache/af-bench-lock}

MVCHECK=0 NOWALK=0 QUICK=0 ONLY= VVL=0 COMPARE= HZS= DWELL= SHOT_DWELL= EYE=1680x1760 WAIT_HEADSET=0 WAIT_MAX=3600 PLAN=0 KEEP_RAW=0 DIFF_OLD= DIFF_NEW=
while [ $# -gt 0 ]; do
    case $1 in
        --quick) QUICK=1 ;;
        --tree) shift; TREE=$(cd "${1:?--tree needs a directory}" && pwd) ;;
        --hz) shift; HZS=${1:?} ;;
        --only) shift; ONLY=${1:?} ;;
        --vvl) VVL=1 ;;
        --compare) shift; COMPARE=${1:?--compare needs a report directory} ;;
        --diff) DIFF_OLD=${2:?--diff OLD NEW}; DIFF_NEW=${3:?--diff OLD NEW}; shift 2 ;;
        --dwell) shift; DWELL=${1:?} ;;
        --shot-dwell) shift; SHOT_DWELL=${1:?} ;;
        --eye) shift; EYE=${1:?} ;;
        --wait) WAIT_HEADSET=1 ;;
        --wait-max) shift; WAIT_MAX=${1:?} ;;
        --no-walk) NOWALK=1 ;;
        --plan) PLAN=1 ;;
        --keep-raw) KEEP_RAW=1 ;;
        --mv-check) MVCHECK=1 ;;
        -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "vr_tour: unknown argument $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done

fail() { echo "vr_tour: $*" >&2; exit 2; }

if [ $MVCHECK = 1 ]; then   # the multiview image check (tools/mv_check.sh) instead of the tour
    MVA=(--tree "$TREE" --eye "$EYE")
    [ -n "$ONLY" ] && MVA+=(--only "$ONLY")
    [ $WAIT_HEADSET = 1 ] && MVA+=(--wait)
    [ $PLAN = 1 ] && MVA+=(--plan)
    # shellcheck disable=SC2086
    exec "$SELF/mv_check.sh" BIN="$TREE/build/lasrecomp" "${MVA[@]}" --wait-max "$WAIT_MAX" ${MV_CHECK_ARGS:-}
fi

# name|level|locator  (the free-camera sweep's list, tools/freecam_sweep.sh; runtime/ui/warp_data.h)
SPOTS=(
    "Mabe Village|Field|Field_11C"
    "Mysterious Forest|Field|Field_06C"
    "Koholint beach (Field_09B)|Field|Field_09B"
    "Marin and Tarin house|MarinTarinHouse|MarinTarinHouse_01A"
    "Tail Cave entrance|Lv01TailCave|Lv01TailCave_08D"
    "Tail Cave room 04B|Lv01TailCave|Lv01TailCave_04B"
    "Bottle Grotto room 05D|Lv02BottleGrotto|Lv02BottleGrotto_05D"
)
if [ $QUICK = 1 ]; then
    SPOTS=("${SPOTS[0]}" "${SPOTS[3]}" "${SPOTS[5]}")
    : "${HZS:=90}" "${DWELL:=10}" "${SHOT_DWELL:=10}"
fi
: "${HZS:=90 120}" "${DWELL:=12}" "${SHOT_DWELL:=10}"
if [ -n "$ONLY" ]; then
    SEL=()
    for s in "${SPOTS[@]}"; do [ -n "$(echo "${s%%|*}" | grep -i -F -- "$ONLY")" ] && SEL+=("$s"); done
    [ ${#SEL[@]} -gt 0 ] || fail "no spot matches '$ONLY'"
    SPOTS=("${SEL[@]}")
fi
read -ra HZL <<< "$HZS"

PY=
for c in "${VR_TOUR_PYTHON:-}" "$HOME/Games/AwakeningFrame/venv-tools/bin/python" "$TREE/tools/venv/bin/python" python3; do
    [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && "$c" -I -c 'import PIL' 2>/dev/null && { PY=$c; break; }
done
[ -n "$PY" ] || fail "no Python with PIL (pip install pillow in a venv, then VR_TOUR_PYTHON=<python>)"

if [ -n "$DIFF_OLD" ]; then
    exec "$PY" -I "$HELP/tour.py" compare "$DIFF_OLD" "$DIFF_NEW"
fi

# ---- the schedule of one session: STEP game frames a spot (the game runs at 30 fps while the display is below 100 Hz)
SESSIONS=()   # kind:hz:dwell
SESSIONS+=("shots:${HZL[0]}:$SHOT_DWELL")
for hz in "${HZL[@]}"; do SESSIONS+=("timing:$hz:$DWELL"); done
[ $VVL = 1 ] && SESSIONS+=("vvl:${HZL[0]}:6")
START=900
gamefps() { [ "$1" -lt 100 ] && echo 30 || echo 60; }

if [ $PLAN = 1 ]; then
    echo "vr_tour plan (tree $TREE): ${#SPOTS[@]} spots, eye $EYE"
    i=0; for s in "${SPOTS[@]}"; do IFS='|' read -r n l c <<< "$s"; printf '  spot %d: %-28s %s / %s\n' $i "$n" "$l" "$c"; i=$((i + 1)); done
    for se in "${SESSIONS[@]}"; do
        IFS=: read -r k hz d <<< "$se"
        printf '  session %-6s %3d Hz: %ss a spot = %d game frames at %d fps, ~%ds + boot%s\n' "$k" "$hz" "$d" $((d * $(gamefps "$hz"))) "$(gamefps "$hz")" $((d * ${#SPOTS[@]})) \
            "$([ "$k" = shots ] && echo ', MOCK_XR_SHOT every 4 s')"
    done
    exit 0
fi

# ---- setup checks
[ -x "$TREE/build/lasrecomp" ] || fail "no $TREE/build/lasrecomp (afbuild.sh, or make -j5 DEPS=...)"
[ -f "$TREE/build/lasgame.so" ] || fail "no $TREE/build/lasgame.so (make gen; make)"
[ -e "$TREE/extracted/romfs" ] && [ -e "$TREE/extracted/exefs/main" ] || fail "no extracted/ game files in $TREE (steam-frame/prepare-game.sh)"
[ -d "$SAVE/save_0" ] || fail "no test save in $SAVE (a folder holding save_0/; see tools/freecam_sweep.sh)"
if [ -e "$HEADSET" ] && [ $WAIT_HEADSET = 0 ]; then
    echo "vr_tour: $HEADSET exists (headset in use): not running (--wait to wait for it)" >&2; exit 75
fi

MOCK_JSON=${XRMOCK_JSON:-}
if [ -z "$MOCK_JSON" ]; then
    if [ -f "$TREE/build/xrmock/runtime.json" ] && [ -f "$TREE/build/xrmock/libxrmock.so" ]; then
        MOCK_JSON=$TREE/build/xrmock/runtime.json
    elif [ -x "$TREE/tools/xrmock/build.sh" ] && [ ! -e "$HEADSET" ]; then
        echo "vr_tour: building the mock runtime in $TREE/build/xrmock"
        MOCK_JSON=$("$TREE/tools/xrmock/build.sh" 2>/dev/null | tail -1) || MOCK_JSON=
    fi
fi
if [ -z "$MOCK_JSON" ] || [ ! -f "$MOCK_JSON" ]; then   # another checkout's mock will do (it only needs the loader)
    for w in $(git -C "$TREE" worktree list --porcelain 2>/dev/null | sed -n 's/^worktree //p'); do
        [ -f "$w/build/xrmock/runtime.json" ] && { MOCK_JSON=$w/build/xrmock/runtime.json; echo "vr_tour: using the mock from $w"; break; }
    done
fi
[ -f "$MOCK_JSON" ] || fail "no xrmock (tools/fetch_openxr.sh && tools/xrmock/build.sh, or XRMOCK_JSON=<runtime.json>)"

TS=$(date +%Y%m%d-%H%M%S)
BASE=$TREE/build/vr_tour
OUT=$BASE/$TS
ROOT=$BASE/root          # the scratch data root: extracted/ -> the tree's, cache/ kept between tours, a fresh save copy per session

# ---- the machine: wait for it, take the bench lock
CHILD=0
release() {
    if [ "$CHILD" != 0 ]; then pkill -TERM -P "$CHILD" 2>/dev/null; kill "$CHILD" 2>/dev/null; fi
    if [ -f "$LOCK" ] && [ "$(cut -d' ' -f2 "$LOCK" 2>/dev/null)" = "$$" ]; then rm -f "$LOCK"; fi
}
trap release EXIT
trap 'echo "vr_tour: interrupted" >&2; exit 130' INT TERM

DEADLINE=$(( $(date +%s) + WAIT_MAX ))
wait_for() {   # <what> <test command...>: poll while the test succeeds (the thing is busy)
    local what=$1; shift; local said=0
    while "$@"; do
        [ "$said" = 0 ] && echo "vr_tour: waiting for $what"; said=1
        [ "$(date +%s)" -gt "$DEADLINE" ] && { echo "vr_tour: still waiting for $what after ${WAIT_MAX}s: giving up" >&2; exit 75; }
        sleep 2
    done
}
other_lasrecomp() { pgrep -x lasrecomp >/dev/null 2>&1; }
headset_on() { [ -e "$HEADSET" ]; }
lock_held_by_other() { [ -e "$LOCK" ]; }
wait_for "the headset (~/.cache/headset-in-use)" headset_on
wait_for "the bench lock (~/.cache/af-bench-lock)" lock_held_by_other
until ( set -o noclobber; echo "vr_tour $$" > "$LOCK" ) 2>/dev/null; do wait_for "the bench lock (~/.cache/af-bench-lock)" lock_held_by_other; done
wait_for "another lasrecomp to end" other_lasrecomp
T_LOCK=$(date +%s)

mkdir -p "$OUT/raw" "$OUT/spots" "$ROOT/build"
ln -sfn "$(readlink -f "$TREE/extracted")" "$ROOT/extracted"
if [ ! -d "$ROOT/cache" ]; then   # the first tour would compile every pipeline from scratch: start from the tree's own cache
    if [ -d "$TREE/cache" ]; then cp -R "$TREE/cache" "$ROOT/cache"; else mkdir -p "$ROOT/cache"; fi
fi
printf 'aspect = unlocked\nfullscreen = 0\nshow_fps = 0\nresolution = match\naspect_asked = 1\nmenu_tip_seen = 1\n' > "$ROOT/settings.ini"

# ---- git facts and the table the analysis reads
{
    printf 'tree\t%s\n' "$TREE"
    printf 'rev\t%s\n' "$(git -C "$TREE" rev-parse --short HEAD 2>/dev/null)"
    printf 'branch\t%s\n' "$(git -C "$TREE" branch --show-current 2>/dev/null)"
    printf 'dirty\t%s\n' "$(git -C "$TREE" status --porcelain --untracked-files=no 2>/dev/null | wc -l)"
    printf 'binary\t%s\n' "$(date -r "$TREE/build/lasrecomp" +%Y-%m-%dT%H:%M:%S)"
    printf 'mock\t%s\n' "$MOCK_JSON"
    printf 'eye\t%s\n' "$EYE"
    printf 'date\t%s\n' "$(date -Is)"
    printf 'quick\t%s\n' "$QUICK"
    printf 'start_frame\t%s\n' "$START"
} > "$OUT/meta.tsv"
i=0; : > "$OUT/spots.tsv"
for s in "${SPOTS[@]}"; do IFS='|' read -r n l c <<< "$s"; printf '%d\t%s\t%s\t%s\n' $i "$n" "$l" "$c" >> "$OUT/spots.tsv"; i=$((i + 1)); done
: > "$OUT/sessions.tsv"

# the login of the smoke tests: L+R at the title, A through the file menu (the perf lane's bench)
LOGIN=$(for f in $(seq 300 60 780); do printf '%d:L:12,%d:R:12,%d:A:6,' $f $f $((f + 30)); done)

run_session() {   # <kind> <hz> <dwell s>
    local kind=$1 hz=$2 dwell=$3
    local name=${kind}_${hz} S fps step n=${#SPOTS[@]} i s l c lev loc
    S=$OUT/raw/$name; mkdir -p "$S/shots"
    fps=$(gamefps "$hz"); step=$((dwell * fps))
    rm -rf "$ROOT/save" "$ROOT/build/frames" && cp -R "$SAVE" "$ROOT/save"
    # warps at START + k * step (the save is loaded by frame ~700); a walk in the middle of each spot's time: right and back, left and back
    # (sideways only and short: the warp points are at doors, and a door of Link's own starts a level change the next warp waits for)
    local warps= walk= base q=$((fps * 4 / 10)) first=
    for i in $(seq 0 $((n - 1))); do
        IFS='|' read -r s lev loc <<< "${SPOTS[$i]}"
        base=$((START + i * step)); warps+="$base:$lev:$loc,"
        if [ $NOWALK = 0 ] && [ "$kind" != shots ]; then   # (the screenshot session stands still: a walking camera glides, and a dump then shows it in two states)
            local w=$((base + step * 50 / 100))
            walk+="$w:STICK_RIGHT:$q,$((w + q)):STICK_LEFT:$q,$((w + 2 * q)):STICK_LEFT:$q,$((w + 3 * q)):STICK_RIGHT:$q,"
        fi
    done
    local last=$((START + n * step))
    # the head: 1.6 m up, looking 35 degrees down (the table is 45 down from the start pose), a sway of +-15 degrees yaw (0.4 Hz) and 4 cm sideways (0.25 Hz)
    awk 'BEGIN { pi = 3.14159265; for (i = 0; i <= 12000; i++) { t = i * 0.1;
        printf "%.1f %.3f 1.6 0 %.2f %.2f 0\n", t, 0.04 * sin(2 * pi * 0.25 * t), 15 * sin(2 * pi * 0.4 * t), -35 + 3 * sin(2 * pi * 0.2 * t) } }' > "$S/pose.txt"
    local mockenv=(MOCK_XR_HZ="$hz" MOCK_XR_EYE_SIZE="$EYE" MOCK_XR_POSE="file:$S/pose.txt" MOCK_XR_SECONDS=0 MOCK_XR_TIMING="$S/timing.csv" MOCK_XR_TRACE="$S/trace.txt")
    [ "$kind" = shots ] && mockenv+=(MOCK_XR_SHOT="every:$((hz * 4))" MOCK_XR_SHOT_DIR="$S/shots")
    local runner=()
    if [ "$kind" = vvl ]; then
        runner=("$TREE/tools/vvl/vvl.sh"); mockenv+=(VVL_NO_WSI=1)   # (the layer's messages go to stderr: into the stamped log)
    fi
    local tmo=$((300 + n * dwell * 4))
    wait_for "another lasrecomp to end" other_lasrecomp
    headset_on && { echo "vr_tour: the headset went on: stopping" >&2; exit 75; }
    local t0 t1 rc
    t0=$(date +%s.%N)
    echo "vr_tour: session $name: $n spots, $dwell s each, $hz Hz (game at $fps fps, $step frames a spot)"
    (
        cd "$ROOT" || exit 99
        env -u LARECOMP_XR_QUAD -u LARECOMP_XR_EYE_SCALE -u LARECOMP_XR_YAW -u LARECOMP_XR_PREDICT -u LARECOMP_STEREO -u LARECOMP_XR_LAYOUT \
            ENABLE_GAMESCOPE_WSI=0 SDL_VIDEODRIVER=offscreen LARECOMP_NO_LAUNCHER=1 LARECOMP_NO_AUDIO=1 LARECOMP_NO_UPDATE_CHECK=1 \
            LARECOMP_SETTINGS="$ROOT/settings.ini" LARECOMP_SAVE_DIR="$ROOT/save" LARECOMP_BLUR_REMOVAL=1 LARECOMP_CHEATS=hearts \
            LARECOMP_INPUT="${LOGIN}${walk%,}" LARECOMP_WARP_AT="${warps%,}" \
            LARECOMP_LOG_PACE=1 LARECOMP_FPS_INTERVAL=2 LARECOMP_WINDOW_SHOT=$last LARECOMP_SHOT_PATH="$ROOT/window_last.png" LARECOMP_EXIT_AFTER_SHOT=1 \
            XR_RUNTIME_JSON="$MOCK_JSON" LARECOMP_XR=1 LARECOMP_XR_HZ="$hz" "${mockenv[@]}" \
            timeout "$tmo" nice -n 5 "${runner[@]}" "$TREE/build/lasrecomp" extracted 2>&1 | python3 -I "$HELP/stamp.py" "$S/run.log"
        exit "${PIPESTATUS[0]}"
    ) &
    CHILD=$!
    wait "$CHILD"; rc=$?
    CHILD=0
    t1=$(date +%s.%N)
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$kind" "$hz" "$dwell" "$step" "$rc" "$t0" "$t1" "$S" >> "$OUT/sessions.tsv"
    echo "vr_tour: session $name ended (exit $rc, $(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.0f", b - a }') s)"
}

for se in "${SESSIONS[@]}"; do
    IFS=: read -r kind hz dwell <<< "$se"
    run_session "$kind" "$hz" "$dwell"
done
release   # (the timing-sensitive part is over)
echo "vr_tour: runs took $(( $(date +%s) - T_LOCK )) s; analysing"

ARGS=("$OUT")
[ $KEEP_RAW = 1 ] && ARGS+=(--keep-raw)
[ -n "$COMPARE" ] && ARGS+=(--compare "$COMPARE")
"$PY" -I "$HELP/tour.py" report "${ARGS[@]}"
exit $?
