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
# the whole bench against a stand-in program that writes the log lines a run has (two programs: a table and a comparison)
printf '#!/bin/bash\necho "[info] fake"\nfor s in Field_11C Field_06C MarinTarinHouse_01A Lv01TailCave_04B; do echo "[warp] jumping to X / $s"; for i in $(seq 60); do echo "[pace] period 16.67 blocked 1.00 busy 5.00 cost 5.00 ms (queue wait 0.10, cpu 3.20, render cpu 2.10, gpu $GPU, gpu wait 0.2, submit 0.1, present 0.1, hooks 0.1, lock 0.0)"; done; done > "$LARECOMP_DATA_ROOT/lasrecomp.log"\n' > "$F/bench-app/lasrecomp"
cp -r "$F/bench-app" "$F/bench-app2"; sed -i 's/\$GPU/7.00/' "$F/bench-app2/lasrecomp"; sed -i 's/\$GPU/4.50/' "$F/bench-app/lasrecomp"
echo "0.1.1" > "$F/bench-app/VERSION"; echo "0.1.1-dev-abc" > "$F/bench-app2/VERSION"
mkdir -p "$F/.local/share/lasrecomp-dev/extracted/exefs"; echo ex > "$F/.local/share/lasrecomp-dev/extracted/exefs/main"
AFDEV_BENCH_ROOT=$T/scratch AF_BENCH_LOCK=$T/bench.lock AF_HEADSET_FLAG=$T/nohs FRAME_BENCH_ALLOW_RUNNING=1 "$KITD/frame-bench.sh" --dwell 1 --settle 0 --hz 120 --out "$T/res.md" "$F/bench-app" "$F/bench-app2" > "$T/b3.log" 2>&1
check "bench: two programs run one after the other, exit 0" test $? -eq 0
check "bench: one results file with a row for each spot" test "$(grep -c '^| .* | 60 | ' "$T/res.md")" = 8
check "bench: GPU ms and FPS per spot" grep -q "| 60 | 60.0 | 4.50 |" "$T/res.md"
check "bench: the second program compared with the first" grep -q "(+2.50)" "$T/res.md"
check "bench: peak memory in the results, the lock released" sh -c "grep -q 'MB peak RSS' '$T/res.md' && [ ! -e '$T/bench.lock' ]"
check "bench: the Dev and released data folders untouched" test "$(sums "$F/.local/share/lasrecomp")" = "$REL2"
# variants: a stand-in that takes its GPU time from $LARECOMP_FAKE_GPU, notes its cache folder and writes the settings it got
mkdir -p "$F/bench-var"; cp "$F/bench-app/lasgame.so" "$F/bench-var/"; echo "0.1.1-dev-var" > "$F/bench-var/VERSION"
cat > "$F/bench-var/lasrecomp" << 'EOT'
#!/bin/bash
R=$LARECOMP_DATA_ROOT
if [ -e "$R/cache/marker" ]; then c=seen; else c=absent; touch "$R/cache/marker"; fi
env | grep -E '^(LARECOMP_VR_GAZE_MOCK|LARECOMP_VR_DYNRES|MOCK_XR_EYE_SIZE|LARECOMP_FAKE_(GPU|X))=' | sort > "$R/env.txt"
{
echo "[info] cache marker $c"
for w in ${LARECOMP_WARP_AT//,/ }; do s=${w##*:}
    echo "[warp] jumping to X / $s"
    for i in $(seq 60); do echo "[pace] period 16.67 blocked 1.00 busy 5.00 cost 5.00 ms (queue wait 0.10, cpu 3.20, render cpu 2.10, gpu ${LARECOMP_FAKE_GPU:-4.50}, gpu wait 0.2, submit 0.1, present 0.1, hooks 0.1, lock 0.0)"; done
done
} > "$R/lasrecomp.log"
EOT
chmod +x "$F/bench-var/lasrecomp"
VB() { AFDEV_BENCH_ROOT=$T/scratch AF_BENCH_LOCK=$T/bench.lock AF_HEADSET_FLAG=$T/nohs FRAME_BENCH_ALLOW_RUNNING=1 "$KITD/frame-bench.sh" --dwell 1 --settle 0 "$@"; }
VB --out "$T/v1.md" --variant base: --variant fast:LARECOMP_FAKE_GPU=3.00,LARECOMP_FAKE_X=1 "$F/bench-var" > "$T/v1.log" 2>&1
check "bench variants: two variants of one app, exit 0" test $? -eq 0
check "bench variants: a section for each run with its settings" sh -c "grep -c '^## 1-bench-var\.' '$T/v1.md' | grep -qx 2 && grep -q 'Variant fast: LARECOMP_FAKE_GPU=3.00, LARECOMP_FAKE_X=1' '$T/v1.md' && grep -q 'Variant base: no extra settings' '$T/v1.md'"
check "bench variants: the comparison with the first run has the right difference" grep -q "3.00 (-1.50)" "$T/v1.md"
check "bench variants: a table per spot, rows are the runs" test "$(grep -c '^### ' "$T/v1.md")" = 4 -a "$(grep -c '^| 1-bench-var.fast | 3.00 | -1.50 | ' "$T/v1.md")" = 4
check "bench variants: the variant's variables reach the program, the lock is released" sh -c "grep -qx 'LARECOMP_FAKE_GPU=3.00' '$T/scratch/run-1-bench-var.fast/env.txt' && [ ! -e '$T/bench.lock' ]"
check "bench variants: the second variant sees the cache the first one filled" sh -c "grep -q 'cache marker absent' '$T/v1.md.d/1-bench-var.base.log' && grep -q 'cache marker seen' '$T/v1.md.d/1-bench-var.fast.log'"
check "bench variants: the shared cache is removed after the last variant" test ! -e "$T/scratch/cache-1-bench-var"
VB --out "$T/v2.md" --warm --variant a:LARECOMP_FAKE_GPU=5.00 --variant b:LARECOMP_FAKE_GPU=6.00 "$F/bench-var" > "$T/v2.log" 2>&1
check "bench variants: --warm fills the cache before the first variant (warm-up marked, not counted)" sh -c "grep -q 'warm-up, not counted' '$T/v2.md' && grep -q 'cache marker seen' '$T/v2.md.d/1-bench-var.a.log' && grep -q 'cache marker absent' '$T/v2.md.d/1-bench-var.warm.log'"
VB --out "$T/v3.md" --variant 'bad:FAKE X=1' "$F/bench-var" > "$T/v3.log" 2>&1; r1=$?
VB --out "$T/v3.md" --variant 'bad:LARECOMP_FAKE_X=a b' "$F/bench-var" > "$T/v3.log" 2>&1; r2=$?
VB --out "$T/v3.md" --variant 'bad:LARECOMP_FAKE_X=$HOME' "$F/bench-var" > "$T/v3.log" 2>&1; r3=$?
VB --out "$T/v3.md" --variant 'bad name:LARECOMP_FAKE_X=1' "$F/bench-var" > "$T/v3.log" 2>&1; r4=$?
VB --out "$T/v3.md" --variant 'bad:x=1' "$F/bench-var" > "$T/v3.log" 2>&1; r5=$?
VB --out "$T/v3.md" --variant 'bad:LD_PRELOAD=/x.so' "$F/bench-var" > "$T/v3.log" 2>&1; r6=$?
check "bench variants: bad --variant values (and variables other than the game's and the mock's) are refused with exit 2" test "$r1$r2$r3$r4$r5$r6" = 222222
VB --out "$T/v4.md" --spots mabe,forest --gaze 0,0 --eye 1728x1728 "$F/bench-var" > "$T/v4.log" 2>&1
check "bench spots: only the chosen spots in the results" test "$(grep -c '^| .* | 60 | ' "$T/v4.md")" = 2 -a "$(grep -c 'MarinTarin\|Tail' "$T/v4.md")" = 0
check "bench gaze and eye: LARECOMP_VR_GAZE_MOCK, MOCK_XR_EYE_SIZE and LARECOMP_VR_DYNRES=0 reach the program" test "$(cat "$T/scratch/run-1-bench-var/env.txt")" = "$(printf 'LARECOMP_VR_DYNRES=0\nLARECOMP_VR_GAZE_MOCK=0,0\nMOCK_XR_EYE_SIZE=1728x1728')"
VB --plan --spots forest,cave --gaze 0.1,-0.2 --warm --variant a:LARECOMP_FAKE_GPU=1 --variant b: "$F/bench-var" "$F/bench-app" > "$T/v5.log" 2>&1
check "bench plan: the chosen spots, the runs with settings, gaze and eye" test "$(grep -c '^  spot ' "$T/v5.log")" = 2 -a "$(grep -c '^  run: ' "$T/v5.log")" = 4 && grep -q 'variant a: LARECOMP_FAKE_GPU=1' "$T/v5.log" && grep -q 'mock gaze 0.1,-0.2' "$T/v5.log" && grep -q 'eye size 1680x1760' "$T/v5.log"
check "bench plan: an estimated time (4 runs and 2 warm-ups of 77 s: about 8 minutes)" grep -q 'about 8 minutes' "$T/v5.log"
VB --plan "$F/bench-app" > "$T/v6.log" 2>&1
check "bench plan: without variants one run and its estimate" grep -q 'about 2 minutes' "$T/v6.log"
VB --out "$T/v7.md" --gaze x "$F/bench-app" > "$T/v7.log" 2>&1; r1=$?
VB --out "$T/v7.md" --eye 12 "$F/bench-app" > "$T/v7.log" 2>&1; r2=$?
VB --out "$T/v7.md" --spots forest,moon "$F/bench-app" > "$T/v7.log" 2>&1; r3=$?
check "bench: bad --gaze, --eye and --spots are refused with exit 2" test "$r1$r2$r3" = 222
AF_HEADSET_FLAG=$T/headset-in-use AF_BENCH_LOCK=$T/bench.lock "$KITD/frame-bench.sh" --variant a:LARECOMP_FAKE_GPU=1 "$F/bench-var" > "$T/v8.log" 2>&1
check "bench variants: exit 75 while the headset flag exists" test $? -eq 75
echo
if [ "$fails" = 0 ]; then echo "all passed"; else echo "$fails FAILED"; fi
[ "$fails" = 0 ]
