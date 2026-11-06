#!/bin/bash
# Streaming lane test session (docs/af/STREAM.md): one headless game run from a scratch copy of the test save, with the
# given extra environment; the log goes to <out>/game.log. Research artefacts stay outside the repo.
#   tools/stream_run.sh <out dir> <last frame> [VAR=value ...]
# Defaults: LARECOMP_WARP_ON_LOAD=Field:Field_11C (override in the arguments), hearts cheat, title input of the smoketest.
# STREAM_BIN=<checkout> runs another checkout's build (default: this one). Waits while ~/.cache/headset-in-use or
# ~/.cache/af-bench-lock exists or another lasrecomp runs.
set -uo pipefail
REPO=${STREAM_BIN:-$(cd "$(dirname "$0")/.." && pwd)}
O=$1 LAST=$2; shift 2
SAVE=${LARECOMP_SMOKE_SAVE:-$HOME/.local/share/lasrecomp-smoketest/save}
# wait (poll every 2 min, up to an hour) while the headset is in use, the perf lane benchmarks (~/.cache/af-bench-lock)
# or another lasrecomp runs
busy() { [ -e "$HOME/.cache/headset-in-use" ] || [ -e "$HOME/.cache/af-bench-lock" ] || pgrep -x lasrecomp >/dev/null; }
for i in $(seq 1 30); do busy || break; echo "stream_run: headset in use / bench lock / another lasrecomp: waiting" >&2; sleep 120; done
busy && { echo "stream_run: still busy: not running" >&2; exit 75; }
mkdir -p "$O" && O=$(cd "$O" && pwd)
rm -rf "$O/save" && cp -R "$SAVE" "$O/save" && : > "$O/settings.ini"
IN=$(for f in $(seq 600 100 6000); do printf '%d:L:12,%d:R:12,%d:A:6,' $f $f $((f + 50)); done)
cd "$REPO" || exit 1
timeout "${STREAM_TIMEOUT:-1200}" env SDL_VIDEODRIVER=offscreen ENABLE_GAMESCOPE_WSI=0 LARECOMP_NO_LAUNCHER=1 LARECOMP_NO_AUDIO=1 \
    LARECOMP_NO_UPDATE_CHECK=1 LARECOMP_SETTINGS="$O/settings.ini" LARECOMP_SAVE_DIR="$O/save" LARECOMP_CHEATS=hearts \
    LARECOMP_INPUT="${STREAM_INPUT:-${IN%,}}" LARECOMP_WARP_ON_LOAD=Field:Field_11C LARECOMP_WINDOW_SHOT="$LAST" \
    LARECOMP_SHOT_PATH="$O/last.png" LARECOMP_EXIT_AFTER_SHOT=1 "$@" nice ./build/lasrecomp extracted > "$O/game.log" 2>&1
echo "stream_run: exit $? ($O/game.log)"
