#!/bin/bash
# Checks install.sh, awakeningframe.sh and uninstall.sh without SteamVR and without touching the real Steam:
# a stand-in source tree, a scratch program and data folder, a scratch Steam folder, a stand-in for Steam's
# debugging port (test/mock-steam-cef.py) and a stand-in vrcmd that logs its arguments.
#   steam-frame/test-install.sh        prints PASS / FAIL lines; exit 1 on any FAIL
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$HERE")
T=$(mktemp -d)
MOCK_PID=
trap '[ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2> /dev/null; rm -rf "$T"' EXIT
fails=0
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
check() { local what=$1; shift; if "$@"; then ok "$what"; else bad "$what"; fi; }

# ---- the scene: source tree, folders, stand-ins
mkdir -p "$T/src/runtime" "$T/src/build" "$T/steam/userdata/1234/config" "$T/steam/userdata/0/config" "$T/data/extracted/exefs"
echo "all:" > "$T/src/Makefile"
printf '#!/bin/bash\necho "fake lasrecomp: XR=${LARECOMP_XR:-unset} HZ=${LARECOMP_XR_HZ:-unset} FPS=${LARECOMP_XR_FPS:-unset} root=$LARECOMP_DATA_ROOT args=$*"\necho "# LARECOMP_XR_HZ"\ncase "$FAKE_MODE" in sleep) exec sleep 30 ;; esac\n' > "$T/src/build/lasrecomp"
chmod +x "$T/src/build/lasrecomp"
echo fake-library > "$T/src/build/lasgame.so"; echo fake-id > "$T/src/build/lasgame.so.id"
touch "$T/data/extracted/exefs/main" "$T/steam/userdata/1234/config/shortcuts.vdf"
printf '#!/bin/bash\necho "$@" >> "%s/vrcmd.log"\n' "$T" > "$T/vrcmd"; chmod +x "$T/vrcmd"
PORT=$((20000 + RANDOM % 20000))
python3 -I "$HERE/test/mock-steam-cef.py" "$PORT" "$T/state.json" "$T/mock.log" &
MOCK_PID=$!
sleep 1
export AWAKENINGFRAME_APP=$T/Games/AwakeningFrame/app LARECOMP_DATA_ROOT=$T/data AF_STEAM_DIR=$T/steam AF_CEF_PORT=$PORT \
       AF_VRCMD=$T/vrcmd AF_HEADSET_FLAG=$T/headset-in-use
unset AWAKENINGFRAME_SKIP_STEAM AWAKENINGFRAME_SRC SteamAppId LARECOMP_XR
APP=$AWAKENINGFRAME_APP
jq() { python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$@"; }

# ---- install, skipping Steam
"$REPO/install.sh" --src "$T/src" --no-build --skip-steam > "$T/i1.log" 2>&1
check "install (no Steam): exit 0" test $? -eq 0
for f in lasrecomp lasgame.so lasgame.so.id awakeningframe.sh steam-cef.py set-art.py; do check "installed $f" test -f "$APP/$f"; done
check "the program's art copied" test -f "$APP/art/icon.png"
check "no Steam traffic when skipped" test ! -s "$T/mock.log"

# ---- install with Steam (stand-in): shortcut, art, SteamVR keys
"$REPO/install.sh" --src "$T/src" --no-build > "$T/i2.log" 2>&1
check "install (Steam): exit 0" test $? -eq 0
APPID=$(jq "$T/state.json" 'list(d["shortcuts"])[0]' 2> /dev/null)
check "one shortcut named AwakeningFrame" test "$(jq "$T/state.json" 'len(d["shortcuts"])')" = 1 -a "$(jq "$T/state.json" 'list(d["shortcuts"].values())[0]["name"]')" = AwakeningFrame
check "shortcut runs the wrapper" test "$(jq "$T/state.json" 'list(d["shortcuts"].values())[0]["exe"]')" = "\"$APP/awakeningframe.sh\""
check "appid has the shortcut bit" test "$((APPID & 0x80000000))" -ne 0
check "four library images set live" test "$(jq "$T/state.json" 'len(list(d["shortcuts"].values())[0]["art"])')" = 4
check "dock icon set to the program's art/icon.png" test "$(jq "$T/state.json" 'list(d["shortcuts"].values())[0].get("icon")')" = "$APP/art/icon.png"
G=$T/steam/userdata/1234/config/grid
for s in p "" _hero _logo _icon; do check "grid file $APPID$s.png" test -f "$G/$APPID$s.png"; done
check "the userdata/0 folder was left alone" test ! -d "$T/steam/userdata/0/config/grid"
for kv in preferredRefreshRate=120 framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2; do
  check "vrcmd: ${kv%=*} for the appid" grep -qx -- "--set-settings-int steam.app.$APPID.${kv%=*} ${kv#*=}" "$T/vrcmd.log"
done
# again: updated, not duplicated
"$REPO/install.sh" --src "$T/src" --no-build > "$T/i3.log" 2>&1
check "second install: still one shortcut, same appid" test "$(jq "$T/state.json" 'len(d["shortcuts"])')" = 1 -a "$(jq "$T/state.json" 'list(d["shortcuts"])[0]')" = "$APPID"
check "second install said updating" grep -q "updating it" "$T/i3.log"

# ---- refusals
touch "$AF_HEADSET_FLAG"
"$REPO/install.sh" --src "$T/src" --skip-steam > "$T/i4.log" 2>&1
check "build refused while the headset flag exists (exit 75)" test $? -eq 75
rm -f "$AF_HEADSET_FLAG"
"$REPO/install.sh" --src "$T/nowhere" --skip-steam > "$T/i5.log" 2>&1
check "not a source tree: refused" test $? -ne 0
AF_CEF_PORT=1 "$REPO/install.sh" --src "$T/src" --no-build > "$T/i6.log" 2>&1
check "Steam unreachable: refused" test $? -ne 0

# ---- the wrapper (the fake lasrecomp prints its environment)
export SteamAppId=$APPID
: > "$T/vrcmd.log"
"$APP/awakeningframe.sh" extra-arg
RUN=$(ls "$T"/Games/AwakeningFrame/logs/run-*.log | head -1)
check "wrapper log written" test -f "$RUN"
check "VR on, 90 Hz (the default), data root, --play" grep -q "fake lasrecomp: XR=1 HZ=90 FPS=unset root=$T/data args=--play extra-arg" "$RUN"
check "wrapper set the four SteamVR keys at launch" test "$(grep -c "steam.app.$APPID" "$T/vrcmd.log")" = 4
check "headset flag removed after exit" test ! -e "$AF_HEADSET_FLAG"
sleep 1; echo 'AF_REFRESH_HZ=90' > "$T/Games/AwakeningFrame/session.env"
"$APP/awakeningframe.sh" --flat
RUN2=$(ls -t "$T"/Games/AwakeningFrame/logs/run-*.log | head -1)
check "--flat: no VR variable" grep -q "fake lasrecomp: XR=unset HZ=unset" "$RUN2"
check "--flat: no SteamVR keys, no flag" test "$(grep -c 'flat mode' "$RUN2")" = 1
sleep 1
"$APP/awakeningframe.sh" > /dev/null; RUN3=$(ls -t "$T"/Games/AwakeningFrame/logs/run-*.log | head -1)
check "session.env: 90 Hz asked" grep -q "HZ=90" "$RUN3"
rm -f "$T/Games/AwakeningFrame/session.env"
# the VR tab's Refresh Rate: vr_refresh in the data root's larecomp.ini; AF_FPS from session.env
printf 'vr_refresh = 120\nvr_fps = 60\n' > "$T/data/larecomp.ini"
sleep 1; "$APP/awakeningframe.sh" > /dev/null; RUN4=$(ls -t "$T"/Games/AwakeningFrame/logs/run-*.log | head -1)
check "larecomp.ini in the data root: 120 Hz asked" grep -q "HZ=120 FPS=unset" "$RUN4"
rm -f "$T/data/larecomp.ini"
echo 'AF_FPS=60' > "$T/Games/AwakeningFrame/session.env"
sleep 1; "$APP/awakeningframe.sh" > /dev/null; RUN5=$(ls -t "$T"/Games/AwakeningFrame/logs/run-*.log | head -1)
check "session.env AF_FPS=60: LARECOMP_XR_FPS=60" grep -q "HZ=90 FPS=60" "$RUN5"
rm -f "$T/Games/AwakeningFrame/session.env"
# the flag exists while the game runs, and goes when the wrapper is stopped
sleep 1; FAKE_MODE=sleep "$APP/awakeningframe.sh" > /dev/null 2>&1 &
WPID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -e "$AF_HEADSET_FLAG" ] && break; sleep 0.3; done
check "headset flag exists while the game runs" grep -q '^awakeningframe [0-9]' "$AF_HEADSET_FLAG"
kill -TERM "$WPID"; wait "$WPID" 2> /dev/null
sleep 0.5
check "headset flag removed after TERM" test ! -e "$AF_HEADSET_FLAG"
check "the game was stopped with its wrapper" test -z "$(pgrep -f 'sleep 30' -P 1 2> /dev/null)"

# ---- uninstall
"$REPO/uninstall.sh" > "$T/u.log" 2>&1
check "uninstall: exit 0" test $? -eq 0
check "shortcut removed" test "$(jq "$T/state.json" 'len(d["shortcuts"])')" = 0
check "art files removed" test ! -e "$G/${APPID}p.png"
check "program folder removed" test ! -e "$APP"
check "data folder kept" test -f "$T/data/extracted/exefs/main"
echo
if [ "$fails" = 0 ]; then echo "all passed"; else echo "$fails FAILED"; fi
[ "$fails" = 0 ]
