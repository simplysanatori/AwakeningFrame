#!/bin/bash
# Warms the shader caches: a headless tour that warps through every warp spot of the game (overworld, houses and caves,
# dungeons: runtime/ui/warp_data.h, tools/warp_table.py) so every shader / pipeline variant the game builds is made once
# and kept (docs/af/WARM.md). Two sessions: VR mode under the mock OpenXR runtime (tools/xrmock) and the flat window,
# because the VR executor draws into other targets than the flat path does. Nothing is shown and no SteamVR is used.
#
#   tools/warm_shaders.sh [--app DIR] [--root DIR] [--save DIR] [--flat-only | --vr-only] [--per N] [--chunk N]
#                         [--start I] [--end I] [--dry-run]
#     --app DIR     the program folder (lasrecomp + lasgame.so): default ./build of the source tree, or $AWAKENINGFRAME_APP
#                   (~/Games/AwakeningFrame/app) when there is no build here
#     --root DIR    the game's data folder (extracted/, cache/): default $LARECOMP_DATA_ROOT or ~/.local/share/lasrecomp.
#                   What the tour learns goes to <root>/cache: vk_pipelines.bin, spirv/, shader_seen.txt (the list of
#                   variants beyond the shipped one, which the next start prepares)
#     --save DIR    a folder holding save_0/ for the tour to play from (a COPY is used; default $LARECOMP_SMOKE_SAVE or
#                   ~/.local/share/lasrecomp-smoketest/save)
#     --per N       frames each spot renders (default 150; the tour's own minimum is 120)
#     --chunk N     spots per game session (default 90): a crash or an interruption costs one chunk
#     --start I --end I  only spots I..END-1 of the table (short tours, tests)
#     --dry-run     print the plan and the first command, run nothing
# The program, the settings and the save are the user's own, copied: the real saves and settings are never written.
# Before every launch: waits while ~/.cache/headset-in-use or ~/.cache/af-bench-lock exists or another lasrecomp is running
# (polls every 2 minutes); a session in progress is stopped (and its chunk redone) if the headset marker or the
# benchmark lock appears.
# Exit status: 0 all chunks done, 1 a chunk failed, 2 a setup problem.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
APP=${AWAKENINGFRAME_APP:-}
ROOT=${LARECOMP_DATA_ROOT:-$HOME/.local/share/lasrecomp}
SAVE=${LARECOMP_SMOKE_SAVE:-$HOME/.local/share/lasrecomp-smoketest/save}
PER=150 CHUNK=90 START=0 END=0 DO_VR=1 DO_FLAT=1 DRY=0
HEADSET=${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}
BENCH=${AF_BENCH_LOCK:-$HOME/.cache/af-bench-lock}
POLL=${WARM_POLL:-120}

while [ $# -gt 0 ]; do
    case $1 in
        --app) APP=$2; shift ;;
        --root) ROOT=$2; shift ;;
        --save) SAVE=$2; shift ;;
        --per) PER=$2; shift ;;
        --chunk) CHUNK=$2; shift ;;
        --start) START=$2; shift ;;
        --end) END=$2; shift ;;
        --flat-only) DO_VR=0 ;;
        --vr-only) DO_FLAT=0 ;;
        --dry-run) DRY=1 ;;
        -h | --help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "warm_shaders: unknown argument $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done
fail() { echo "warm_shaders: $*" >&2; exit 2; }

if [ -z "$APP" ]; then
    if [ -x "$REPO/build/lasrecomp" ] && [ -f "$REPO/build/lasgame.so" ]; then APP=$REPO/build; else APP=$HOME/Games/AwakeningFrame/app; fi
fi
[ -x "$APP/lasrecomp" ] && [ -f "$APP/lasgame.so" ] || fail "no program in $APP (lasrecomp and lasgame.so: build, or install.sh)"
[ -f "$ROOT/extracted/exefs/main" ] || fail "no game files in $ROOT/extracted (steam-frame/prepare-game.sh)"
[ -d "$SAVE/save_0" ] || fail "no test save in $SAVE (a folder with save_0/: start a new game once, copy the data folder's save/ there)"
TOTAL=$(grep -c '^    {[0-9],' "$REPO/runtime/ui/warp_data.h" 2> /dev/null || echo 0)
[ "$TOTAL" -gt 0 ] || TOTAL=410
[ "$END" -gt 0 ] && [ "$END" -le "$TOTAL" ] || END=$TOTAL

# ---- VR under the mock runtime
MOCKJSON=
if [ "$DO_VR" = 1 ]; then
    # (the source tree this script is in, or AWAKENINGFRAME_SRC / ~/Games/AwakeningFrame/src: install.sh --src DIR)
    TREES=("$REPO" "${AWAKENINGFRAME_SRC:-$HOME/Games/AwakeningFrame/src}")
    for d in "$APP/xrmock" "${TREES[@]/%//build/xrmock}"; do [ -f "$d/runtime.json" ] && [ -f "$d/libxrmock.so" ] && { MOCKJSON=$d/runtime.json; break; }; done
    for t in "${TREES[@]}"; do
        [ -z "$MOCKJSON" ] && [ -x "$t/tools/xrmock/build.sh" ] && [ "$DRY" = 0 ] && {
            MOCKJSON=$("$t/tools/xrmock/build.sh" 2> /dev/null | tail -1)
            [ -f "$MOCKJSON" ] || MOCKJSON=
        }
    done
    if [ -z "$MOCKJSON" ] && [ "$DRY" = 0 ]; then
        echo "warm_shaders: no mock OpenXR runtime (tools/xrmock/build.sh needs the source tree): flat session only" >&2
        DO_VR=0
    fi
    grep -q LARECOMP_XR_HZ "$APP/lasrecomp" || { echo "warm_shaders: $APP/lasrecomp has no VR mode: flat session only" >&2; DO_VR=0; }
fi
[ "$DO_VR" = 1 ] || [ "$DO_FLAT" = 1 ] || fail "nothing to do"

WORK=${WARM_WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/awakeningframe-warm}
mkdir -p "$WORK"
LOGDIR=$WORK/logs
mkdir -p "$LOGDIR"
# the user's own settings (shaders depend on them: anti-aliasing, blur removal, frame interpolation), as a scratch copy
if [ -f "$ROOT/larecomp.ini" ]; then cp "$ROOT/larecomp.ini" "$WORK/settings.ini"; else printf 'aspect = unlocked\n' > "$WORK/settings.ini"; fi
grep -q '^menu_tip_seen' "$WORK/settings.ini" || echo 'menu_tip_seen = 1' >> "$WORK/settings.ini"
grep -q '^aspect_asked' "$WORK/settings.ini" || echo 'aspect_asked = 1' >> "$WORK/settings.ini"

# the title screen: L+R, then A through the file menu (as tools/freecam_sweep.sh)
IN=$(for f in $(seq 600 100 6000); do printf '%d:L:12,%d:R:12,%d:A:6,' $f $f $((f + 50)); done)
IN=${IN%,}

clear_to_run() {  # waits while the headset, the benchmark or another lasrecomp is there
    local why
    while [ -e "$HEADSET" ] || [ -e "$BENCH" ] || pgrep -x lasrecomp > /dev/null 2>&1; do
        why=
        [ -e "$HEADSET" ] && why+="headset in use; "
        [ -e "$BENCH" ] && why+="benchmark lock; "
        pgrep -x lasrecomp > /dev/null 2>&1 && why+="a lasrecomp is running"
        echo "warm_shaders: $(date +%T) waiting ($why)"
        sleep "$POLL"
    done
}

run_chunk() {  # <mode vr|flat> <from> <to> : one game session; 0 = the tour reached its end, 1 = not, 2 = stopped for a lock
    local mode=$1 from=$2 to=$3 tag log n limit
    tag=$mode-$from
    log=$LOGDIR/tour-$tag.log
    n=$((to - from))
    limit=$((420 + n * (PER / 20 + 14)))
    local xr=()
    [ "$mode" = vr ] && xr=(XR_RUNTIME_JSON="$MOCKJSON" LARECOMP_XR=1 MOCK_XR_SECONDS=0 MOCK_XR_EYE_SIZE=1728x1728 MOCK_XR_POSE=static:0,1.6,0,0,-45)
    local cmd=(env ENABLE_GAMESCOPE_WSI=0 SDL_VIDEODRIVER=offscreen LARECOMP_DATA_ROOT="$ROOT" LARECOMP_NO_LAUNCHER=1 LARECOMP_NO_AUDIO=1
        LARECOMP_NO_UPDATE_CHECK=1 LARECOMP_CHEATS=hearts LARECOMP_SETTINGS="$WORK/settings.ini" LARECOMP_SAVE_DIR="$WORK/save"
        LARECOMP_INPUT="$IN" LARECOMP_SHADER_PREP=1 LARECOMP_SHADER_PREP_BUDGET=900 LARECOMP_SHADER_TOUR="$PER" LARECOMP_TOUR_START="$from" LARECOMP_TOUR_END="$to"
        LARECOMP_FPS_INTERVAL=5 "${xr[@]}" nice -n 10 "$APP/lasrecomp")
    if [ "$DRY" = 1 ]; then echo "[dry run] cd $APP && ${cmd[*]} > $log"; return 0; fi
    clear_to_run
    rm -rf "$WORK/save" && cp -R "$SAVE" "$WORK/save"
    echo "warm_shaders: $(date +%T) $mode spots $from..$((to - 1)) of $TOTAL (log $log)"
    (cd "$APP" && "${cmd[@]}" > "$log" 2>&1) &
    local p=$! t=0
    while kill -0 $p 2> /dev/null; do
        sleep 5
        t=$((t + 5))
        if [ -e "$HEADSET" ] || [ -e "$BENCH" ]; then
            echo "warm_shaders: headset / benchmark lock appeared: stopping this session, redoing it later" >&2
            kill $p 2> /dev/null; sleep 3; kill -9 $p 2> /dev/null; wait $p 2> /dev/null
            return 2
        fi
        if [ $t -ge "$limit" ]; then
            echo "warm_shaders: session timed out after ${limit}s" >&2
            kill $p 2> /dev/null; sleep 3; kill -9 $p 2> /dev/null; wait $p 2> /dev/null
            return 1
        fi
    done
    wait $p 2> /dev/null
    # the game's own log is in the data folder
    [ -f "$ROOT/lasrecomp.log" ] && cp "$ROOT/lasrecomp.log" "$LOGDIR/game-$tag.log"
    grep -q '^\[tour\] done' "$LOGDIR/game-$tag.log" 2> /dev/null
}

rc=0
modes=()
[ "$DO_VR" = 1 ] && modes+=(vr)
[ "$DO_FLAT" = 1 ] && modes+=(flat)
[ "$DRY" = 1 ] && [ "$DO_VR" = 1 ] && [ -z "$MOCKJSON" ] && MOCKJSON='<build/xrmock/runtime.json>'
echo "warm_shaders: $APP, data $ROOT, spots $START..$((END - 1)) of $TOTAL, $PER frames each, ${modes[*]}"
t_all=$(date +%s)
for mode in "${modes[@]}"; do
    i=$START
    while [ $i -lt "$END" ]; do
        to=$((i + CHUNK))
        [ $to -gt "$END" ] && to=$END
        tries=0
        while :; do
            run_chunk "$mode" "$i" "$to"
            r=$?
            [ $r = 0 ] && break
            [ $r = 2 ] && continue  # (stopped for the headset / benchmark: again, after the wait)
            tries=$((tries + 1))
            if [ $tries -ge 2 ]; then echo "warm_shaders: $mode spots $i..$((to - 1)) did not finish (see $LOGDIR)" >&2; rc=1; break; fi
            echo "warm_shaders: $mode spots $i..$((to - 1)) did not finish, again" >&2
        done
        [ "$DRY" = 1 ] && break 2
        i=$to
    done
done
[ "$DRY" = 1 ] && exit 0
echo "warm_shaders: done in $(( $(date +%s) - t_all )) s, exit $rc"
CACHE=$ROOT/cache
echo "caches in $CACHE: pipelines $(du -h "$CACHE/vk_pipelines.bin" 2> /dev/null | cut -f1), SPIR-V $(du -sh "$CACHE/spirv" 2> /dev/null | cut -f1) ($(ls "$CACHE/spirv" 2> /dev/null | wc -l) files), seen list $(grep -c '^[GC] ' "$CACHE/shader_seen.txt" 2> /dev/null || echo 0) records"
exit $rc
