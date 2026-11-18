#!/bin/bash
# Copies a Dev app folder (steam-frame/dev/make-dev-app.sh) to the Steam Frame over ssh, next to the released version, never over it.
#   install-to-frame.sh --owner-ok [--dry-run] [--rate KBPS] [--keep N] APP_DIR
#   --owner-ok   required: the owner has said the headset may be written to now (it also stands for "not in the headset")
#   --dry-run    print the commands, run nothing (no ssh at all)
#   --rate KBPS  copy speed limit in KB/s (default 2500, the same as scp -l 20000)
#   --keep N     previous app folders kept as app.bak-<date> (default 2)
# What it does, one ssh connection for each step, no loops and no polling:
#   1 checks over ssh: no lasrecomp is running, room for the copy in ~/Games/AwakeningFrame-dev
#   2 copies APP_DIR (tar over ssh, rate limited) to ~/Games/AwakeningFrame-dev/app.incoming
#   3 checks every copied file against the sums made here
#   4 moves app to app.bak-<date> and app.incoming to app, removes the older app.bak-* beyond --keep
#   5 puts the kit's scripts (run.sh, setup-dev-home.sh, ... art-dev/, bench/) in ~/Games/AwakeningFrame-dev; never session.env
# It writes nothing under ~/Games/AwakeningFrame/ or ~/.local/share/lasrecomp/, compiles nothing, and starts nothing.
# Environment: FRAME_SSH (default "ssh frame"), FRAME_HOME (the headset's home folder; default: what $HOME is there; a test sets it).
set -euo pipefail
OK=0 DRY=0 RATE=2500 KEEP=2 APP=
while [ $# -gt 0 ]; do
    case $1 in
        --owner-ok) OK=1 ;;
        --dry-run) DRY=1 ;;
        --rate) RATE=${2:?}; shift ;;
        --keep) KEEP=${2:?}; shift ;;
        -h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        -*) echo "install-to-frame: unknown argument $1 (--help)" >&2; exit 2 ;;
        *) APP=$1 ;;
    esac
    shift
done
[ "$OK" = 1 ] || { echo "install-to-frame: needs --owner-ok (the owner's OK to write to the headset); nothing done" >&2; exit 2; }
[ -n "$APP" ] && [ -d "$APP" ] || { echo "usage: install-to-frame.sh --owner-ok [--dry-run] APP_DIR" >&2; exit 2; }
APP=$(cd "$APP" && pwd)
for f in lasrecomp lasgame.so lasgame.so.id awakeningframe.sh VERSION lib; do
    [ -e "$APP/$f" ] || { echo "install-to-frame: $APP has no $f (is it a make-dev-app.sh folder?)" >&2; exit 1; }
done
grep -q -- '-dev-' "$APP/VERSION" || { echo "install-to-frame: $APP/VERSION is not a dev stamp: not copying a release over the Dev app" >&2; exit 1; }
file -b "$APP/lasrecomp" | grep -q 'ARM aarch64' || { echo "install-to-frame: $APP/lasrecomp is not an aarch64 program" >&2; exit 1; }
KITSRC=$(dirname "$APP")
read -ra SSH <<< "${FRAME_SSH:-ssh frame}"
# the headset's folder, as text the remote shell expands
if [ -n "${FRAME_HOME:-}" ]; then RHOME=$(printf '%q' "$FRAME_HOME"); else RHOME='"$HOME"'; fi
KIT="$RHOME/Games/AwakeningFrame-dev"
SIZEKB=$(du -sk "$APP" | cut -f1)
DATE=$(date +%Y%m%d-%H%M%S)
KITFILES=()
for f in run.sh setup-dev-home.sh add-dev-shortcut.py collect-logs.sh frame-bench.sh session.env.example steam-cef.py set-art.py art-dev bench; do
    [ -e "$KITSRC/$f" ] && KITFILES+=("$f")
done
THROTTLE='import sys,time
r=int(sys.argv[1])*1024;t=time.time();n=0
while True:
    b=sys.stdin.buffer.read(65536)
    if not b: break
    sys.stdout.buffer.write(b);sys.stdout.buffer.flush();n+=len(b)
    d=n/r-(time.time()-t)
    if d>0: time.sleep(d)'

run() {  # <what> <command...>: print it; run it unless --dry-run
    echo "+ $*"
    [ "$DRY" = 1 ] || "$@"
}
say() { echo "== $*"; }
WHERE=${FRAME_HOME:-"the headset"}
echo "install $APP ($(head -n 1 "$APP/VERSION"), ${SIZEKB} KB) to $WHERE:Games/AwakeningFrame-dev/app; ssh: ${SSH[*]}"

say "1 preflight"
PRE="set -e; K=$KIT; if pgrep -x lasrecomp > /dev/null; then echo RUNNING; exit 3; fi; mkdir -p \"\$K\"; df -Pk \"\$K\" | awk 'NR==2{print \"FREEKB\", \$4}'; echo OK"
if [ "$DRY" = 1 ]; then echo "+ ${SSH[*]} '$PRE'"; else
    out=$("${SSH[@]}" "$PRE") || { echo "install-to-frame: lasrecomp is running on the headset (or ssh failed): nothing copied" >&2; exit 3; }
    echo "$out"
    free=$(echo "$out" | sed -n 's/^FREEKB //p')
    [ -n "$free" ] && [ "$free" -gt $((SIZEKB * 2 + 200000)) ] || { echo "install-to-frame: not enough room on the headset ($free KB free, need about $((SIZEKB * 2)))" >&2; exit 1; }
fi

say "2 copy to app.incoming (tar over ssh, $RATE KB/s)"
CP="set -e; K=$KIT; rm -rf \"\$K/app.incoming\"; mkdir -p \"\$K/app.incoming\"; tar -C \"\$K/app.incoming\" -xf -"
echo "+ tar -C $APP -cf - . | python3 -I -c <throttle> $RATE | ${SSH[*]} '$CP'"
[ "$DRY" = 1 ] || { tar -C "$APP" -cf - . | python3 -I -c "$THROTTLE" "$RATE" | "${SSH[@]}" "$CP"; }

say "3 check the copy against the sums made here"
CK="set -e; cd $KIT/app.incoming; sha256sum -c --quiet; echo \"files: \$(find . -type f | wc -l)\""
echo "+ (cd $APP && find . -type f | sort | xargs sha256sum) | ${SSH[*]} '$CK'"
if [ "$DRY" != 1 ]; then
    (cd "$APP" && find . -type f | sort | xargs sha256sum) | "${SSH[@]}" "$CK" | sed 's/^/   remote /'
    echo "   local files: $(cd "$APP" && find . -type f | wc -l)"
fi

say "4 swap in, keep the last $KEEP"
SW="set -e; K=$KIT; cd \"\$K\"; if [ -e app ]; then mv app \"app.bak-$DATE\"; echo \"previous app is app.bak-$DATE\"; fi; mv app.incoming app; ls -1d app.bak-* 2> /dev/null | sort | head -n -$KEEP | xargs -r rm -rf; echo \"kept: \$(ls -1d app app.bak-* | tr '\\n' ' ')\""
run "${SSH[@]}" "$SW"

say "5 the kit's scripts (never session.env)"
if [ ${#KITFILES[@]} -gt 0 ]; then
    KT="set -e; K=$KIT; mkdir -p \"\$K\"; tar -C \"\$K\" -xf -; chmod +x \"\$K\"/*.sh \"\$K\"/*.py 2> /dev/null || true; echo \"kit: ${KITFILES[*]}\""
    echo "+ tar -C $KITSRC -cf - ${KITFILES[*]} | ${SSH[*]} '$KT'"
    [ "$DRY" = 1 ] || tar -C "$KITSRC" -cf - "${KITFILES[@]}" | "${SSH[@]}" "$KT"
else
    echo "   no kit files next to $APP: skipped"
fi
echo
if [ "$DRY" = 1 ]; then echo "dry run: nothing was run"; else
    echo "done: $(head -n 1 "$APP/VERSION") is in ~/Games/AwakeningFrame-dev/app on the headset. Not touched: ~/Games/AwakeningFrame/, ~/.local/share/lasrecomp/."
    echo "first time: on the headset run ~/Games/AwakeningFrame-dev/setup-dev-home.sh and add-dev-shortcut.py, then launch \"AwakeningFrame (Dev)\"."
fi
