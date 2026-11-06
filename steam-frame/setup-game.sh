#!/bin/bash
# setup-game.sh: gets AwakeningFrame's game files ready without a terminal, for install.sh (Frametanium, FrameDrop)
# and the launch wrapper. Three steps, each skipped when it is done already:
#   1. game files: if the data folder has no extracted/exefs and romfs yet, look for your own copy of the game in
#      the usual places (below) and extract it there with prepare-game.sh: an NSP or XCI of the base game plus
#      prod.keys, or a folder with the extracted exefs and romfs (hard-linked when on the same drive).
#   2. game code: build it from those files with the program itself (lasrecomp --build-game-code: the bundled
#      generator and zig), into the data folder's extracted/lasgame.so. Stopped when the time budget runs out; the
#      finished pieces stay, and the next launch's start screen finishes the build with a progress bar.
#   3. nothing else: shader preparation stays at the first launch.
#   setup-game.sh --app DIR [--data DIR] [--budget SECONDS] [--no-build]
#     --app DIR       the installed program (lasrecomp, hactool, prepare-game.sh)
#     --data DIR      the data folder (default LARECOMP_DATA_ROOT, else ~/.local/share/lasrecomp)
#     --budget S      stop the game code build after S seconds (0, the default: no limit)
#     --no-build      only step 1
# Where it looks (AF_GAME_SEARCH=dir:dir:... replaces the list): ~/Games/AwakeningFrame/game-files, ~/Downloads,
# ~/Documents, ~/Desktop, your home folder itself, and SD cards and drives under /run/media; up to four folders deep.
# prod.keys: the same places and ~/.switch. The keys are read where they are, never copied.
# Exit status: 0 game files and game code ready, 3 no game files found, 4 game files ready but the game code not
# (yet) built, 1 an error (the message says which).
set -u
APP= DATA=${LARECOMP_DATA_ROOT:-$HOME/.local/share/lasrecomp} BUDGET=0 BUILD=1
while [ $# -gt 0 ]; do
    case "$1" in
        --app) APP=$2; shift 2 ;;
        --data) DATA=$2; shift 2 ;;
        --budget) BUDGET=$2; shift 2 ;;
        --no-build) BUILD=0; shift ;;
        -h | --help) sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "usage: $0 --app DIR [--data DIR] [--budget SECONDS] [--no-build]" >&2; exit 2 ;;
    esac
done
[ -n "$APP" ] && [ -x "$APP/lasrecomp" ] || { echo "setup-game: --app DIR must hold lasrecomp" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
PREP=$HERE/prepare-game.sh
[ -f "$PREP" ] || PREP=$APP/prepare-game.sh
X=$DATA/extracted
say() { echo "setup-game: $*"; }

have_files() { [ -f "$X/exefs/main" ] && [ -d "$X/romfs/region_common" ]; }

# ---- 1. the game files
if have_files; then
    say "game files already in $X"
else
    if [ -n "${AF_GAME_SEARCH:-}" ]; then
        IFS=: read -r -a PLACES <<< "$AF_GAME_SEARCH"
    else
        PLACES=("$HOME/Games/AwakeningFrame/game-files" "$HOME/Downloads" "$HOME/Documents" "$HOME/Desktop" "$HOME")
        for m in /run/media/*/* /run/media/*; do [ -d "$m" ] && PLACES+=("$m"); done
    fi
    found_pkgs=() found_dirs=() keys=
    for p in "${PLACES[@]}"; do
        [ -d "$p" ] || continue
        depth=4; [ "$p" = "$HOME" ] && depth=1   # (the home folder itself: only its top level; the rest is above)
        while IFS= read -r -d '' f; do found_pkgs+=("$f"); done < <(
            find "$p" -maxdepth "$depth" -type f \( -iname '*.nsp' -o -iname '*.xci' \) -size +1000M -print0 2>/dev/null)
        while IFS= read -r -d '' f; do
            d=$(dirname "$(dirname "$f")")
            case "$d" in "$DATA" | "$DATA"/*) continue ;; esac   # (our own copy)
            [ -d "$d/romfs/region_common" ] && found_dirs+=("$d")
        done < <(find "$p" -maxdepth "$((depth + 1))" -type f -path '*/exefs/main.npdm' -print0 2>/dev/null)
        if [ -z "$keys" ]; then
            keys=$(find "$p" -maxdepth "$depth" -type f -iname 'prod.keys' -print -quit 2>/dev/null)
        fi
    done
    [ -z "$keys" ] && [ -f "$HOME/.switch/prod.keys" ] && keys=$HOME/.switch/prod.keys
    # This game's packages first (its title ID or name in the file name); updates and DLC are never the base game
    pkgs=() rest=()
    for f in "${found_pkgs[@]+"${found_pkgs[@]}"}"; do
        b=$(basename "$f")
        case "${b,,}" in
            *01006bb00c6f0800* | *\[upd\]* | *update*) continue ;;
            *01006bb00c6f0000* | *awakening*) pkgs+=("$f") ;;
            *) rest+=("$f") ;;
        esac
    done
    pkgs+=("${rest[@]+"${rest[@]}"}")
    mkdir -p "$DATA"
    done_ok=0
    for d in "${found_dirs[@]+"${found_dirs[@]}"}"; do
        say "found extracted game files in $d"
        if bash "$PREP" "$d" --dest "$X"; then done_ok=1; break; fi
    done
    if [ "$done_ok" = 0 ] && [ ${#pkgs[@]} -gt 0 ]; then
        if [ -z "$keys" ]; then
            say "found ${pkgs[0]}, but no prod.keys: put prod.keys from your Switch next to it"
            exit 3
        fi
        tkeys=$(dirname "$keys")/title.keys
        [ -f "$tkeys" ] || tkeys=
        free=$(df -Pk "$DATA" | awk 'NR == 2 {print int($4 / 1048576)}')
        [ "${free:-0}" -ge 13 ] || say "warning: ${free} GB free where the game files go; extracting needs about 13 GB for a moment"
        for f in "${pkgs[@]}"; do
            say "found $f; extracting it with your prod.keys (a few minutes)"
            if bash "$PREP" "$f" "$keys" ${tkeys:+"$tkeys"} --dest "$X"; then done_ok=1; break; fi
            say "$(basename "$f") did not work, trying the next one"
        done
    fi
    if [ "$done_ok" = 0 ]; then
        say "no game files found. Put your NSP or XCI of the base game (1.0.0) and prod.keys in ~/Downloads or ~/Games/AwakeningFrame/game-files, then launch AwakeningFrame (or install it again)"
        exit 3
    fi
fi

# ---- 2. the game code
[ "$BUILD" = 1 ] || exit 4
# (a program without these flags would start the game instead: only one built from this tree has them)
grep -aqF -- --build-game-code "$APP/lasrecomp" || { say "this lasrecomp cannot build the game code by itself: the start screen does it"; exit 4; }
cd "$APP" || exit 1   # (not the data folder: the program would take a folder with extracted/ for a source checkout)
if LARECOMP_DATA_ROOT=$DATA "$APP/lasrecomp" --game-code-status > /dev/null 2>&1; then
    say "game code ready"
    exit 0
fi
FLAG=${AF_HEADSET_FLAG:-$HOME/.cache/headset-in-use}
if [ -e "$FLAG" ]; then
    say "the headset is in use ($FLAG): the game code is built at the next launch instead"
    exit 4
fi
TO=()
[ "$BUDGET" -gt 0 ] 2> /dev/null && TO=(timeout -s TERM -k 60 "$BUDGET")
say "building the game code from your game files$([ "$BUDGET" -gt 0 ] 2> /dev/null && echo " (for up to $((BUDGET / 60)) min; the launch finishes what is left)")"
t0=$(date +%s)
LARECOMP_DATA_ROOT=$DATA "${TO[@]}" nice -n 10 "$APP/lasrecomp" --build-game-code 2>/dev/null | while IFS= read -r l; do
    # (one line per 5 %: the full list is in the data folder's lasrecomp.log)
    case "$l" in
        "build "[0-9]*%*) p=${l#build }; p=${p%%%*}; [ $((p % 5)) = 0 ] && [ "$p" != "${last:-}" ] && { say "$l"; last=$p; } ;;
        *) say "$l" ;;
    esac
done
rc=${PIPESTATUS[0]}
say "game code step took $(( $(date +%s) - t0 )) s (exit $rc)"
if LARECOMP_DATA_ROOT=$DATA "$APP/lasrecomp" --game-code-status > /dev/null 2>&1; then
    say "game code ready"
    exit 0
fi
say "the game code is not finished: the first launch shows the start screen, which finishes it with a progress bar"
exit 4
