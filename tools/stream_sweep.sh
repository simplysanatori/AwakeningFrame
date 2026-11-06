#!/bin/bash
# Streaming measurement (docs/af/STREAM.md): one headless session warps to Mabe Village, the Mysterious Forest and
# Field_09B and at each takes the stock view and free-camera orbits that look past the loaded world (yaw 180 / 90,
# distance 50, widened culling), 240 frames per step. Per step: void % (black pixels of the shot), draws per frame,
# GPU ms (LARECOMP_LOG_PACE), resident memory (MB) and the game's Vulkan memory in use.
#   tools/stream_sweep.sh <out dir> [VAR=value ...]     (e.g. LARECOMP_STREAM_RADIUS=5; research output stays outside git)
# STREAM_WIDE=<aspect>:<fovy scale> (default 3.2:1.5, as tools/freecam_sweep.sh), STREAM_SPOTS="name|locator ..." to
# change the spots. Runs through tools/stream_run.sh (waits for the headset / bench lock / other runs).
set -uo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
O=$1; shift
WIDE=${STREAM_WIDE:-3.2:1.5}
SPOTS=(${STREAM_SPOTS:-mabe|Field_11C forest|Field_06C field09b|Field_09B})
STEPS=("game|off|stock" "o180p60|orbit:180:60:50|$WIDE" "o180p35|orbit:180:35:50|$WIDE" "o90p60|orbit:90:60:50|$WIDE" "o0p35|orbit:0:35:50|$WIDE")
START=7000 SETTLE=600 GAP=240
mkdir -p "$O" && O=$(cd "$O" && pwd)
AT= SHOTS= WARPS= FIRST= i=0 PLAN=
for s in "${SPOTS[@]}"; do
    name=${s%%|*} loc=${s#*|}
    base=$((START + i * (SETTLE + (${#STEPS[@]} + 1) * GAP)))
    [ $i = 0 ] && FIRST="Field:$loc" || WARPS+="$base:Field:$loc,"
    for j in "${!STEPS[@]}"; do
        IFS='|' read -r lab mode cull <<< "${STEPS[$j]}"
        f=$((base + SETTLE + j * GAP))
        AT+="$f:$mode/$cull,"; SHOTS+="$((f + 200)),"; PLAN+="$name $lab $f $((f + GAP))"$'\n'
    done
    AT+="$((base + SETTLE + ${#STEPS[@]} * GAP)):off/stock,"
    LAST=$((base + SETTLE + ${#STEPS[@]} * GAP + 60))
    i=$((i + 1))
done
printf '%s' "$PLAN" > "$O/plan.txt"
ENVV=(LARECOMP_WARP_ON_LOAD="$FIRST" LARECOMP_FREECAM=off LARECOMP_FREECAM_AT="${AT%,}" LARECOMP_SHOT_AT="${SHOTS%,}"
      LARECOMP_FREECAM_FAR=120 LARECOMP_BLUR_REMOVAL=1 LARECOMP_LOG_PACE=1)
[ -n "$WARPS" ] && ENVV+=(LARECOMP_WARP_AT="${WARPS%,}")
rm -rf build/frames
( # resident memory every 2 s while the game runs
  while ! pgrep -x lasrecomp >/dev/null; do sleep 1; done; t0=$(date +%s)
  while p=$(pgrep -x lasrecomp | head -1) && [ -n "$p" ]; do
      echo "$(( $(date +%s) - t0 )) $(awk '/VmRSS/{print int($2/1024)}' /proc/$p/status 2>/dev/null)"; sleep 2
  done > "$O/rss.txt" ) &
STREAM_TIMEOUT=2400 tools/stream_run.sh "$O" "$LAST" "${ENVV[@]}" "$@"
wait
mkdir -p "$O/shots" && rm -f "$O/shots"/*.png
while read -r name lab f0 f1; do
    src=$(printf 'build/frames/frame_%05d.png' $((f0 + 200)))
    [ -f "$src" ] && cp "$src" "$O/shots/${name}_${lab}.png"
done < "$O/plan.txt"
PY=${FREECAM_PYTHON:-$HOME/Games/AwakeningFrame/venv-tools/bin/python}
"$PY" -I - "$O" <<'PYEOF'
import os, re, sys
from PIL import Image
O = sys.argv[1]
plan = [l.split() for l in open(O + '/plan.txt') if l.strip()]
log = open(O + '/game.log', errors='replace').read().splitlines()
# [vk] t=<s> frame <n> ... (<draws> draws: per 2 s); the [pace] lines between two of them are those frames'
iv, pend = [], []
for l in log:
    if l.startswith('[pace] period'):
        m = re.search(r'gpu ([0-9.]+)', l)
        if m: pend.append(float(m.group(1)))
    m = re.match(r'\[vk\] t=([0-9.]+) frame (\d+) .*?\((\d+) draws', l)
    if m:
        iv.append((float(m.group(1)), int(m.group(2)), int(m.group(3)), pend)); pend = []
mem = [(int(m.group(1))) for l in log for m in [re.search(r'\[vk\] memory \(\w+\).*in use (\d+) MB', l)] if m]
rss = [tuple(map(int, l.split())) for l in open(O + '/rss.txt') if len(l.split()) == 2]
out = ['%-9s %-8s %6s %7s %7s %7s' % ('spot', 'step', 'void%', 'draws', 'gpu_ms', 'rss_MB')]
for name, lab, f0, f1 in plan:
    f0, f1 = int(f0) + 60, int(f1)
    sel = [x for x in iv if f0 + 120 <= x[1] <= f1]
    draws = sum(x[2] for x in sel) / (120.0 * len(sel)) if sel else float('nan')
    g = [v for x in sel for v in x[3]]
    gpu = sum(g) / len(g) if g else float('nan')
    ts = [x[0] for x in sel]
    r = [m for t, m in rss if ts and min(ts) - 2 <= t <= max(ts) + 2]
    p = '%s/shots/%s_%s.png' % (O, name, lab)
    void = float('nan')
    if os.path.exists(p):
        px = Image.open(p).convert('L').resize((480, 270)).tobytes()
        void = 100.0 * sum(1 for v in px if v < 6) / len(px)
    out.append('%-9s %-8s %6.1f %7.0f %7.2f %7s' % (name, lab, void, draws, gpu, max(r) if r else '-'))
out.append('vulkan in use (MB, logged): %s' % ' '.join(map(str, mem)))
open(O + '/summary.txt', 'w').write('\n'.join(out) + '\n')
print('\n'.join(out))
PYEOF
