#!/bin/bash
# Steam shortcut target for AwakeningFrame (LAS Recomp as a VR diorama on the Steam Frame).
#   awakeningframe.sh [--flat] [--launcher] [lasrecomp arguments...]
#     --flat      no VR: the ordinary window, for debugging (also AF_FLAT=1). No SteamVR settings, no
#                 headset-in-use marker.
#     --launcher  show LAS Recomp's start screen instead of starting the game at once (also AF_LAUNCHER=1).
# Where things are:
#   program     the folder this script is in (~/Games/AwakeningFrame/app)
#   game data   ~/.local/share/lasrecomp (LARECOMP_DATA_ROOT): extracted game files, saves, settings, caches,
#               and the game's own logs (lasrecomp.log, xr_timing.log). Nothing of it is copied elsewhere.
#   logs        <program folder>/../logs: run-<time>.log (this script and the game's output), and per
#               session xr_timing-<time>.log, lasrecomp-<time>.log, vrtiming-<time>.log (SteamVR's compositor)
# Settings for one launch from the Steam library, which passes no environment of its own: VAR=value lines
# in <program folder>/../session.env (e.g. AF_REFRESH_HZ=90, LARECOMP_XR_EYE_SCALE=0.8).
# Variables: AF_REFRESH_HZ (72, 90 or 120; default: the VR tab's Refresh Rate, else 90), AF_FPS=60 (a test: 60 fps at
# 90 Hz, frames alternating one and two refreshes; the VR tab's third Refresh Rate choice does the same), AF_VRTIMING=0 (no compositor log), AF_VRCMD,
# AF_HEADSET_FLAG, AF_LOGS, AF_KEEP (sessions of logs kept, default 12), LARECOMP_DATA_ROOT.
HERE=$(dirname "$(readlink -f "$0")")
ROOT=$(dirname "$HERE")
LOGS=${AF_LOGS:-$ROOT/logs}
mkdir -p "$LOGS" 2> /dev/null
STAMP=$(date +%Y%m%d-%H%M%S)
RUNLOG=$LOGS/run-$STAMP.log
exec > "$RUNLOG" 2>&1
log() { echo "[awakeningframe $(date +%T)] $*"; }

FLAT=${AF_FLAT:-0}
LAUNCHER=${AF_LAUNCHER:-0}
ARGS=()
for a in "$@"; do
    case "$a" in
        --flat) FLAT=1 ;;
        --launcher) LAUNCHER=1 ;;
        *) ARGS+=("$a") ;;
    esac
done

log "wrapper, session $STAMP, steam appid '${SteamAppId:-unset}', program $HERE"
if [ -f "$ROOT/session.env" ]; then
    log "session.env:"; cat "$ROOT/session.env"
    set -a; . "$ROOT/session.env"; set +a
    [ -n "${AF_FLAT:-}" ] && FLAT=$AF_FLAT
fi

export LARECOMP_DATA_ROOT=${LARECOMP_DATA_ROOT:-$HOME/.local/share/lasrecomp}
DATA=$LARECOMP_DATA_ROOT
cd "$HERE" || exit 1   # (no extracted/ here, so the data root is never taken for a source checkout)

# ---- what is there
EXE=$HERE/lasrecomp
[ -x "$EXE" ] || { log "$EXE is missing: run install.sh"; exit 66; }
if [ -f "$HERE/lasgame.so" ] && [ -f "$DATA/extracted/exefs/main" ]; then
    log "game library and game files present"
else
    [ -f "$HERE/lasgame.so" ] || log "no lasgame.so next to the program: the start screen will build the game code"
    [ -f "$DATA/extracted/exefs/main" ] || log "no game files in $DATA/extracted (steam-frame/prepare-game.sh)"
    LAUNCHER=1
fi
[ "$LAUNCHER" = 1 ] || ARGS=(--play "${ARGS[@]}")

# ---- VR or flat
# The refresh rate: AF_REFRESH_HZ (session.env) wins; else the VR tab's Refresh Rate (vr_refresh in larecomp.ini); else 90.
# The ini is the file the game reads: LARECOMP_SETTINGS, else larecomp.ini in the data root (where the game keeps it:
# LARECOMP_DATA_ROOT, set above), else the program folder's. AF_REFRESH_FROM tells the game which, so its VR tab greys
# the row out when session.env sets it. AF_FPS=60 (session.env) is the 60 fps test at 90 Hz; without it the ini's vr_fps
# (written by the tab's third choice) reaches the game itself, which sets LARECOMP_XR_FPS.
INI=${LARECOMP_SETTINGS:-}
if [ -z "$INI" ]; then
    for f in "$DATA/larecomp.ini" "$HERE/larecomp.ini"; do
        [ -f "$f" ] && { INI=$f; break; }
    done
fi
if [ -n "${AF_REFRESH_HZ:-}" ]; then
    export AF_REFRESH_FROM=env
else
    INI_HZ=
    [ -n "$INI" ] && INI_HZ=$(sed -n 's/^vr_refresh[[:space:]]*=[[:space:]]*\([0-9]*\).*/\1/p' "$INI" 2> /dev/null | tail -n 1)
    [ -n "$INI_HZ" ] && log "refresh rate $INI_HZ Hz from $INI (the VR tab's Refresh Rate)"
    export AF_REFRESH_FROM=ini
fi
HZ=${AF_REFRESH_HZ:-${INI_HZ:-90}}
case "$HZ" in 72 | 90 | 120) ;; *) log "AF_REFRESH_HZ=$HZ is not 72, 90 or 120, using 90"; HZ=90 ;; esac
if [ -n "${AF_FPS:-}" ]; then
    export LARECOMP_XR_FPS=$AF_FPS
    log "AF_FPS=$AF_FPS: LARECOMP_XR_FPS=$AF_FPS"
fi
if [ "$FLAT" = 1 ]; then
    unset LARECOMP_XR
    log "flat mode: no VR"
else
    export LARECOMP_XR=1
    export LARECOMP_XR_HZ=$HZ
    log "VR mode, $HZ Hz asked of SteamVR"
    # SteamVR's per-app settings, every launch: SteamVR has dropped them from its settings file before (a
    # dashboard change or a restart rewrites the file). 120 Hz is not offered through the OpenXR refresh-rate
    # extension; this is what sets it. No throttling to every other refresh, no motion smoothing (the game
    # paces its own 60 fps and a smoothed frame ghosts). SteamAppId is set by Steam when launched from the library.
    VRCMD=${AF_VRCMD:-/opt/steamvr/bin/linuxarm64/vrcmd}
    if [ -n "$SteamAppId" ] && [ -x "$VRCMD" ]; then
        for kv in preferredRefreshRate=$HZ framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2; do
            "$VRCMD" --set-settings-int "steam.app.$SteamAppId.${kv%=*}" "${kv#*=}" > /dev/null 2>&1 ||
                log "could not set SteamVR's ${kv%=*} for the game (is SteamVR running?)"
        done
    else
        log "not started from a Steam library entry (SteamAppId unset) or no vrcmd: SteamVR's per-app settings left as they are"
    fi
fi

# ---- the headset-in-use marker: other projects' builds wait while it exists
HEADSET_MARK=${AF_HEADSET_FLAG:-${XDG_CACHE_HOME:-$HOME/.cache}/headset-in-use}
MARKED=
if [ "$FLAT" != 1 ]; then
    if [ -e "$HEADSET_MARK" ]; then
        other=$(awk '{print $2}' "$HEADSET_MARK" 2> /dev/null)
        if [ -n "$other" ] && kill -0 "$other" 2> /dev/null; then log "WARNING: $HEADSET_MARK names a running process ($(cat "$HEADSET_MARK"))"; fi
    fi
    # Compilers running now cost the session frames (five usable CPUs shared with SteamVR).
    busy=$(pgrep -x 'clang|clang\+\+|cc1|ld.lld|make|rustc|cargo' 2> /dev/null | wc -l)
    [ "$busy" -gt 0 ] && log "WARNING: $busy build process(es) running at launch; the session will have dropped frames"
fi

# ---- the compositor timing logger (read-only), for the whole session
VRT_PID=
SESSION_START=$LOGS/.session-start
touch "$SESSION_START" 2> /dev/null
if [ "$FLAT" != 1 ] && [ "${AF_VRTIMING:-1}" != 0 ] && command -v python3 > /dev/null 2>&1 &&
    [ -f "$HERE/vrtiming.py" ] && [ -f /opt/steamvr/bin/linuxarm64/libopenvr_api.so ]; then
    env -u LD_LIBRARY_PATH python3 -I "$HERE/vrtiming.py" 86400 "$LOGS/vrtiming-$STAMP.log" > /dev/null 2>&1 &
    VRT_PID=$!
fi

# ---- run; not exec: the wrapper stays to remove the marker and collect the logs. A signal for the
# wrapper (Steam stopping the app) goes on to the game.
cleanup() {
    [ -n "$VRT_PID" ] && kill "$VRT_PID" 2> /dev/null
    if [ -n "$MARKED" ] && [ -f "$HEADSET_MARK" ] && grep -q "^awakeningframe " "$HEADSET_MARK" 2> /dev/null; then rm -f "$HEADSET_MARK"; fi
    MARKED=
}
trap cleanup EXIT
log "starting $EXE ${ARGS[*]}"
"$EXE" "${ARGS[@]}" &
GAME_PID=$!
if [ "$FLAT" != 1 ]; then echo "awakeningframe $GAME_PID" > "$HEADSET_MARK" 2> /dev/null && MARKED=1; fi
trap 'kill -TERM "$GAME_PID" 2> /dev/null' TERM INT HUP
rc=0
while kill -0 "$GAME_PID" 2> /dev/null; do  # a signal interrupts wait before the game is gone
    wait "$GAME_PID"
    rc=$?
done
trap - TERM INT HUP
log "the game exited with status $rc"
cleanup

# ---- keep this session's logs next to the run log (the game's own are rewritten by the next session)
for f in xr_timing.log lasrecomp.log; do
    [ -f "$DATA/$f" ] && [ "$DATA/$f" -nt "$SESSION_START" ] && cp -f "$DATA/$f" "$LOGS/${f%.log}-$STAMP.log" 2> /dev/null
done
rm -f "$SESSION_START"
KEEP=${AF_KEEP:-12}
for pfx in run xr_timing lasrecomp vrtiming; do
    ls -1t "$LOGS/$pfx"-*.log 2> /dev/null | tail -n +$((KEEP + 1)) | xargs -r rm -f --
done
exit $rc
