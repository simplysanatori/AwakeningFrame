#!/bin/bash
# Flicker runs: the game in VR mode under xrmock at Mabe Village (Field_11C) in the headset's settings, a run of
# consecutive eye images taken without stalling the loop (MOCK_XR_FLICK), scored by tools/flicker.py
# (docs/af/TOOLING.md "Flicker").
#
# tools/flicker_run.sh <config> [options]
#   configs: default (Surroundings and Table Edge off), backdrop, full, low (Surroundings), fade, passthrough (Table Edge)
#   --runs N        program runs (3); each prints its numbers, then the mean and spread over the runs
#   --head still    the head held still, only the gaze, the eye size and the game moving (the default: no registration)
#   --head move     a head on a neck (tools/xrmock/headpose.py, seeded): images registered by rotation before scoring;
#                   compare a moving run only with another of the same seed
#   --seed S        the head and gaze trace's seed (1; run k uses S + k - 1 unless --same-seed)
#   --same-seed     every run plays the same trace
#   --pose FILE     play this pose file (MOCK_XR_POSE=file: format, gaze columns optional) instead of a generated one
#   --seconds S     images taken for S seconds (8)      --from S   from S seconds into the session (24)
#   --dynres MODE   step (default: 1536 and 1728 px, a second each, LARECOMP_VR_DYNRES_STEP), pin (1536, as the headset's
#                   Backdrop session sat), auto (the GPU decides: on this desktop it stays at full size)
#   --fps30         the game held at 30 fps at 120 Hz (LARECOMP_XR_FPS=30), as the pacer held it in the headset sessions
#   --fov8          no LARECOMP_VR_FOVEATION_TEXEL=16 (on a GPU with 16 px shading-rate texels foveation is then off)
#   --set VAR=VAL   one more environment variable for the game or the mock (repeatable)
#   --keep          keep the images (deleted after scoring otherwise)
#   --tag NAME      a name for this set of runs in the output folder
# Output: build/flicker/<date>_<config>_<head>[_tag]/run<k>/ (run.log, index.txt, flicker.json, strip.png, series.tsv) and
# summary.tsv. The footprint (the table's outline in the still head's view) comes from a `default` run with the still
# head, made first when missing (build/flicker/footprint.npz).
# Takes ~/.cache/af-bench-lock (noclobber, removed on exit) around each program run only, about 45 s each; refuses while
# ~/.cache/headset-in-use exists (exit 75). AF_BENCH_LOCK / AF_HEADSET_FLAG name other files (tests).
set -uo pipefail
SELF=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TREE=$(cd "$SELF/.." && pwd)
SAVE=${FLICKER_SAVE:-$HOME/.local/share/lasrecomp-smoketest/save}
HEADSET=${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}
LOCK=${AF_BENCH_LOCK:-$HOME/.cache/af-bench-lock}
BASE=$TREE/build/flicker
ROOT=$BASE/root

fail() { echo "flicker_run: $*" >&2; exit 2; }
CONFIG=${1:-}
case $CONFIG in
    default) SCENE=off EDGE=black ;;
    backdrop|full|low) SCENE=$CONFIG EDGE=black ;;
    fade|passthrough) SCENE=off EDGE=$CONFIG ;;
    -h|--help|'') sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) fail "unknown config $CONFIG (default backdrop full low fade passthrough)" ;;
esac
shift
RUNS=3 HEAD=still POSE= SEED=1 SAME=0 SECS=8 FROM=24 DYN=step FPS30=0 FOV8=0 KEEP=0 TAG= EXTRA=()
while [ $# -gt 0 ]; do
    case $1 in
        --runs) shift; RUNS=${1:?} ;;
        --head) shift; HEAD=${1:?}; [ "$HEAD" = still ] || [ "$HEAD" = move ] || fail "--head still|move" ;;
        --seed) shift; SEED=${1:?} ;;
        --same-seed) SAME=1 ;;
        --pose) shift; POSE=$(readlink -f "${1:?}") ;;
        --seconds) shift; SECS=${1:?} ;;
        --from) shift; FROM=${1:?} ;;
        --dynres) shift; DYN=${1:?}; case $DYN in step|pin|auto) ;; *) fail "--dynres step|pin|auto" ;; esac ;;
        --fps30) FPS30=1 ;;
        --fov8) FOV8=1 ;;
        --set) shift; EXTRA+=("${1:?}") ;;
        --keep) KEEP=1 ;;
        --tag) shift; TAG=_${1:?} ;;
        *) fail "unknown argument $1 (--help)" ;;
    esac
    shift
done

PY=
for c in "${FLICKER_PYTHON:-}" "$TREE/tools/venv/bin/python" python3; do
    [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && "$c" -I -c 'import numpy, PIL' 2>/dev/null && { PY=$c; break; }
done
[ -n "$PY" ] || fail "no Python with numpy and PIL (FLICKER_PYTHON=<python>)"
[ -x "$TREE/build/lasrecomp" ] || fail "no build/lasrecomp"
[ -f "$TREE/build/lasgame.so" ] || fail "no build/lasgame.so"
[ -e "$TREE/extracted/romfs" ] || fail "no extracted/ game files"
[ -d "$SAVE/save_0" ] || fail "no test save in $SAVE (a folder holding save_0/)"
[ -e "$HEADSET" ] && { echo "flicker_run: $HEADSET exists (headset in use): not running" >&2; exit 75; }
MOCK_JSON=${XRMOCK_JSON:-$("$TREE/tools/xrmock/build.sh" 2>/dev/null | tail -1)}
[ -f "$MOCK_JSON" ] || fail "no xrmock (tools/xrmock/build.sh)"

mkdir -p "$ROOT/build"
ln -sfn "$(readlink -f "$TREE/extracted")" "$ROOT/extracted"
if [ ! -d "$ROOT/cache" ]; then   # (the pipelines: from the tour's cache when there is one, else built on the first run)
    if [ -d "$TREE/build/vr_tour/root/cache" ]; then cp -R "$TREE/build/vr_tour/root/cache" "$ROOT/cache"; else mkdir -p "$ROOT/cache"; fi
fi
printf 'aspect = unlocked\nfullscreen = 0\nshow_fps = 0\nresolution = match\naspect_asked = 1\nmenu_tip_seen = 1\n' > "$ROOT/settings.ini"

# ---- the lock: held around each program run only (another job waits at most five minutes for it)
HELD=0 CHILD=0
release() {
    if [ "$CHILD" != 0 ]; then pkill -TERM -P "$CHILD" 2>/dev/null; kill "$CHILD" 2>/dev/null; CHILD=0; fi
    if [ $HELD = 1 ] && [ "$(cut -d' ' -f2 "$LOCK" 2>/dev/null)" = "$$" ]; then rm -f "$LOCK"; fi
    HELD=0
}
trap release EXIT
trap 'echo "flicker_run: interrupted" >&2; exit 130' INT TERM
take_lock() {
    local said=0
    while :; do
        [ -e "$HEADSET" ] && { echo "flicker_run: the headset went on: stopping" >&2; exit 75; }
        if ! pgrep -x lasrecomp >/dev/null 2>&1 && ( set -o noclobber; echo "flicker_run $$" > "$LOCK" ) 2>/dev/null; then
            if pgrep -x lasrecomp >/dev/null 2>&1; then rm -f "$LOCK"; else HELD=1; return; fi
        fi
        [ $said = 0 ] && echo "flicker_run: waiting for the bench lock / another lasrecomp" && said=1
        sleep 2
    done
}

# the login of the smoke tests (L+R at the title, A through the file menu), then the warp; frames are game frames
LOGIN=$(for f in $(seq 300 60 780); do printf '%d:L:12,%d:R:12,%d:A:6,' $f $f $((f + 30)); done)
GFPS=60; [ $FPS30 = 1 ] && GFPS=30
# the run ends (a window screenshot, then exit) 3 s after the images: the session time runs about with the game frames
LAST=$(( (FROM + SECS + 3) * GFPS ))

game_run() {   # <out dir> <config scene> <edge> <pose file> -> exit status; the run's log in <out>/run.log
    local S=$1 scene=$2 edge=$3 pose=$4
    mkdir -p "$S/img"
    rm -rf "$ROOT/save" && cp -R "$SAVE" "$ROOT/save"
    rm -f "$ROOT/vr_rig.ini"
    local env=(
        ENABLE_GAMESCOPE_WSI=0 SDL_VIDEODRIVER=offscreen LARECOMP_NO_LAUNCHER=1 LARECOMP_NO_AUDIO=1 LARECOMP_NO_UPDATE_CHECK=1
        LARECOMP_SETTINGS="$ROOT/settings.ini" LARECOMP_SAVE_DIR="$ROOT/save" LARECOMP_VR_RIG="$ROOT/vr_rig.ini" LARECOMP_VR_RIG_SAVE=0
        LARECOMP_BLUR_REMOVAL=1 LARECOMP_CHEATS=hearts LARECOMP_INPUT="${LOGIN%,}" LARECOMP_WARP_AT="900:Field:Field_11C"
        LARECOMP_LOG_PACE=1 LARECOMP_WINDOW_SHOT=$LAST LARECOMP_SHOT_PATH="$ROOT/window_last.png" LARECOMP_EXIT_AFTER_SHOT=1
        XR_RUNTIME_JSON="$MOCK_JSON" LARECOMP_XR=1 LARECOMP_XR_HZ=120 MOCK_XR_HZ=120 MOCK_XR_SECONDS=0
        # the headset's eyes: 1728 x 1728, an asymmetric fov mirrored for the right eye (assumed Frame values: the logs
        # of that day do not print them), the head and the gaze from the trace
        MOCK_XR_EYE_SIZE=1728x1728 MOCK_XR_FOV=-52,44,50,-54 MOCK_XR_FOV_RIGHT=mirror
        MOCK_XR_POSE="file:$pose" MOCK_XR_GAZE=file
        MOCK_XR_FLICK="$S/img" MOCK_XR_FLICK_AT="$FROM,$((FROM + SECS))" MOCK_XR_FLICK_SHIFT=2 MOCK_XR_FLICK_EYES=0
        MOCK_XR_TIMING="$S/timing.csv"
        # the headset's settings that day: dynamic resolution with the old budget, foveation high, the copies variant
        # of the Frame profile (no driver-memory heap), Follow Smooth, the table 0.8 m away
        LARECOMP_VR_DYNRES=1 LARECOMP_VR_HEADROOM=0 LARECOMP_VR_FOVEATION=high LARECOMP_VR_FOLLOW=Smooth LARECOMP_VR_DISTANCE=0.8
        LARECOMP_VR_SCENE="$scene" LARECOMP_VR_EDGE="$edge" LARECOMP_VR_BACKDROP_LOG=1 LARECOMP_VR_EDGE_LOG=1
    )
    case $DYN in
        step) env+=(LARECOMP_VR_DYNRES_STEP=1) ;;
        pin) env+=(LARECOMP_VR_DYNRES_MAX=1536) ;;
    esac
    [ $FPS30 = 1 ] && env+=(LARECOMP_XR_FPS=30)
    [ $FOV8 = 0 ] && env+=(LARECOMP_VR_FOVEATION_TEXEL=16)
    env+=("${EXTRA[@]}")
    take_lock
    (
        cd "$ROOT" || exit 99
        set -a; . "$TREE/tools/frame_profile.env"; . "$TREE/tools/frame_profile_copies.env"; set +a
        env -u LARECOMP_XR_QUAD -u LARECOMP_XR_EYE_SCALE -u LARECOMP_XR_YAW -u LARECOMP_XR_PREDICT -u LARECOMP_STEREO -u LARECOMP_XR_LAYOUT \
            "${env[@]}" timeout 110 nice -n 5 "$TREE/build/lasrecomp" extracted 2>&1 |
            "$PY" -I -c 'import sys, time
out = open(sys.argv[1], "wb", buffering=0)
for line in sys.stdin.buffer: out.write(b"%.4f\t" % time.monotonic() + line.rstrip(b"\r\n") + b"\n")' "$S/run.log"
        exit "${PIPESTATUS[0]}"
    ) &
    CHILD=$!
    wait "$CHILD"; local rc=$?
    CHILD=0
    release
    cp "$S/img/index.txt" "$S/index.txt" 2>/dev/null
    return $rc
}

check_run() {   # <out dir>: the warp was taken and settled before the images; prints why not
    "$PY" -I - "$1" "$FROM" <<'EOF'
import sys
d, frm = sys.argv[1], float(sys.argv[2])
begin = None
for l in open(d + "/index.txt"):
    if l.startswith("begin"):
        begin = int(l.split("monotonic_ns=")[1].split()[0]) / 1e9
jump = loaded = None
for l in open(d + "/run.log", errors="replace"):
    t, _, msg = l.partition("\t")
    if "[warp] jumping to Field / Field_11C" in msg: jump = float(t)
    elif jump and loaded is None and "[warp] level-jump request" in msg: loaded = float(t)
if begin is None or loaded is None:
    print("no warp to Field_11C seen"); sys.exit(1)
at = loaded - begin
if at + 3 > frm:
    print("the warp loaded %.1f s into the session, under 3 s before the images (--from %.0f)" % (at, frm)); sys.exit(1)
EOF
}

STAMP=$(date +%Y%m%d-%H%M%S)
FOOT=$BASE/footprint.npz
make_pose() {   # <file> <still|move> <seed>
    # (the rest pose looks 10 degrees down: the table's far side, its edge and the void past it all in view)
    "$PY" -I "$SELF/xrmock/headpose.py" "$1" --seed "$3" --seconds $((FROM + SECS + 40)) --base 0,1.6,0,0,-10 $([ "$2" = still ] && echo --still)
}
if [ ! -f "$FOOT" ]; then
    echo "flicker_run: no footprint yet: one default run with the still head first"
    F=$BASE/footprint_run; rm -rf "$F"; mkdir -p "$F"
    make_pose "$F/head.pose" still 1
    game_run "$F" off black "$F/head.pose"; rc=$?
    why=$(check_run "$F") || fail "the footprint run failed (exit $rc): $why"
    "$PY" -I "$SELF/flicker.py" footprint "$F/img" "$FOOT" || fail "no footprint"
fi

OUT=$BASE/${STAMP}_${CONFIG}_${HEAD}${TAG}
mkdir -p "$OUT"
{
    printf 'config\t%s\nhead\t%s\nseed\t%s\nsame_seed\t%s\nseconds\t%s\nfrom\t%s\ndynres\t%s\nfps30\t%s\nfov8\t%s\nextra\t%s\n' \
        "$CONFIG" "$HEAD" "$SEED" "$SAME" "$SECS" "$FROM" "$DYN" "$FPS30" "$FOV8" "${EXTRA[*]:-}"
    printf 'rev\t%s\ndirty\t%s\n' "$(git -C "$TREE" rev-parse --short HEAD)" "$(git -C "$TREE" status --porcelain --untracked-files=no | wc -l)"
} > "$OUT/meta.tsv"
echo "flicker_run: $CONFIG, head $HEAD, $RUNS runs, images $FROM-$((FROM + SECS)) s, dynres $DYN$([ $FPS30 = 1 ] && echo ', 30 fps') -> $OUT"
for k in $(seq 1 "$RUNS"); do
    R=$OUT/run$k; mkdir -p "$R"
    sd=$SEED; [ $SAME = 0 ] && sd=$((SEED + k - 1))
    if [ -n "$POSE" ]; then cp "$POSE" "$R/head.pose"; else make_pose "$R/head.pose" "$HEAD" "$sd"; fi
    t0=$(date +%s)
    game_run "$R" "$SCENE" "$EDGE" "$R/head.pose"; rc=$?
    if ! why=$(check_run "$R"); then
        echo "flicker_run: run $k not usable (exit $rc): $why"; continue
    fi
    "$PY" -I "$SELF/flicker.py" score "$R/img" --footprint "$FOOT" --out "$R" --json "$R/flicker.json" | sed "s/^flicker:/run $k ($(( $(date +%s) - t0 )) s, seed $sd):/"
    [ $KEEP = 1 ] || rm -rf "$R/img"
done
"$PY" -I - "$OUT" <<'EOF'
import glob, json, sys
out = sys.argv[1]
rs = [json.load(open(f)) for f in sorted(glob.glob(out + "/run*/flicker.json"))]
if not rs: sys.exit("flicker_run: no usable run")
with open(out + "/summary.tsv", "w") as f:
    f.write("region\t" + "\t".join("run%d" % (i + 1) for i in range(len(rs))) + "\tmean\tmin\tmax\n")
    for reg in ("table", "edge", "far"):
        for key in ("mean", "drawn_mean", "p99"):
            v = [100 * (r[reg][key] or 0) for r in rs]  # (None: no pixel of the region in view, or none drawn)
            f.write("%s %s %%\t" % (reg, key) + "\t".join("%.4f" % x for x in v) + "\t%.4f\t%.4f\t%.4f\n" % (sum(v) / len(v), min(v), max(v)))
print("flicker_run: %d runs; flicker, %% of the region's pixels per image (mean of runs, min-max):" % len(rs))
for reg in ("table", "edge", "far"):
    v = [100 * (r[reg]["mean"] or 0) for r in rs]; d = [100 * (r[reg]["drawn_mean"] or 0) for r in rs]
    print("  %-5s %.4f%% (%.4f-%.4f)   of drawn pixels %.4f%% (%.4f-%.4f)" % (reg, sum(v) / len(v), min(v), max(v), sum(d) / len(d), min(d), max(d)))
EOF
