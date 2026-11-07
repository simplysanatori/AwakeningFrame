#!/bin/bash
# Checks a release archive's install.sh, uninstall.sh and the launch wrapper the way Frametanium runs them, in a
# scratch home folder: a stand-in for Steam's debugging port (test/mock-steam-cef.py), a stand-in vrcmd, no real
# Steam library, catalog or data folder touched. The headset-in-use marker is the real one (a build waits for it).
#   steam-frame/test-release.sh ARCHIVE.tar.gz [--game DIR] [--frametanium MANAGER.py]
#     --game DIR          also set up real game files: DIR is a folder holding exefs/ and romfs/ (hard-linked into
#                         the scratch home, so nothing is copied or changed), then build the game code from them:
#                         first with a short time budget (it must stop and keep its pieces), then to the end.
#                         Heavy: take the bench lock first.
#     --frametanium FILE  also run Frametanium's own install and remove code (test/frametanium-sandbox.py) on it
# Prints PASS / FAIL lines; exit 1 on any FAIL. AF_TEST_KEEP=1 keeps the scratch folder.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ARCHIVE=$(readlink -f "${1:?usage: $0 ARCHIVE.tar.gz [--game DIR] [--frametanium MANAGER.py]}"); shift
GAME= MANAGER=
while [ $# -gt 0 ]; do
    case "$1" in
        --game) GAME=$(readlink -f "$2"); shift 2 ;;
        --frametanium) MANAGER=$(readlink -f "$2"); shift 2 ;;
        *) echo "unknown $1" >&2; exit 2 ;;
    esac
done
REALFLAG=$HOME/.cache/headset-in-use
# (on the home drive, not /tmp: the game files are hard-linked in, and the game code build writes a few GB)
mkdir -p "$HOME/.cache/af-release-test"
T=$(mktemp -d "$HOME/.cache/af-release-test/run.XXXXXX")
export AF_SANDBOX_ROOT=$HOME/.cache/af-release-test/
MOCK_PID=
cleanup() { [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2> /dev/null; [ -n "${AF_TEST_KEEP:-}" ] && echo "kept $T" || rm -rf "$T"; }
trap cleanup EXIT
fails=0
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
check() { local what=$1; shift; if "$@"; then ok "$what"; else bad "$what"; fi; }
jq() { python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$@"; }

H=$T/home
mkdir -p "$H/.local/share/Steam/userdata/1234/config" "$H/Downloads" "$H/empty"
touch "$H/.local/share/Steam/userdata/1234/config/shortcuts.vdf"
printf '#!/bin/bash\necho "$@" >> "%s/vrcmd.log"\n' "$T" > "$T/vrcmd"; chmod +x "$T/vrcmd"
PORT=$((20000 + RANDOM % 20000))
python3 -I "$HERE/test/mock-steam-cef.py" "$PORT" "$T/state.json" "$T/mock.log" &
MOCK_PID=$!
sleep 1
# (only the scratch home: no AWAKENINGFRAME_APP / LARECOMP_DATA_ROOT, so the defaults under HOME are what is tested)
export HOME=$H AF_CEF_PORT=$PORT AF_VRCMD=$T/vrcmd AF_HEADSET_FLAG=$REALFLAG AF_GAME_SEARCH=$H/empty
unset AWAKENINGFRAME_APP LARECOMP_DATA_ROOT AWAKENINGFRAME_SKIP_STEAM AWAKENINGFRAME_SRC SteamAppId LARECOMP_XR XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME XDG_STATE_HOME
APP=$H/Games/AwakeningFrame/app
DATA=$H/.local/share/lasrecomp
nshort() { jq "$T/state.json" 'len(d["shortcuts"])' 2> /dev/null || echo 0; }

# ---- unpack as Frametanium does, into its apps folder (the budget follows from the path)
U=$H/.local/share/frametanium/apps/awakeningframe
mkdir -p "$U" && tar -xzf "$ARCHIVE" -C "$U"
check "archive: install.sh, uninstall.sh, VERSION at the top" test -f "$U/install.sh" -a -f "$U/uninstall.sh" -a -f "$U/VERSION"
check "archive: no game code or game files" test -z "$(cd "$U" && find . \( -name 'lasgame*' -o -name exefs -o -name romfs -o -name '*.keys' -o -name generated -o -name extracted \) | head -1)"
VER=$(cat "$U/VERSION")

# ---- 1. fresh install, no game files anywhere
(cd "$U" && bash ./install.sh < /dev/null > "$T/i1.log" 2>&1); rc=$?
check "fresh install: exit 0" test $rc -eq 0
for f in lasrecomp hactool awakeningframe.sh setup-game.sh prepare-game.sh versions.h steam-cef.py set-art.py zig/zig lasgen/lasgen gamelib-src/cpu.h art/icon.png DejaVuSans.ttf; do
    check "installed $f" test -e "$APP/$f"
done
check "marked as a release" grep -qx "release $VER" "$APP/.af-install"
check "one shortcut" test "$(nshort)" = 1
APPID=$(jq "$T/state.json" 'list(d["shortcuts"])[0]')
check "shortcut runs the wrapper" test "$(jq "$T/state.json" 'list(d["shortcuts"].values())[0]["exe"]')" = "\"$APP/awakeningframe.sh\""
check "SteamVR keys set" grep -q "steam.app.$APPID.preferredRefreshRate 120" "$T/vrcmd.log"
check "says no game files were found (last line)" sh -c "tail -n 1 '$T/i1.log' | grep -q 'No game files found'"
check "the program runs and says the game code is not ready" sh -c "cd '$APP' && ! LARECOMP_DATA_ROOT='$DATA' ./lasrecomp --game-code-status | grep -q ready"

# ---- 2. an update over it: same entry and appid, data kept
mkdir -p "$DATA/save"; echo "vr_refresh = 90" > "$DATA/larecomp.ini"; echo keep > "$DATA/save/marker"
(cd "$U" && bash ./install.sh < /dev/null > "$T/i2.log" 2>&1); rc=$?
check "update: exit 0" test $rc -eq 0
check "update: still one shortcut, same appid" test "$(nshort)" = 1 -a "$(jq "$T/state.json" 'list(d["shortcuts"])[0]')" = "$APPID"
check "update: said updating it" grep -q "updating it" "$T/i2.log"
check "update: settings and saves kept" test "$(cat "$DATA/larecomp.ini")" = "vr_refresh = 90" -a -f "$DATA/save/marker"

# ---- 3. the launch wrapper with no game files: the start screen with the hint, no --play
# (a stand-in program that prints how it was started; --flat: no VR, no headset marker)
mv "$APP/lasrecomp" "$T/real-lasrecomp"
printf '#!/bin/bash\necho "fake lasrecomp: args=$* hint=${AF_SETUP_HINT:+set}"\n' > "$APP/lasrecomp"; chmod +x "$APP/lasrecomp"
"$APP/awakeningframe.sh" --flat > /dev/null 2>&1
mv "$T/real-lasrecomp" "$APP/lasrecomp"
RUN=$(ls -t "$H"/Games/AwakeningFrame/logs/run-*.log | head -1)
check "wrapper: looked for the game files" grep -q "looking for them (setup-game.sh)" "$RUN"
check "wrapper: start screen with the hint, no --play" grep -q "fake lasrecomp: args= hint=set" "$RUN"

# ---- 4. uninstall from the unpacked folder (Frametanium's removal)
(cd "$U" && bash ./uninstall.sh < /dev/null > "$T/u1.log" 2>&1); rc=$?
check "uninstall: exit 0" test $rc -eq 0
check "uninstall: shortcut removed" test "$(nshort)" = 0
check "uninstall: program removed" test ! -e "$APP"
check "uninstall: data kept" test -f "$DATA/save/marker"

# ---- 5. a build from source in app/ with its own entry: the release goes beside it, the entry is left alone
mkdir -p "$APP"
printf '#!/bin/bash\necho dev build\n' > "$APP/lasrecomp"; chmod +x "$APP/lasrecomp"; echo dev > "$APP/lasgame.so"
printf '#!/bin/bash\n' > "$APP/awakeningframe.sh"
python3 -I "$HERE/steam-cef.py" "(async () => { const id = await SteamClient.Apps.AddShortcut('AwakeningFrame', '$APP/awakeningframe.sh', '$APP', ''); return id; })()" > "$T/devid"
DEVID=$(cat "$T/devid")
(cd "$U" && bash ./install.sh < /dev/null > "$T/i3.log" 2>&1); rc=$?
check "beside a source install: exit 0" test $rc -eq 0
check "beside: release in ~/Games/AwakeningFrame/release" grep -qx "release $VER" "$H/Games/AwakeningFrame/release/.af-install"
check "beside: the source install untouched" test "$(cat "$APP/lasgame.so")" = dev -a ! -e "$APP/.af-install"
check "beside: still one shortcut, the source install's" test "$(nshort)" = 1 -a "$(jq "$T/state.json" 'list(d["shortcuts"].values())[0]["exe"]')" = "\"$APP/awakeningframe.sh\""
(cd "$U" && bash ./uninstall.sh < /dev/null > "$T/u2.log" 2>&1)
check "beside: uninstall removes only the release" test ! -e "$H/Games/AwakeningFrame/release" -a -f "$APP/lasgame.so" -a "$(nshort)" = 1
# --replace-source: the release takes app/ and the same entry
(cd "$U" && bash ./install.sh --replace-source < /dev/null > "$T/i4.log" 2>&1); rc=$?
check "--replace-source: exit 0" test $rc -eq 0
check "--replace-source: app/ is the release, the source build's game code gone" test ! -e "$APP/lasgame.so" -a -f "$APP/lasgen/lasgen"
check "--replace-source: same entry and appid" test "$(nshort)" = 1 -a "$(jq "$T/state.json" 'list(d["shortcuts"])[0]')" = "$DEVID"

# ---- 6. FrameDrop: the zip next to the archive, run.sh installs without a second entry and starts the wrapper
Z=$(ls "$(dirname "$ARCHIVE")"/AwakeningFrame-*-framedrop-linux-arm64.zip 2> /dev/null | head -1)
if [ -n "$Z" ]; then
    (cd "$U" && bash ./uninstall.sh > /dev/null 2>&1)
    FD=$H/framedrop && mkdir -p "$FD" && python3 -I -c 'import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$Z" "$FD"
    chmod -x "$FD/install.sh" "$FD/lasrecomp" 2> /dev/null
    (cd "$FD" && FRAMEDROP_RUN_DRY=1 sh ./run.sh > "$T/fd.log" 2>&1); rc=$?
    check "FrameDrop run.sh: exit 0" test $rc -eq 0
    check "FrameDrop run.sh: would start the installed wrapper" grep -q "would start: $APP/awakeningframe.sh" "$T/fd.log"
    check "FrameDrop run.sh: no Steam entry of its own" test "$(nshort)" = 0
    check "FrameDrop run.sh: exec bits given back" test -x "$APP/lasrecomp"
    python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d["schema"]=="framedrop.install/v1" and d["files"][0]["url"].endswith(sys.argv[2])' \
        "$(dirname "$ARCHIVE")/awakeningframe.framedrop.json" "$(basename "$Z")"
    check "FrameDrop manifest names the zip" test $? -eq 0
    (cd "$U" && bash ./uninstall.sh > /dev/null 2>&1)
fi

# ---- 7. real game files: found, linked in, game code built (stopped once by the budget, then finished)
if [ -n "$GAME" ]; then
    [ -e "$REALFLAG" ] && { echo "the headset is in use: not building" >&2; exit 1; }
    mkdir -p "$H/Downloads/LA"
    cp -al "$GAME/exefs" "$GAME/romfs" "$H/Downloads/LA/"
    T0=$(date +%s)
    (cd "$U" && AF_GAME_SEARCH=$H/Downloads AF_INSTALL_BUDGET=150 bash ./install.sh < /dev/null > "$T/i5.log" 2>&1); rc=$?
    T1=$(date +%s)
    check "install with game files: exit 0 ($((T1 - T0)) s, budget 150 s)" test $rc -eq 0
    check "game files linked into the data folder" test -f "$DATA/extracted/exefs/main" -a -d "$DATA/extracted/romfs/region_common"
    check "the budget stopped the build in time" test $((T1 - T0)) -lt 260
    check "it says the first launch finishes it" sh -c "tail -n 1 '$T/i5.log' | grep -q 'first launch finishes'"
    check "the stopped build kept its pieces" test -d "$DATA/extracted/.gamebuild"
    T0=$(date +%s)
    bash "$APP/setup-game.sh" --app "$APP" --data "$DATA" > "$T/s1.log" 2>&1; rc=$?
    T1=$(date +%s)
    check "the rest of the build: exit 0 ($((T1 - T0)) s)" test $rc -eq 0
    check "game code ready" sh -c "cd '$APP' && LARECOMP_DATA_ROOT='$DATA' ./lasrecomp --game-code-status | grep -q ready"
    check "the library is in the data folder, not next to the program" test -f "$DATA/extracted/lasgame.so" -a ! -e "$APP/lasgame.so"
    (cd "$U" && bash ./install.sh < /dev/null > "$T/i6.log" 2>&1)
    check "install again: game code already ready" sh -c "tail -n 1 '$T/i6.log' | grep -q 'game code built'"
    cp "$T/i5.log" "$T/s1.log" /tmp/ 2> /dev/null
fi

# ---- 8. Frametanium's own code on the archive
if [ -n "$MANAGER" ]; then
    (cd "$U" && bash ./uninstall.sh > /dev/null 2>&1)
    rm -rf "$U"
    python3 -I "$HERE/test/frametanium-sandbox.py" "$MANAGER" "$ARCHIVE" install "v$VER" > "$T/ft1.json" 2>&1; rc=$?
    check "Frametanium install: done" test $rc -eq 0
    check "Frametanium chose the tar.gz on its own" grep -q "\"chosen\": \"$(basename "$ARCHIVE")\"" "$T/ft1.json"
    check "Frametanium ran it as an installer" grep -q '"mode": "installer"' "$T/ft1.json"
    check "Frametanium: one shortcut" test "$(nshort)" = 1
    sed -n 's/.*"message": "\(.*\)",/Frametanium says: \1/p' "$T/ft1.json"
    python3 -I "$HERE/test/frametanium-sandbox.py" "$MANAGER" "$ARCHIVE" remove > "$T/ft2.json" 2>&1; rc=$?
    check "Frametanium remove: done" test $rc -eq 0
    check "Frametanium remove: shortcut and program gone" test "$(nshort)" = 0 -a ! -e "$APP"
fi

echo
if [ "$fails" = 0 ]; then echo "all passed"; else echo "$fails FAILED (logs in $T: AF_TEST_KEEP=1 keeps them)"; fi
[ "$fails" = 0 ]
