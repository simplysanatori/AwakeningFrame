#!/bin/bash
# Streaming lane, VR path (docs/af/STREAM.md): one xrmock session (90 Hz, the head 45 degrees down at the table, as
# docs/af/STEREO.md's mock run) at Mabe Village, then the Mysterious Forest. Per spot: draws per frame, GPU ms
# (LARECOMP_LOG_PACE), resident memory, and both eye images (LARECOMP_STEREO_DUMP_AT) with their void % (black pixels).
#   tools/stream_vr.sh <out dir> [VAR=value ...]       e.g. LARECOMP_VR_CULL_EXTENT=52,42
# Needs build/xrmock (tools/xrmock/build.sh). Runs through tools/stream_run.sh (waits for the headset / bench lock / runs).
set -uo pipefail
cd "$(dirname "$0")/.."
O=$1; shift
mkdir -p "$O/eyes" && O=$(cd "$O" && pwd) && rm -f "$O/eyes"/*.png
SPOTS=(${STREAM_SPOTS:-mabe|Field_11C forest|Field_06C})
START=7000 STEP=1500 PLAN= WARPS= DUMP= FIRST=
for i in "${!SPOTS[@]}"; do
    name=${SPOTS[$i]%%|*} loc=${SPOTS[$i]#*|} base=$((START + i * STEP))
    [ $i = 0 ] && FIRST="Field:$loc" || WARPS+="$base:Field:$loc,"
    DUMP+="$((base + 1200)),"; PLAN+="$name $((base + 600)) $((base + 1400)) $((base + 1200))"$'\n'
done
LAST=$((START + ${#SPOTS[@]} * STEP))
printf '%s' "$PLAN" > "$O/plan.txt"
( while ! pgrep -x lasrecomp >/dev/null; do sleep 1; done; t0=$(date +%s)
  while p=$(pgrep -x lasrecomp | head -1) && [ -n "$p" ]; do
      echo "$(( $(date +%s) - t0 )) $(awk '/VmRSS/{print int($2/1024)}' /proc/$p/status 2>/dev/null)"; sleep 2
  done > "$O/rss.txt" ) &
ENVV=(XR_RUNTIME_JSON="$PWD/build/xrmock/runtime.json" LARECOMP_XR=1 MOCK_XR_HZ=90 MOCK_XR_EYE_SIZE=1680x1760 MOCK_XR_SECONDS=0
      MOCK_XR_POSE=static:0,1.6,0,0,-45 LARECOMP_WARP_ON_LOAD="$FIRST" LARECOMP_STEREO_DUMP_AT="${DUMP%,}"
      LARECOMP_STEREO_DUMP_DIR="$O/eyes" LARECOMP_LOG_PACE=1 LARECOMP_LOG_CULL=1 LARECOMP_BLUR_REMOVAL=1)
[ -n "$WARPS" ] && ENVV+=(LARECOMP_WARP_AT="${WARPS%,}")
STREAM_TIMEOUT=1800 tools/stream_run.sh "$O" "$LAST" "${ENVV[@]}" "$@"
wait
PY=${FREECAM_PYTHON:-$HOME/Games/AwakeningFrame/venv-tools/bin/python}
"$PY" -I - "$O" <<'PYEOF'
import os, re, sys
from PIL import Image
O = sys.argv[1]
log = open(O + '/game.log', errors='replace').read().splitlines()
iv, pend = [], []
for l in log:
    if l.startswith('[pace] period'):
        m = re.search(r'gpu ([0-9.]+)', l)
        if m: pend.append(float(m.group(1)))
    m = re.match(r'\[vk\] t=([0-9.]+) frame (\d+) .*?\((\d+) draws', l)
    if m: iv.append((float(m.group(1)), int(m.group(2)), int(m.group(3)), pend)); pend = []
rss = [tuple(map(int, l.split())) for l in open(O + '/rss.txt') if len(l.split()) == 2]
out = ['%-8s %7s %7s %7s %6s %6s' % ('spot', 'draws', 'gpu_ms', 'rss_MB', 'void0', 'void1')]
for line in open(O + '/plan.txt'):
    name, f0, f1, fd = line.split(); f0, f1 = int(f0), int(f1)
    sel = [x for x in iv if f0 + 120 <= x[1] <= f1]
    n = sum(1 for x in sel)
    draws = sum(x[2] for x in sel) / (120.0 * n) if n else float('nan')
    g = [v for x in sel for v in x[3]]
    ts = [x[0] for x in sel]
    r = [m for t, m in rss if ts and min(ts) - 2 <= t <= max(ts) + 2]
    voids = []
    for e in (0, 1):
        p = '%s/eyes/stereo_%05d_eye%d.png' % (O, int(fd), e)
        if os.path.exists(p):
            px = Image.open(p).convert('L').resize((420, 440)).tobytes()
            voids.append('%5.1f' % (100.0 * sum(1 for v in px if v < 6) / len(px)))
        else: voids.append('    -')
    out.append('%-8s %7.0f %7.2f %7s %6s %6s' % (name, draws, sum(g) / len(g) if g else float('nan'), max(r) if r else '-', *voids))
cull = [l for l in log if l.startswith('[vrcull]')]
out += cull[-4:]
open(O + '/summary.txt', 'w').write('\n'.join(out) + '\n'); print('\n'.join(out))
PYEOF
