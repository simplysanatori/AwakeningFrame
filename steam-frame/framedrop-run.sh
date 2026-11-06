#!/bin/sh
# run.sh: what FrameDrop (or Steam) starts for AwakeningFrame when it was installed as a FrameDrop zip.
# (From GalaxyFrame's steam-frame/framedrop-run.sh.)
#
# FrameDrop unpacks this zip onto the Frame and adds its own library entry for this run.sh. The first time it runs,
# and again whenever the VERSION file in this folder differs from the version it installed last, it runs ./install.sh
# without a terminal, with the Steam shortcut step turned off (AWAKENINGFRAME_SKIP_STEAM=1) so there is no second
# library entry, and without the game files step (AF_GAME_SETUP=0): the launch wrapper does that next, and the start
# screen shows the game code build with a progress bar. Then it starts the installed game's launch wrapper.
# Everything the installer prints goes to the log file named below.
#
# Environment (all optional):
#   AWAKENINGFRAME_APP    where the program is installed (the installer's default is used otherwise)
#   FRAMEDROP_RUN_DRY=1   install if needed, print the launcher it would start, and stop
cd "$(dirname "$0")" || exit 1
HERE=$(pwd)
VERSION=$(head -n 1 VERSION 2> /dev/null | tr -d '\r' | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
[ -n "$VERSION" ] || VERSION=unknown
STATE=${XDG_STATE_HOME:-$HOME/.local/state}/framedrop-awakeningframe
LOG=$STATE/install.log
mkdir -p "$STATE" 2> /dev/null

# where install.sh puts it: ~/Games/AwakeningFrame/app, or release/ beside a build from source there
app_dir() {
    if [ -n "$AWAKENINGFRAME_APP" ]; then echo "$AWAKENINGFRAME_APP"; return; fi
    a=$HOME/Games/AwakeningFrame/app
    if [ -x "$a/lasrecomp" ] && ! grep -q '^release ' "$a/.af-install" 2> /dev/null; then a=$HOME/Games/AwakeningFrame/release; fi
    echo "$a"
}

# Symbolic links the zip does not carry are listed in .framedrop-symlinks (path, tab, target); make them
# again, and replace the small file some unzip tools write instead of a link.
if [ -f .framedrop-symlinks ]; then
    while IFS="$(printf '\t')" read -r p t; do
        [ -n "$p" ] || continue
        [ -L "$p" ] && continue
        if [ -e "$p" ] && { [ -d "$p" ] || [ "$(wc -c < "$p")" -gt 4096 ]; }; then continue; fi
        rm -f "$p"; ln -s "$t" "$p"
    done < .framedrop-symlinks
fi

# A FrameDrop unpack can lose the executable bits; give them back to programs and scripts.
python3 -I - "$HERE" << 'PY' 2> /dev/null
import os, stat, sys
for d, _, files in os.walk(sys.argv[1]):
    for n in files:
        p = os.path.join(d, n)
        try:
            if os.path.islink(p):
                continue
            with open(p, "rb") as f:
                h = f.read(4)
            if h == b"\x7fELF" or h[:2] == b"#!":
                os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        except OSError:
            pass
PY

LAUNCHER=$(app_dir)/awakeningframe.sh
if [ ! -x "$LAUNCHER" ] || [ "$(cat "$STATE/installed-version" 2> /dev/null)" != "$VERSION" ]; then
    echo "AwakeningFrame $VERSION: installing (log: $LOG)"
    {
        echo "== $(date '+%Y-%m-%d %H:%M:%S') run.sh: install.sh from $HERE"
        AWAKENINGFRAME_SKIP_STEAM=1 AF_GAME_SETUP=0 bash ./install.sh < /dev/null
    } >> "$LOG" 2>&1
    rc=$?
    LAUNCHER=$(app_dir)/awakeningframe.sh
    if [ "$rc" = 0 ]; then
        printf '%s\n' "$VERSION" > "$STATE/installed-version"
    else
        echo "AwakeningFrame: the installer failed (exit $rc); last lines of $LOG:" >&2
        tail -n 8 "$LOG" >&2
        [ -x "$LAUNCHER" ] || exit "$rc"
        echo "AwakeningFrame: starting the version that was installed before." >&2
    fi
fi

[ -x "$LAUNCHER" ] || { echo "AwakeningFrame: $LAUNCHER is missing" >&2; exit 1; }
if [ -n "$FRAMEDROP_RUN_DRY" ]; then echo "would start: $LAUNCHER $*"; exit 0; fi
exec "$LAUNCHER" "$@"
