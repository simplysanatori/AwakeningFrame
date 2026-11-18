#!/bin/bash
# Checks the Dev kit's scripts against a stand-in headset: a scratch folder acts as the headset's home through a fake ssh
# (FRAME_SSH), a fake pgrep, a stand-in for Steam's debugging port and a stand-in vrcmd. Touches nothing real and never connects
# anywhere.   steam-frame/dev/test-dev-kit.sh     prints PASS / FAIL lines; exit 1 on any FAIL
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$(dirname "$HERE")")
T=$(mktemp -d)
MOCK_PID=
trap '[ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2> /dev/null; rm -rf "$T"' EXIT
fails=0
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
check() { local what=$1; shift; if "$@"; then ok "$what"; else bad "$what"; fi; }

# ---- the scene: a headset home with the released version's files, a fake ssh, a fake pgrep, the kit and an app folder
F=$T/frame                         # the headset's home
mkdir -p "$F/Games/AwakeningFrame/app" "$F/.local/share/lasrecomp/extracted/exefs" "$F/.local/share/lasrecomp/save/save_0" "$T/bin"
echo released > "$F/Games/AwakeningFrame/app/lasrecomp"; echo ex > "$F/.local/share/lasrecomp/extracted/exefs/main"
echo save0 > "$F/.local/share/lasrecomp/save/save_0/slot"; echo "vr_refresh = 90" > "$F/.local/share/lasrecomp/larecomp.ini"
echo "height = 0.1" > "$F/.local/share/lasrecomp/vr_rig.ini"
printf 'LARECOMP_VR_DYNRES=1\n# a comment\nAF_REFRESH_HZ=90\n' > "$F/Games/AwakeningFrame/session.env"
cat > "$T/bin/fakessh" << 'SH'
#!/bin/bash
# a stand-in for `ssh frame COMMAND`: runs COMMAND here, with the scratch folder as the home
export HOME=$FAKE_HOME PATH=$FAKE_BIN:$PATH
{ echo "--- ssh call"; echo "$*"; } >> "$FAKE_SSH_LOG"
exec bash -c "$*"
SH
printf '#!/bin/bash\n[ -n "${FAKE_RUNNING:-}" ] && { echo 4242; exit 0; }\nexit 1\n' > "$T/bin/pgrep"
chmod +x "$T/bin/fakessh" "$T/bin/pgrep"
export FAKE_HOME=$F FAKE_BIN=$T/bin FAKE_SSH_LOG=$T/ssh.log FRAME_SSH=$T/bin/fakessh FRAME_HOME=$F
unset FAKE_RUNNING AFDEV_DATA LARECOMP_DATA_ROOT
sums() { (cd "$1" && find . -type f | sort | xargs sha256sum | sha256sum); }
REL1=$(sums "$F/Games/AwakeningFrame"); REL2=$(sums "$F/.local/share/lasrecomp")

mkdir -p "$T/out/app-abc" "$T/out/app-abc/lib" "$T/out/art-dev" "$T/out/bench"
python3 -I - "$T/out/app-abc/lasrecomp" << 'PY'
import struct, sys   # a 64-byte ELF header for aarch64 (file says "ARM aarch64")
h = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8) + struct.pack("<HHIQQQIHHHHHH", 2, 183, 1, 0, 64, 0, 0, 64, 56, 0, 64, 0, 0)
open(sys.argv[1], "wb").write(h + bytes(64))
PY
echo "0.1.1-dev-abc" > "$T/out/app-abc/VERSION"; echo game > "$T/out/app-abc/lasgame.so"; echo id > "$T/out/app-abc/lasgame.so.id"
echo '#!/bin/bash' > "$T/out/app-abc/awakeningframe.sh"; echo lib > "$T/out/app-abc/lib/libx.so"
for f in run.sh setup-dev-home.sh add-dev-shortcut.py collect-logs.sh frame-bench.sh session.env.example; do cp "$HERE/$f" "$T/out/$f"; done
cp "$REPO/steam-frame/steam-cef.py" "$REPO/steam-frame/set-art.py" "$T/out/"
echo "x" > "$T/out/bench/libxrmock.so"; echo "x" > "$T/out/art-dev/p.png"
I=$HERE/install-to-frame.sh
KITD=$F/Games/AwakeningFrame-dev

# ---- install-to-frame.sh
"$I" "$T/out/app-abc" > "$T/i0.log" 2>&1
check "install: refused without --owner-ok (exit 2), nothing done" test $? -eq 2 -a ! -e "$T/ssh.log"
"$I" --owner-ok --dry-run "$T/out/app-abc" > "$T/i1.log" 2>&1
check "dry run: exit 0, prints the commands" grep -q "dry run: nothing was run" "$T/i1.log"
check "dry run: no ssh, nothing on the headset" test ! -e "$T/ssh.log" -a ! -e "$KITD"
check "dry run names every step" test "$(grep -c '^== ' "$T/i1.log")" = 5
mkdir -p "$T/rel" && cp -r "$T/out/app-abc/." "$T/rel/" && echo 0.1.1 > "$T/rel/VERSION"
"$I" --owner-ok "$T/rel" > "$T/i2.log" 2>&1
check "a release folder (no -dev- stamp) is refused" test $? -ne 0 -a ! -e "$KITD"
FAKE_RUNNING=1 "$I" --owner-ok "$T/out/app-abc" > "$T/i3.log" 2>&1
check "lasrecomp running on the headset: refused (exit 3), no copy" test $? -eq 3 -a ! -e "$KITD/app.incoming" -a ! -e "$KITD/app"
mkdir -p "$KITD"; echo "keep me" > "$KITD/session.env"
"$I" --owner-ok --rate 100000 "$T/out/app-abc" > "$T/i4.log" 2>&1
check "install: exit 0" test $? -eq 0
check "app installed with its files" test -f "$KITD/app/lasrecomp" -a -f "$KITD/app/lasgame.so.id" -a -f "$KITD/app/lib/libx.so"
check "the kit's scripts installed" test -x "$KITD/run.sh" -a -x "$KITD/collect-logs.sh" -a -f "$KITD/bench/libxrmock.so" -a -f "$KITD/art-dev/p.png"
check "session.env of the Dev entry left alone" test "$(cat "$KITD/session.env")" = "keep me"
check "the released version and its data folder untouched" test "$(sums "$F/Games/AwakeningFrame")" = "$REL1" -a "$(sums "$F/.local/share/lasrecomp")" = "$REL2"
check "no staging folder left" test ! -e "$KITD/app.incoming"
for n in 1 2 3; do sleep 1; echo "build $n" > "$T/out/app-abc/lasgame.so.id"; "$I" --owner-ok --rate 100000 "$T/out/app-abc" > "$T/i5-$n.log" 2>&1; done
check "previous apps kept as app.bak-<date>, the last two" test "$(ls -1d "$KITD"/app.bak-* | wc -l)" = 2
check "the newest build is in app, the older ones in the backups" test "$(cat "$KITD/app/lasgame.so.id")" = "build 3"
check "the output says what it did" grep -q "previous app is app.bak-" "$T/i5-3.log"
check "ssh: one connection per step (5 a install, 1 for the refused one)" test "$(grep -c '^--- ssh call' "$T/ssh.log")" = 21
check "nothing was written outside the Dev folder" test "$(find "$F" -newer "$T/i0.log" -type f -not -path "$KITD/*" | wc -l)" = 0

# ---- setup-dev-home.sh (on the headset: here with the scratch home)
export HOME=$F
"$KITD/setup-dev-home.sh" > "$T/s1.log" 2>&1; DEV=$F/.local/share/lasrecomp-dev
check "setup: extracted/ is a link to the release's" test -L "$DEV/extracted" -a "$(readlink "$DEV/extracted")" = "$F/.local/share/lasrecomp/extracted"
check "setup: save, larecomp.ini, vr_rig.ini copied" test -f "$DEV/save/save_0/slot" -a -f "$DEV/larecomp.ini" -a -f "$DEV/vr_rig.ini"
check "setup: no shader cache copied" test ! -e "$DEV/cache"
rm -f "$KITD/session.env"; "$KITD/setup-dev-home.sh" > /dev/null 2>&1
check "setup: session.env made, logs on, the release's lines added" grep -q '^LARECOMP_XR_POSE_LOG=' "$KITD/session.env" -a "$(grep -c '^AF_REFRESH_HZ=90' "$KITD/session.env")" = 1
check "setup: the A/B switches are comments" test "$(grep -c '^AF_FDM\|^AF_THROTTLE\|^LARECOMP_VR_CULL\|^LARECOMP_VR_SCENE\|^LARECOMP_VR_HEADROOM' "$KITD/session.env")" = 0
echo "my dev save" > "$DEV/save/save_0/slot"; echo "dev ini" > "$DEV/larecomp.ini"
"$KITD/setup-dev-home.sh" > /dev/null 2>&1
check "setup: a second run overwrites nothing" test "$(cat "$DEV/save/save_0/slot")" = "my dev save" -a "$(cat "$DEV/larecomp.ini")" = "dev ini"
check "setup: the release's data folder untouched" test "$(sums "$F/.local/share/lasrecomp")" = "$REL2"
bash -c "LOGS=/l STAMP=S; set -a; . '$HERE/session.env.example'; echo \"\$LARECOMP_XR_POSE_LOG \$LARECOMP_LOG_PACE \${AF_FDM-unset} \${LARECOMP_VR_HEADROOM-unset}\"" > "$T/env.out"
check "session.env.example: pose log per launch, pace on, switches off" grep -qx "/l/pose-S.log 1 unset unset" "$T/env.out"

# ---- run.sh: the Dev data folder and logs, then the wrapper
mkdir -p "$KITD/app"; printf '#!/bin/bash\necho "root=$LARECOMP_DATA_ROOT keep=$AF_KEEP args=$*"\n' > "$KITD/app/awakeningframe.sh"; chmod +x "$KITD/app/awakeningframe.sh"
check "run.sh: Dev data folder and 10 logs kept" test "$("$KITD/run.sh" a)" = "root=$F/.local/share/lasrecomp-dev keep=10 args=a"

# ---- collect-logs.sh
L=$KITD/logs; mkdir -p "$L"
mk() {  # stamp, exit status, switch
    cat > "$L/run-$1.log" << EOT
[awakeningframe ${1:9:2}:${1:11:2}:${1:13:2}] wrapper, session $1, steam appid '1', program $KITD/app
[awakeningframe ${1:9:2}:${1:11:2}:${1:13:2}] version 0.1.1-dev-abc, dev abc
[awakeningframe ${1:9:2}:${1:11:2}:${1:13:2}] AF_FDM=$3, VK_INSTANCE_LAYERS='', DISABLE_VULKAN_FDM_INJECTION_LAYER=''
[awakeningframe ${1:9:2}:$(printf %02d $((10#${1:11:2} + 12))):${1:13:2}] the game exited with status $2
EOT
    printf 'LARECOMP_LOG_PACE=1\nAF_FDM=%s\n#AF_THROTTLE=1\n' "$3" > "$L/session-$1.env"
    echo game > "$L/lasrecomp-$1.log"; echo p > "$L/pose-$1.log"
}
mk 20261118-100000 0 0; mk 20261118-110000 0 1; mk 20261118-120000 139 0
echo "current" > "$DEV/lasrecomp.log"
OUT=$T/collected
"$KITD/collect-logs.sh" > "$T/c0.log" 2>&1
check "collect: refused without --owner-ok (exit 2)" test $? -eq 2
: > "$T/ssh.log"
"$KITD/collect-logs.sh" --owner-ok --sessions 2 --out "$OUT" > "$T/c1.log" 2>&1
check "collect: exit 0" test $? -eq 0
check "collect: the last 2 sessions only" test -f "$OUT/logs/run-20261118-120000.log" -a -f "$OUT/logs/run-20261118-110000.log" -a ! -e "$OUT/logs/run-20261118-100000.log"
check "collect: session.env, larecomp.ini, vr_rig.ini, the game log and the pose log" test -f "$OUT/larecomp.ini" -a -f "$OUT/vr_rig.ini" -a -f "$OUT/logs/pose-20261118-120000.log" -a -f "$OUT/logs/session-20261118-110000.env" -a -f "$OUT/data-lasrecomp.log"
check "collect: one ssh connection" test "$(grep -c '^--- ssh call' "$T/ssh.log")" = 1
check "collect: a summary line per session with length, version, switches" grep -q "20261118-110000  12m00s  0.1.1-dev-abc, dev abc  exit 0  switches: AF_FDM=1" "$T/c1.log"
check "collect: a failed session shows its exit status" grep -q "20261118-120000 .*exit 139" "$T/c1.log"
check "collect: nothing changed on the headset" test "$(find "$F" -newer "$T/c0.log" -type f | wc -l)" = 0

# ---- add-dev-shortcut.py against the stand-in for Steam's debugging port
mkdir -p "$F/steam/userdata/1234/config" && touch "$F/steam/userdata/1234/config/shortcuts.vdf"
printf '#!/bin/bash\necho "$@" >> "%s/vrcmd.log"\n' "$T" > "$T/vrcmd"; chmod +x "$T/vrcmd"
PORT=$((20000 + RANDOM % 20000))
python3 -I "$REPO/steam-frame/test/mock-steam-cef.py" "$PORT" "$T/state.json" "$T/mock.log" &
MOCK_PID=$!
sleep 1
jq() { python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$@"; }
for n in p wide hero logo icon; do cp "$REPO/steam-frame/art/icon.png" "$KITD/art-dev/$n.png" 2> /dev/null || echo x > "$KITD/art-dev/$n.png"; done
AF_CEF_PORT=$PORT AF_VRCMD=$T/vrcmd AF_STEAM_DIR=$F/steam python3 -I "$KITD/add-dev-shortcut.py" > "$T/a1.log" 2>&1
check "shortcut: exit 0" test $? -eq 0
check "shortcut: one entry named AwakeningFrame (Dev) that runs the kit's run.sh" test "$(jq "$T/state.json" 'len(d["shortcuts"])')" = 1 -a "$(jq "$T/state.json" 'list(d["shortcuts"].values())[0]["name"]')" = "AwakeningFrame (Dev)" -a "$(jq "$T/state.json" 'list(d["shortcuts"].values())[0]["exe"]')" = "\"$KITD/run.sh\""
APPID=$(jq "$T/state.json" 'list(d["shortcuts"])[0]')
check "shortcut: the four SteamVR keys for its appid, as the launcher sets them" test "$(grep -c "^--set-settings-int steam.app.$APPID\." "$T/vrcmd.log")" = 4
check "shortcut: art set live and into the grid" test "$(jq "$T/state.json" 'len(list(d["shortcuts"].values())[0]["art"])')" = 4 -a -f "$F/steam/userdata/1234/config/grid/${APPID}p.png"
AF_CEF_PORT=$PORT AF_VRCMD=$T/vrcmd AF_STEAM_DIR=$F/steam python3 -I "$KITD/add-dev-shortcut.py" > "$T/a2.log" 2>&1
check "shortcut: a second run updates, no duplicate" test "$(jq "$T/state.json" 'len(d["shortcuts"])')" = 1 -a "$(jq "$T/state.json" 'list(d["shortcuts"])[0]')" = "$APPID"

# ---- frame-bench.sh refusals and plan
mkdir -p "$F/bench-app"; cp "$T/out/app-abc/lasrecomp" "$T/out/app-abc/lasgame.so" "$F/bench-app/"; chmod +x "$F/bench-app/lasrecomp"
touch "$T/headset-in-use"
AF_HEADSET_FLAG=$T/headset-in-use AF_BENCH_LOCK=$T/bench.lock "$KITD/frame-bench.sh" "$F/bench-app" > "$T/b1.log" 2>&1
check "bench: refused while the headset is in use (exit 75)" test $? -eq 75
AF_HEADSET_FLAG=$T/headset-in-use AF_BENCH_LOCK=$T/bench.lock "$KITD/frame-bench.sh" --plan "$F/bench-app" > "$T/b2.log" 2>&1
check "bench: --plan lists the four spots" test "$(grep -c '^  spot ' "$T/b2.log")" = 4
echo
if [ "$fails" = 0 ]; then echo "all passed"; else echo "$fails FAILED"; fi
[ "$fails" = 0 ]
