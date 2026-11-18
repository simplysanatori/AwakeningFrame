#!/bin/bash
# Runs one or more app folders one after the other on the headset, without SteamVR, under the mock OpenXR runtime
# (bench/libxrmock.so of the kit: an Arm64 build of tools/xrmock), at 120 Hz, at four spots of the game, and writes one results
# file so two builds (the released 0.1.1 and a new one) can be compared in one sitting.
#   frame-bench.sh [--owner-ok] [--plan] [--dwell S] [--settle S] [--out FILE] [--hz N] APP_DIR [APP_DIR ...]
#   --owner-ok   run while ~/.cache/headset-in-use exists (the owner has said so); without it the bench refuses then
#   --plan       print what would run and stop
#   --dwell S    seconds in each spot (default 15)        --settle S   seconds at the start of a spot not counted (default 6)
#   --out FILE   the results (default <kit>/bench-results-<time>.md; the game's logs go to FILE.d/)
#   --hz N       the mock's display rate (default 120; the game then runs at 60 fps)
# The spots: Mabe Village, the Mysterious Forest (Field_06C), Marin and Tarin's house, Tail Cave room 04B. Each program runs in
# a scratch data folder (~/.local/share/lasrecomp-bench): extracted/ linked from the Dev data folder (or the released one), a
# fresh copy of the Dev save, the Dev larecomp.ini and vr_rig.ini, and the LARECOMP_* lines of the kit's session.env (not the
# log ones: the bench turns LARECOMP_LOG_PACE on itself); shader caches start empty for each program (--cache DIR shares one).
# One program at a time under a lock, a time limit for each, stopped and gone before the next; nothing is left running.
# It writes only in the scratch folder and next to the results; never in the Dev or the released data folders or app folders.
# The table: GPU ms and render CPU ms and game CPU ms per game frame, frames per second, the slowest 5 percent of frames
# (p95 of the period), peak memory (RSS) of the whole run. Two app folders end in about 5 minutes.
# Environment: AFDEV_DATA, AF_REL_DATA (data folders), AF_HEADSET_FLAG, AF_BENCH_LOCK, AFDEV_BENCH_ROOT (the scratch folder);
# for a desktop test under qemu: FRAME_BENCH_PREFIX (a command prefix for the program), FRAME_BENCH_LOADER (an OpenXR loader),
# FRAME_BENCH_ENVFILE (a file to source first, the Turnip shim's), FRAME_BENCH_ALLOW_RUNNING=1 (other lasrecomp processes are not ours).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OWNER=0 PLAN=0 DWELL=15 SETTLE=6 OUT= HZ=120 CACHE= APPS=()
while [ $# -gt 0 ]; do
    case $1 in
        --owner-ok) OWNER=1 ;;
        --plan) PLAN=1 ;;
        --dwell) DWELL=${2:?}; shift ;;
        --settle) SETTLE=${2:?}; shift ;;
        --out) OUT=${2:?}; shift ;;
        --hz) HZ=${2:?}; shift ;;
        --cache) CACHE=${2:?}; shift ;;
        -h | --help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        -*) echo "frame-bench: unknown argument $1 (--help)" >&2; exit 2 ;;
        *) APPS+=("$1") ;;
    esac
    shift
done
[ ${#APPS[@]} -gt 0 ] || { echo "usage: frame-bench.sh [--owner-ok] APP_DIR [APP_DIR ...]" >&2; exit 2; }
for a in "${APPS[@]}"; do
    [ -x "$a/lasrecomp" ] && [ -f "$a/lasgame.so" ] || { echo "frame-bench: $a has no lasrecomp and lasgame.so" >&2; exit 2; }
done
case "$DWELL$SETTLE$HZ" in *[!0-9]*) echo "frame-bench: --dwell, --settle and --hz are whole numbers" >&2; exit 2 ;; esac
HEADSET=${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}
LOCK=${AF_BENCH_LOCK:-$HOME/.cache/af-bench-lock}
if [ -e "$HEADSET" ] && [ $OWNER = 0 ] && [ $PLAN = 0 ]; then
    echo "frame-bench: $HEADSET exists (the headset is in use): not running (--owner-ok if the owner says so)" >&2; exit 75
fi
BENCH=$HERE/bench
[ -f "$BENCH/libxrmock.so" ] || BENCH=$HERE   # (run from the repository: a mock built next to it)
MOCK=$BENCH/libxrmock.so
[ -f "$MOCK" ] || { echo "frame-bench: no libxrmock.so in $HERE/bench (the kit's: make-dev-app.sh builds it)" >&2; exit 1; }
DEVDATA=${AFDEV_DATA:-$HOME/.local/share/lasrecomp-dev}
RELDATA=${AF_REL_DATA:-$HOME/.local/share/lasrecomp}
GAMEFILES=
for d in "$DEVDATA" "$RELDATA"; do [ -e "$d/extracted/exefs/main" ] && { GAMEFILES=$(readlink -f "$d/extracted"); break; }; done
[ -n "$GAMEFILES" ] || { echo "frame-bench: no game files in $DEVDATA/extracted or $RELDATA/extracted" >&2; exit 1; }
SAVEFROM=
for d in "$DEVDATA" "$RELDATA"; do [ -d "$d/save" ] && { SAVEFROM=$d; break; }; done
[ -n "$SAVEFROM" ] || { echo "frame-bench: no save to copy in $DEVDATA/save or $RELDATA/save" >&2; exit 1; }
SCRATCH=${AFDEV_BENCH_ROOT:-$HOME/.local/share/lasrecomp-bench}
TS=$(date +%Y%m%d-%H%M%S)
[ -n "$OUT" ] || OUT=$HERE/bench-results-$TS.md
KITENV=$HERE/session.env; [ -f "$KITENV" ] || KITENV=
if [ $PLAN = 0 ]; then
    [ -n "${FRAME_BENCH_ALLOW_RUNNING:-}" ] || [ -z "$(pgrep -x lasrecomp 2> /dev/null)" ] || { echo "frame-bench: a lasrecomp is running: not starting" >&2; exit 75; }
    mkdir -p "$(dirname "$LOCK")"
    ( set -o noclobber; echo "frame-bench $$" > "$LOCK" ) 2> /dev/null || { echo "frame-bench: $LOCK is held by: $(cat "$LOCK")" >&2; exit 75; }
    trap 'rm -f "$LOCK"' EXIT
    mkdir -p "$SCRATCH" "$OUT.d"
fi
export FB_APPS=$(printf '%s\n' "${APPS[@]}") FB_DWELL=$DWELL FB_SETTLE=$SETTLE FB_HZ=$HZ FB_OUT=$OUT FB_SCRATCH=$SCRATCH FB_MOCK=$MOCK \
       FB_GAMEFILES=$GAMEFILES FB_SAVEFROM=$SAVEFROM FB_DEVDATA=$DEVDATA FB_KITENV=$KITENV FB_CACHE=$CACHE FB_PLAN=$PLAN FB_TS=$TS
python3 -I - << 'PY'
import os, re, shutil, signal, subprocess, sys, time, json

E = os.environ
apps = E["FB_APPS"].split("\n")
dwell, settle, hz = int(E["FB_DWELL"]), int(E["FB_SETTLE"]), int(E["FB_HZ"])
out, scratch, plan = E["FB_OUT"], E["FB_SCRATCH"], E["FB_PLAN"] == "1"
fps = 60 if hz >= 100 else 30                  # the game's frame rate at that display rate
step = dwell * fps                             # game frames in a spot
START = 900                                    # the save is loaded by frame ~700
SPOTS = [("Mabe Village", "Field", "Field_11C"), ("Mysterious Forest", "Field", "Field_06C"),
         ("Marin and Tarin house", "MarinTarinHouse", "MarinTarinHouse_01A"), ("Tail Cave 04B", "Lv01TailCave", "Lv01TailCave_04B")]
LOGIN = "".join("%d:L:12,%d:R:12,%d:A:6," % (f, f, f + 30) for f in range(300, 781, 60))
warps = ",".join("%d:%s:%s" % (START + i * step, lev, loc) for i, (n, lev, loc) in enumerate(SPOTS))
last = START + len(SPOTS) * step
limit = 240 + len(SPOTS) * dwell * 3           # seconds, hard stop for one program

# the owner's LARECOMP_* settings from the kit's session.env (no log switches, nothing with a shell expansion)
session = {}
if E["FB_KITENV"] and os.path.isfile(E["FB_KITENV"]):
    for l in open(E["FB_KITENV"], errors="replace"):
        m = re.match(r"\s*(LARECOMP_[A-Z0-9_]+)=([^\s#$]*)", l)
        if m and "LOG" not in m.group(1):
            session[m.group(1)] = m.group(2)

def prefix():
    return E.get("FRAME_BENCH_PREFIX", "").split()

def child_env(root, label, app):
    env = dict(os.environ)
    for k in [k for k in env if k.startswith(("LARECOMP_", "MOCK_XR_"))]:
        del env[k]
    env.update(session)
    rj = os.path.join(root, "runtime.json")
    open(rj, "w").write('{"file_format_version": "1.0.0", "runtime": {"api_version": "1.0", "library_path": "%s", "name": "xrmock"}}\n' % E["FB_MOCK"])
    env.update(XR_RUNTIME_JSON=rj, ENABLE_GAMESCOPE_WSI="0", SDL_VIDEODRIVER="offscreen", LARECOMP_XR="1", LARECOMP_XR_HZ=str(hz),
               LARECOMP_DATA_ROOT=root, LARECOMP_NO_LAUNCHER="1", LARECOMP_NO_AUDIO="1", LARECOMP_NO_UPDATE_CHECK="1",
               LARECOMP_SETTINGS=os.path.join(root, "larecomp.ini"), LARECOMP_SAVE_DIR=os.path.join(root, "save"), LARECOMP_CHEATS="hearts",
               LARECOMP_INPUT=LOGIN.rstrip(","), LARECOMP_WARP_AT=warps, LARECOMP_LOG_PACE="1", LARECOMP_FPS_INTERVAL="2",
               LARECOMP_WINDOW_SHOT=str(last), LARECOMP_SHOT_PATH=os.path.join(root, "window_last.png"), LARECOMP_EXIT_AFTER_SHOT="1",
               MOCK_XR_HZ=str(hz), MOCK_XR_SECONDS="0", MOCK_XR_EYE_SIZE=E.get("FRAME_BENCH_EYE", "1680x1760"), MOCK_XR_POSE="sway")
    if E.get("FRAME_BENCH_LOADER"):
        env["LARECOMP_XR_LOADER"] = E["FRAME_BENCH_LOADER"]
    if E.get("FRAME_BENCH_ENVFILE"):   # (a desktop test: the Turnip shim's variables)
        dump = subprocess.run(["bash", "-c", 'set -a; . "$1" > /dev/null 2>&1; env -0', "x", E["FRAME_BENCH_ENVFILE"]], capture_output=True).stdout
        for kv in dump.split(b"\0"):
            if b"=" in kv:
                k, v = kv.decode(errors="replace").split("=", 1)
                if k not in env or k.startswith(("VK_", "MESA_", "TU_", "LD_")) or k == "QEMU_LD_PREFIX":
                    env[k] = v
    return env

def prepare(root, label):
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    os.symlink(E["FB_GAMEFILES"], os.path.join(root, "extracted"))
    shutil.copytree(os.path.join(E["FB_SAVEFROM"], "save"), os.path.join(root, "save"))
    for f in ("larecomp.ini", "vr_rig.ini"):
        for d in (E["FB_DEVDATA"], E["FB_SAVEFROM"]):
            if os.path.isfile(os.path.join(d, f)):
                shutil.copyfile(os.path.join(d, f), os.path.join(root, f)); break
    if E["FB_CACHE"]:
        os.symlink(os.path.abspath(E["FB_CACHE"]), os.path.join(root, "cache"))

class Timeout(Exception): pass
def on_alarm(sig, frm): raise Timeout()

def run_one(app, label, logpath):
    root = os.path.join(scratch, "run-" + label)
    prepare(root, label)
    cmd = prefix() + [os.path.join(app, "lasrecomp"), "extracted"]
    env = child_env(root, label, app)
    t0 = time.time()
    log = open(logpath + ".stdout", "wb")
    p = subprocess.Popen(cmd, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    status, rss, why = None, None, "ended"
    signal.signal(signal.SIGALRM, on_alarm)
    signal.alarm(limit)
    def stop(sig, frm): raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, stop)
    try:
        _, st, ru = os.wait4(p.pid, 0)
        status = os.waitstatus_to_exitcode(st); rss = ru.ru_maxrss
    except (Timeout, KeyboardInterrupt) as e:
        why = "time limit (%d s)" % limit if isinstance(e, Timeout) else "interrupted"
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try: os.killpg(p.pid, sig)
            except ProcessLookupError: break
            try:
                signal.alarm(10)
                _, st, ru = os.wait4(p.pid, 0)
                status = os.waitstatus_to_exitcode(st); rss = ru.ru_maxrss
                break
            except Timeout:
                continue
    finally:
        signal.alarm(0)
        try: os.killpg(p.pid, signal.SIGKILL)   # (nothing of it stays)
        except ProcessLookupError: pass
        log.close()
    # (the game writes its log in the data folder: kept next to the results)
    if os.path.isfile(os.path.join(root, "lasrecomp.log")):
        shutil.copyfile(os.path.join(root, "lasrecomp.log"), logpath)
    if not E["FB_CACHE"]:
        shutil.rmtree(os.path.join(root, "cache"), ignore_errors=True)
    open(logpath, "a").close()
    return status, rss, why, time.time() - t0

KV = re.compile(r"([a-z][a-z ]*?) (-?\d+(?:\.\d+)?)")
def mean(v): v = [x for x in v if x is not None]; return sum(v) / len(v) if v else None
def pct(v, p): v = sorted(v); return v[min(len(v) - 1, int(len(v) * p))] if v else None

def analyse(logpath):
    """Per spot: the [pace] lines after a [warp] line, the first `settle` seconds left out."""
    spots, cur = [], None
    for l in open(logpath, errors="replace"):
        l = l.rstrip("\n")
        if l.startswith("[warp] jumping to"):
            cur = {"name": l.split("to", 1)[1].strip(), "pace": [], "gpu": []}; spots.append(cur)
        elif cur is not None and l.startswith("[pace] period") and "(no gameplay)" not in l:
            cur["pace"].append({k.strip(): float(v) for k, v in KV.findall(l[len("[pace] "):])})
        elif cur is not None and l.startswith("[stereo] GPU per game frame"):
            m = re.search(r"both ([\d.]+) ms", l)
            if m: cur["gpu"].append(float(m.group(1)))
    rows = []
    for s in spots:
        p = s["pace"][settle * fps:]
        if not p: rows.append({"name": s["name"], "frames": 0}); continue
        per = [x["period"] for x in p if "period" in x]
        rows.append({"name": s["name"], "frames": len(p), "fps": 1000.0 / mean(per), "p95": pct(per, 0.95),
                     "gpu": mean([x.get("gpu") for x in p]), "gpu2": mean(s["gpu"]), "render": mean([x.get("render cpu") for x in p]),
                     "cpu": mean([x.get("cpu") for x in p])})
    return rows

if plan:
    print("frame-bench plan: %d program(s), %d spots of %d s (%d s settle) at %d Hz, game at %d fps, mock %s" % (len(apps), len(SPOTS), dwell, settle, hz, fps, E["FB_MOCK"]))
    for a in apps: print("  program: " + a)
    for i, (n, lev, loc) in enumerate(SPOTS): print("  spot %d: %-24s %s / %s at frame %d" % (i, n, lev, loc, START + i * step))
    print("  game files: %s; save from %s; scratch %s; time limit %d s each" % (E["FB_GAMEFILES"], E["FB_SAVEFROM"], scratch, limit))
    print("  the owner's LARECOMP_* lines applied: %s" % (", ".join("%s=%s" % kv for kv in session.items()) or "none"))
    sys.exit(0)

f = lambda x, nd=1: "-" if x is None else "%.*f" % (nd, x)
lines = ["# Frame bench %s" % E["FB_TS"], "", "Mock OpenXR runtime, %d Hz, game at %d fps, %d s a spot (%d s left out at the start), scratch data folder, no SteamVR." % (hz, fps, dwell, settle),
         "Settings applied from the kit's session.env: %s." % (", ".join("%s=%s" % kv for kv in session.items()) or "none"), ""]
summary = []
base = None
for idx, app in enumerate(apps):
    label = "%d-%s" % (idx + 1, re.sub(r"[^A-Za-z0-9._-]", "_", os.path.basename(app.rstrip("/"))))
    logpath = os.path.join(out + ".d", label + ".log")
    ver = open(os.path.join(app, "VERSION")).readline().strip() if os.path.isfile(os.path.join(app, "VERSION")) else "no VERSION"
    print("frame-bench: %s (%s) ..." % (app, ver), flush=True)
    status, rss, why, secs = run_one(app, label, logpath)
    rows = analyse(logpath)
    mem = "%.0f MB peak RSS" % (rss / 1024.0) if rss else "peak RSS unknown"
    lines += ["## %s: %s" % (label, ver), "", "Exit %s (%s), %.0f s, %s. Log: %s" % (status, why, secs, mem, logpath), "",
              "| Spot | Frames | FPS | GPU ms | GPU ms (both eyes) | Render CPU ms | Game CPU ms | p95 period ms |", "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        lines.append("| %s | %d | %s | %s | %s | %s | %s | %s |" % (r["name"], r["frames"], f(r.get("fps")), f(r.get("gpu"), 2), f(r.get("gpu2"), 2), f(r.get("render"), 2), f(r.get("cpu"), 2), f(r.get("p95"), 2)))
    if not rows: lines.append("| no spot reached (see the log) | | | | | | | |")
    lines.append("")
    summary.append((label, ver, rows, status, rss))
    if base is None: base = rows
if len(summary) > 1 and base:
    lines += ["## Against the first program", "", "| Spot | " + " | ".join("%s GPU ms / FPS" % s[0] for s in summary[1:]) + " |", "|---|" + "---|" * (len(summary) - 1)]
    for i, b in enumerate(base):
        cells = []
        for s in summary[1:]:
            r = s[2][i] if i < len(s[2]) else {}
            cells.append("%s (%+.2f) / %s (%+.1f)" % (f(r.get("gpu"), 2), (r.get("gpu") or 0) - (b.get("gpu") or 0), f(r.get("fps")), (r.get("fps") or 0) - (b.get("fps") or 0)) if r.get("gpu") is not None and b.get("gpu") is not None else "-")
        lines.append("| %s | %s |" % (b["name"], " | ".join(cells)))
    lines.append("")
open(out, "w").write("\n".join(lines) + "\n")
print("frame-bench: results in %s" % out)
left = subprocess.run(["pgrep", "-x", "lasrecomp"], capture_output=True, text=True).stdout.split()
print("frame-bench: lasrecomp still running: %s" % (", ".join(left) if left else "none"))
sys.exit(0 if all(s[3] == 0 for s in summary) else 1)
PY
