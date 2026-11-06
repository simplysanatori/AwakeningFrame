#!/bin/bash
# Free-camera probe sweep (docs/af/FREECAM.md): one game session from a test save warps through the spots below, and at
# each one steps the camera through the stock view, identity and a grid of orbit angles (stock and widened culling),
# with a screenshot of the game image after each step. Result: build/freecam/ with
#   shots/NN_<spot>_<variant>.png   every screenshot          sheet_<n>_<spot>.png one labelled contact sheet per spot
#   sheet.png                       one row per spot          report.txt        void fraction (black pixels) per shot
#   game.log, freecam.log           the run's log / its [freecam] lines
#   suspects.txt                    camera data outside cbuf 8: matrix copies, other cameras, screen-space draws (sorted)
#   floats48.txt                    the camera buffer's floats 48-79 dumps and what they matched
#
# tools/freecam_sweep.sh [--quick] [--only <text>] [--plan | --dry-run]
#   --quick       yaw 0/90, pitch 60, one distance (a fifth of the shots)
#   --only <text> just the spots whose name contains <text> (case-insensitive)
#   --plan        print the spots, camera steps and frames, then exit (no game files needed)
#   --dry-run     check the setup, print the exact command, then exit
# tools/freecam_sweep.sh --identity-check: one run, identity and off alternating in blocks of 8 frames, a shot of every
#   frame; the mean difference of consecutive shots must be the same in both modes and across a switch (the game is not
#   deterministic between runs, so a pixel-for-pixel comparison of two runs is not possible).
# Needs extracted/ (steam-frame/prepare-game.sh), the built game code (make gen; make), build/lasrecomp, and a test save:
# $LARECOMP_SMOKE_SAVE (default ~/.local/share/lasrecomp-smoketest/save, a folder holding save_0/; made by starting a new
# game once and copying the data folder's save/ there; never written to). Refuses to run while ~/.cache/headset-in-use exists.
# Environment: FREECAM_WIDE=<aspect>:<fovy scale> (the widened culling, default 3.2:1.5), FREECAM_SETTLE / _GAP / _SHOT
# (frames: after the warp before the first step, between steps, from the step to its screenshot; 480 / 60 / 30),
# FREECAM_SESSION_SECONDS (kill limit, default 3600).
set -uo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
O=$REPO/build/freecam
SAVE=${LARECOMP_SMOKE_SAVE:-$HOME/.local/share/lasrecomp-smoketest/save}
WIDE=${FREECAM_WIDE:-3.2:1.5}
SETTLE=${FREECAM_SETTLE:-480} GAP=${FREECAM_GAP:-60} SHOTOFF=${FREECAM_SHOT:-30}
LIMIT=${FREECAM_SESSION_SECONDS:-3600}
START=7000  # (as tools/linux/smoketest.sh: the save is in play by ~frame 5500, the first warp is taken 60 frames after)

QUICK=0 ONLY= MODE=run
while [ $# -gt 0 ]; do
    case $1 in
        --quick) QUICK=1 ;;
        --only) shift; ONLY=${1:-} ;;
        --plan) MODE=plan ;;
        --dry-run) MODE=dry ;;
        --identity-check) MODE=identity ;;
        -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "freecam_sweep: unknown argument $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done

# name|level|locator  (tools/warp_table.py / runtime/ui/warp_data.h; the smoketest's Mabe Village and dungeon entrances)
SPOTS=(
    "Mabe Village|Field|Field_11C"
    "Mysterious Forest|Field|Field_06C"
    "Koholint beach|Field|Field_09B"
    "Marin and Tarin house|MarinTarinHouse|MarinTarinHouse_01A"
    "Tail Cave entrance|Lv01TailCave|Lv01TailCave_08D"
    "Tail Cave room 04B|Lv01TailCave|Lv01TailCave_04B"
    "Bottle Grotto room 05D|Lv02BottleGrotto|Lv02BottleGrotto_05D"
)
if [ -n "$ONLY" ]; then
    SEL=()
    for s in "${SPOTS[@]}"; do
        [ -n "$(echo "${s%%|*}" | grep -i -F -- "$ONLY")" ] && SEL+=("$s")
    done
    SPOTS=("${SEL[@]+"${SEL[@]}"}")
    [ ${#SPOTS[@]} -gt 0 ] || { echo "freecam_sweep: no spot matches '$ONLY'" >&2; exit 2; }
fi

# camera steps: label|mode|cull  (cull: stock or the widened one)
STEPS=("game|off|stock" "identity|identity|stock")
if [ $QUICK = 1 ]; then YAWS="0 90" PITCHES="60" DISTS="30"; else YAWS="0 45 90 180" PITCHES="35 60" DISTS="30 50"; fi
for d in $DISTS; do for p in $PITCHES; do for y in $YAWS; do
    for c in stock wide; do
        STEPS+=("o${y}_p${p}_d${d}_${c}|orbit:${y}:${p}:${d}|$([ $c = wide ] && echo "$WIDE" || echo stock)")
    done
done; done; done
NSTEPS=${#STEPS[@]}
STEP=$((SETTLE + (NSTEPS + 1) * GAP + 200))  # frames per spot

slug() { echo "$1" | tr -c 'A-Za-z0-9\n' '_' | sed 's/_*$//'; }

# the schedule, warps and screenshot frames of a run over the given spots; sets AT, WARPS, SHOTS, LAST, FIRST
plan_run() {
    AT= WARPS= SHOTS= FIRST= LAST=0
    local i=0 s name lvl loc base j f
    for s in "$@"; do
        IFS='|' read -r name lvl loc <<< "$s"
        base=$((START + i * STEP))
        if [ $i = 0 ]; then FIRST="$lvl:$loc"; else WARPS+="$base:$lvl:$loc,"; fi
        for j in "${!STEPS[@]}"; do
            IFS='|' read -r _ mode cull <<< "${STEPS[$j]}"
            f=$((base + SETTLE + j * GAP))
            AT+="$f:$mode/$cull,"
            SHOTS+="$((f + SHOTOFF)),"
        done
        AT+="$((base + SETTLE + NSTEPS * GAP)):off/stock,"  # (stock view again for the next warp)
        LAST=$((base + SETTLE + NSTEPS * GAP + 60))
        i=$((i + 1))
    done
    AT=${AT%,} WARPS=${WARPS%,} SHOTS=${SHOTS%,}
}

if [ $MODE = plan ]; then
    plan_run "${SPOTS[@]}"
    echo "freecam_sweep plan: ${#SPOTS[@]} spots x $NSTEPS camera steps = $((${#SPOTS[@]} * NSTEPS)) screenshots, $STEP frames per spot, ends at frame $((LAST))"
    echo "widened culling: $WIDE   settle $SETTLE, gap $GAP, shot offset $SHOTOFF"
    i=0
    for s in "${SPOTS[@]}"; do IFS='|' read -r name lvl loc <<< "$s"; printf '  spot %d: %-26s %s / %s  (warp at frame %d)\n' $i "$name" "$lvl" "$loc" $((START + i * STEP)); i=$((i + 1)); done
    echo "camera steps:"; for st in "${STEPS[@]}"; do IFS='|' read -r l m c <<< "$st"; printf '  %-24s LARECOMP_FREECAM=%-22s culling %s\n' "$l" "$m" "$c"; done
    exit 0
fi

# ---- setup checks
fail() { echo "freecam_sweep: $*" >&2; exit 2; }
[ -d extracted/romfs ] && [ -f extracted/exefs/main ] || fail "no extracted/ game files in this checkout (steam-frame/prepare-game.sh <game.nsp|xci> <prod.keys> --dest $REPO/extracted), then make gen && make"
[ -x build/lasrecomp ] || fail "no build/lasrecomp (afbuild.sh, or make -j5 DEPS=...)"
[ -d "$SAVE/save_0" ] || fail "no test save in $SAVE (see the header of this script)"
[ -e "$HOME/.cache/headset-in-use" ] && { echo "freecam_sweep: ~/.cache/headset-in-use exists (headset in use): not running" >&2; exit 75; }
PY=
for c in "${FREECAM_PYTHON:-}" python3 tools/venv/bin/python "$(git rev-parse --git-common-dir 2>/dev/null)/../tools/venv/bin/python"; do
    [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && "$c" -I -c 'import PIL' 2>/dev/null && { PY=$c; break; }
done
[ -n "$PY" ] || fail "no Python with PIL for the contact sheets (tools/venv/bin/pip install pillow, or FREECAM_PYTHON=<python>)"

# the login-through-the-title-screen input of the smoketest (L+R at the title, A through the file menu)
IN=$(for f in $(seq 600 100 6000); do printf '%d:L:12,%d:R:12,%d:A:6,' $f $f $((f + 50)); done)
IN=${IN%,}

run_game() {  # <log> <window shot frame> <extra env...>: one session from a scratch copy of the save (cwd: the repo)
    local log=$1 last=$2; shift 2
    rm -rf "$O/save" build/frames && cp -R "$SAVE" "$O/save"
    mkdir -p build/test_ini && : > "$O/settings.ini"
    until mkdir build/.freecam.lock 2>/dev/null; do sleep 1; done
    trap 'rmdir build/.freecam.lock 2>/dev/null' EXIT
    env LARECOMP_NO_LAUNCHER=1 LARECOMP_NO_AUDIO=1 LARECOMP_NO_UPDATE_CHECK=1 LARECOMP_SETTINGS="$O/settings.ini" LARECOMP_SAVE_DIR="$O/save" \
        LARECOMP_BLUR_REMOVAL=1 LARECOMP_CHEATS=hearts LARECOMP_INPUT="$IN" LARECOMP_WINDOW_SHOT="$last" LARECOMP_SHOT_PATH="$O/window_last.png" LARECOMP_EXIT_AFTER_SHOT=1 \
        "$@" ./build/lasrecomp extracted > "$log" 2>&1 &
    local p=$!
    local t
    for t in $(seq 1 "$LIMIT"); do kill -0 $p 2>/dev/null || break; sleep 1; done
    if kill -0 $p 2>/dev/null; then kill $p 2>/dev/null; sleep 2; kill -9 $p 2>/dev/null; echo "freecam_sweep: session timed out after ${LIMIT}s" >&2; rmdir build/.freecam.lock 2>/dev/null; trap - EXIT; return 1; fi
    wait $p; local rc=$?
    rmdir build/.freecam.lock 2>/dev/null; trap - EXIT
    return $rc
}

if [ $MODE = identity ]; then
    # One session, 8-frame blocks alternating identity / off over 128 consecutive frames at the first spot, a shot of every
    # frame. The game is not deterministic from run to run (the warp lands on another frame, the world moves all the time),
    # so two runs never match pixel for pixel: instead the mean abs difference of consecutive shots is compared by kind
    # (off-off, identity-identity, across a switch); identity is fine when all three agree. The log line says whether the
    # rebuilt P*V reproduced the game's VP bit for bit (then identity binds the game's exact 320 bytes).
    mkdir -p "$O"
    s=${SPOTS[0]}; IFS='|' read -r name lvl loc <<< "$s"
    F=$((START + SETTLE + 20)); N=144
    echo "freecam_sweep: identity check at $name, frames $F..$((F + N - 1))"
    AT=; SH=
    for i in $(seq 0 15); do f=$((F + 4 + i * 8)); [ $((i % 2)) = 0 ] && AT+="$f:identity/stock," || AT+="$f:off/stock,"; done
    SH=$(seq -s, "$F" $((F + N - 1)))
    run_game "$O/identity.log" $((F + N + 20)) LARECOMP_WARP_ON_LOAD="$lvl:$loc" LARECOMP_SHOT_AT="$SH" LARECOMP_FREECAM_LOG=1 LARECOMP_FREECAM=off \
        LARECOMP_FREECAM_AT="${AT%,}" || fail "the run failed (see $O/identity.log)"
    mkdir -p "$O/identity" && rm -f "$O/identity"/*.png && cp build/frames/frame_*.png "$O/identity/"
    "$PY" -I - "$O/identity" "$F" $((F + N - 1)) $((F + 4)) <<'PYEOF'
import sys, statistics as st
from PIL import Image, ImageChops, ImageStat
d, f0, f1, at = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
ims = {f: Image.open('%s/frame_%05d.png' % (d, f)).convert('L') for f in range(f0, f1 + 1)}
def mode(f): return 'off' if f < at else 'identity' if ((f - at) // 8) % 2 == 0 else 'off'
res = {'off-off': [], 'identity-identity': [], 'across a switch': []}
for a in range(f0, f1):
    v = ImageStat.Stat(ImageChops.difference(ims[a], ims[a + 1])).mean[0]
    ma, mb = mode(a), mode(a + 1)
    res['off-off' if ma == mb == 'off' else 'identity-identity' if ma == mb else 'across a switch'].append(v)
m = {k: st.mean(v) for k, v in res.items()}
for k, v in res.items(): print('%-18s %3d pairs: mean %.3f median %.3f max %.3f' % (k, len(v), m[k], st.median(v), max(v)))
ok = abs(m['identity-identity'] - m['off-off']) <= 0.15 * m['off-off'] + 0.03 and abs(m['across a switch'] - m['off-off']) <= 0.15 * m['off-off'] + 0.03
print('IDENTITY CHECK %s (differences between modes within noise: %s)' % ('PASS' if ok else 'FAIL', 'yes' if ok else 'no'))
sys.exit(0 if ok else 1)
PYEOF
    rc=$?
    grep 'rebuilt P\*V' "$O/identity.log" | tail -1 || true
    exit $rc
fi

# ---- the sweep
plan_run "${SPOTS[@]}"
IFS='|' read -r _ lvl0 loc0 <<< "${SPOTS[0]}"
ENVV=(LARECOMP_WARP_ON_LOAD="$FIRST" LARECOMP_FREECAM_AT="$AT" LARECOMP_FREECAM_LOG=1 LARECOMP_SHOT_AT="$SHOTS")
[ -n "$WARPS" ] && ENVV+=(LARECOMP_WARP_AT="$WARPS")
if [ $MODE = dry ]; then
    echo "freecam_sweep (dry run): ${#SPOTS[@]} spots x $NSTEPS steps, last frame $LAST"
    echo "cd $REPO && env LARECOMP_NO_LAUNCHER=1 LARECOMP_BLUR_REMOVAL=1 LARECOMP_INPUT=<title input> LARECOMP_WINDOW_SHOT=$LAST LARECOMP_EXIT_AFTER_SHOT=1 \\"
    printf '    %s \\\n' "${ENVV[@]}"
    echo "    ./build/lasrecomp extracted"
    exit 0
fi
rm -rf "$O" && mkdir -p "$O/shots"
echo "freecam_sweep: ${#SPOTS[@]} spots x $NSTEPS steps = $((${#SPOTS[@]} * NSTEPS)) screenshots; session up to frame $LAST (log: $O/game.log)"
run_game "$O/game.log" $LAST "${ENVV[@]}"
rc=$?
grep '\[freecam\]' "$O/game.log" > "$O/freecam.log" || true
grep 'SUSPECT' "$O/freecam.log" | sed 's/^\[freecam\] //' | sort -u > "$O/suspects.txt" || true
grep -A 14 'floats 48-79' "$O/freecam.log" > "$O/floats48.txt" || true
if grep -qE '\*\*\* |FATAL' "$O/game.log"; then echo "freecam_sweep: CRASH: $(grep -m1 -E '\*\*\* |FATAL' "$O/game.log") (see $O/game.log)"; fi
[ $rc -ne 0 ] && echo "freecam_sweep: the session did not end by itself (exit $rc)"

# copy the screenshots under their names
i=0; missing=0
for s in "${SPOTS[@]}"; do
    IFS='|' read -r name lvl loc <<< "$s"
    base=$((START + i * STEP))
    for j in "${!STEPS[@]}"; do
        IFS='|' read -r label _ _ <<< "${STEPS[$j]}"
        f=$((base + SETTLE + j * GAP + SHOTOFF))
        src=$(printf 'build/frames/frame_%05d.png' $f)
        dst=$(printf '%s/shots/%02d_%s_%s.png' "$O" $i "$(slug "$name")" "$label")
        if [ -f "$src" ]; then cp "$src" "$dst"; else missing=$((missing + 1)); fi
    done
    i=$((i + 1))
done
[ $missing -gt 0 ] && echo "freecam_sweep: $missing screenshot(s) missing (the run ended early, or a warp was not taken: grep 'warp' $O/game.log)"
for s in "${SPOTS[@]}"; do IFS='|' read -r name lvl loc <<< "$s"; grep -q "\[warp\] jumping to $lvl / $loc" "$O/game.log" || echo "freecam_sweep: warp to $lvl / $loc not taken"; done

# contact sheets (tools/montage.sh is macOS-only: sips): labelled, with each shot's void fraction (black pixels)
"$PY" -I - "$O" <<'PYEOF'
import glob, os, sys
from PIL import Image, ImageDraw
O = sys.argv[1]
shots = sorted(glob.glob(O + '/shots/*.png'))
w, h = 400, 225
def tile(path):
    im = Image.open(path).convert('RGB')
    small = im.resize((w, h))
    px = small.convert('L').tobytes()
    void = sum(1 for p in px if p < 6) / len(px)
    d = ImageDraw.Draw(small)
    label = os.path.basename(path)[:-4].split('_', 1)[1].replace('_', ' ')
    d.rectangle((0, 0, 8 + 6 * len(label), 14), fill=(0, 0, 0))
    d.text((4, 1), label, fill=(255, 230, 80))
    tag = 'void %d%%' % round(100 * void)
    d.rectangle((w - 8 - 6 * len(tag), h - 14, w, h), fill=(0, 0, 0))
    d.text((w - 4 - 6 * len(tag), h - 13), tag, fill=(120, 255, 120))
    return small, void
import re
pat = re.compile(r'^(\d+)_(.*)_(game|identity|o-?\d+_p\d+_d\d+_(?:stock|wide))\.png$')
spots = {}
for p in shots:
    m = pat.match(os.path.basename(p))
    if m: spots.setdefault((m.group(1), m.group(2)), []).append((m.group(3), p))
PICK = ('game', 'identity', 'o45_p60_d50_stock', 'o45_p60_d50_wide', 'o90_p35_d50_wide', 'o180_p60_d50_wide',
        'o0_p60_d30_wide', 'o90_p60_d30_wide')  # (the overview sheet's tiles; the quick sweep has the d30 ones)
rows, report = [], []
for (idx, name), ps in sorted(spots.items()):
    ts = [(lab, tile(p)) for lab, p in ps]
    cols = 4
    r = (len(ts) + cols - 1) // cols
    sheet = Image.new('RGB', (w * cols, h * r))
    for i, (_, (t, _)) in enumerate(ts): sheet.paste(t, ((i % cols) * w, (i // cols) * h))
    sheet.save('%s/sheet_%s_%s.png' % (O, idx, name))
    for (lab, (t, v)), (_, p) in zip(ts, ps): report.append('%-64s void %5.1f%%' % (os.path.basename(p), 100 * v))
    pick = [t for lab, (t, _) in ts if lab in PICK][:6]
    if pick: rows.append(pick)
if rows:
    cols = max(len(r) for r in rows)
    m = Image.new('RGB', (w * cols, h * len(rows)))
    for y, r in enumerate(rows):
        for x, t in enumerate(r): m.paste(t, (x * w, y * h))
    m.save(O + '/sheet.png')
open(O + '/report.txt', 'w').write('\n'.join(report) + '\n')
print('freecam_sweep: %d shots; %s/sheet.png (overview), sheet_<n>_<spot>.png, void fractions in %s/report.txt' % (len(shots), O, O))
PYEOF
echo "freecam_sweep: suspects: $(grep -c 'matrix' "$O/suspects.txt" 2>/dev/null) matrix copies, $(grep -c 'other camera' "$O/suspects.txt" 2>/dev/null) other cameras, $(grep -c 'screen-space' "$O/suspects.txt" 2>/dev/null) screen-space draws ($O/suspects.txt)"
exit $rc
