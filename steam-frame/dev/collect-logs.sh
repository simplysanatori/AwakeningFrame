#!/bin/bash
# Brings the Dev entry's logs from the Steam Frame to this desktop, in one ssh connection that only reads there.
#   collect-logs.sh --owner-ok [--sessions N] [--out DIR]
#   --owner-ok     required, as for install-to-frame.sh (the owner's OK to connect to the headset)
#   --sessions N   how many of the latest sessions (default 3)
#   --out DIR      where (default ~/Games/AwakeningFrame-dev/logs/<date>, <date> = now)
# Per session (the launch time in the names): run-<time>.log (the launcher's and the game's output), lasrecomp-<time>.log
# (the game's log), xr_timing-<time>.log, vrtiming-<time>.log (SteamVR's compositor), pose-<time>.log, session-<time>.env
# (the session.env as it was); also the Dev session.env, larecomp.ini, vr_rig.ini, and the data folder's current lasrecomp.log
# and xr_timing.log (data-lasrecomp.log, data-xr_timing.log). Then one line per session: start, length, version, exit status,
# which A/B switches were set. Nothing on the headset is changed or started.
# Environment: FRAME_SSH (default "ssh frame"), FRAME_HOME (the headset's home folder; default what $HOME is there), AFDEV_DATA.
set -euo pipefail
OK=0 N=3 OUT=
while [ $# -gt 0 ]; do
    case $1 in
        --owner-ok) OK=1 ;;
        --sessions) N=${2:?}; shift ;;
        --out) OUT=${2:?}; shift ;;
        -h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "collect-logs: unknown argument $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done
[ "$OK" = 1 ] || { echo "collect-logs: needs --owner-ok (the owner's OK to connect to the headset); nothing done" >&2; exit 2; }
case $N in '' | *[!0-9]*) echo "collect-logs: --sessions wants a number" >&2; exit 2 ;; esac
[ -n "$OUT" ] || OUT=$HOME/Games/AwakeningFrame-dev/logs/$(date +%Y%m%d-%H%M%S)
read -ra SSH <<< "${FRAME_SSH:-ssh frame}"
if [ -n "${FRAME_HOME:-}" ]; then RHOME=$(printf '%q' "$FRAME_HOME"); else RHOME='"$HOME"'; fi
KIT="$RHOME/Games/AwakeningFrame-dev"
DATA=${AFDEV_DATA:+$(printf '%q' "$AFDEV_DATA")}
[ -n "$DATA" ] || DATA="$RHOME/.local/share/lasrecomp-dev"
# one tar stream of the files that exist: the latest N sessions' logs, the settings, the data folder's current game logs
REMOTE="cd $KIT/logs 2> /dev/null || exit 4
files=
for r in \$(ls -1t run-*.log 2> /dev/null | head -n $N); do
  s=\${r#run-}; s=\${s%.log}
  for p in run-\$s.log lasrecomp-\$s.log xr_timing-\$s.log vrtiming-\$s.log pose-\$s.log session-\$s.env; do [ -f \"\$p\" ] && files=\"\$files logs/\$p\"; done
done
cd $KIT || exit 4
for p in session.env; do [ -f \"\$p\" ] && files=\"\$files \$p\"; done
tar -cf - \$files -C $DATA \$(for p in larecomp.ini vr_rig.ini; do [ -f \"$DATA/\$p\" ] && echo \$p; done) \
  --transform 's,^lasrecomp.log\$,data-lasrecomp.log,;s,^xr_timing.log\$,data-xr_timing.log,' \$(for p in lasrecomp.log xr_timing.log; do [ -f \"$DATA/\$p\" ] && echo \$p; done)"
mkdir -p "$OUT"
echo "collecting the last $N sessions from ${SSH[*]} into $OUT"
"${SSH[@]}" "$REMOTE" | tar -C "$OUT" -xf - || { echo "collect-logs: nothing collected (no $KIT/logs on the headset, or ssh failed)" >&2; exit 1; }
python3 -I - "$OUT" << 'PY'
import glob, os, re, sys
out = sys.argv[1]
SW = ("AF_FDM", "AF_THROTTLE", "LARECOMP_VR_CULL_TABLE", "LARECOMP_VR_CULL", "LARECOMP_VR_SCENE", "LARECOMP_VR_HEADROOM")
runs = sorted(glob.glob(os.path.join(out, "logs", "run-*.log")))
if not runs:
    print("no sessions found")
for r in runs:
    stamp = os.path.basename(r)[4:-4]
    text = open(r, errors="replace").read().splitlines()
    times = [m.group(1) for m in (re.match(r"\[awakeningframe (\d\d:\d\d:\d\d)\]", l) for l in text) if m]
    def secs(t): h, m, s = map(int, t.split(":")); return h * 3600 + m * 60 + s
    length = "?"
    if len(times) > 1:
        d = secs(times[-1]) - secs(times[0])
        length = "%dm%02ds" % (((d + 86400) % 86400) // 60, ((d + 86400) % 86400) % 60)
    ver = next((re.sub(r"^\[awakeningframe [\d:]+\] version ", "", l) for l in text if "] version " in l[:40]), "no version line")
    rc = next((l.split("status ")[-1] for l in reversed(text) if "the game exited with status" in l), "no exit line (still running or killed)")
    envp = os.path.join(out, "logs", "session-%s.env" % stamp)
    sw = []
    if os.path.isfile(envp):
        for l in open(envp, errors="replace"):
            m = re.match(r"\s*(%s)=(\S*)" % "|".join(SW), l)
            if m: sw.append(m.group(1) + "=" + m.group(2))
    fdm = next((l.split("] ", 1)[1] for l in text if "] AF_FDM=" in l[:40] and "VK_INSTANCE_LAYERS" not in l), None)
    have = [p for p in ("lasrecomp", "xr_timing", "vrtiming", "pose") if os.path.isfile(os.path.join(out, "logs", "%s-%s.log" % (p, stamp)))]
    print("%s  %s  %s  exit %s  switches: %s  logs: %s" % (stamp, length, ver, rc, " ".join(sw) or "none", " ".join(have) or "run log only"))
PY
echo "files are in $OUT"
