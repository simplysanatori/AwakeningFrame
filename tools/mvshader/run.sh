#!/bin/bash
# Offline check of the multiview shader variants on the recorded shader corpus (tools/mvshader/harness.c).
#   tools/mvshader/run.sh [list or log ...]
# Defaults: the shipped list, the player's cache/shader_seen.txt and their logs (the "[shader] record" lines). The
# reference translator is REF (default: the steam-frame branch's runtime/nvn/maxwell). Needs shaderc, SPIRV-Tools
# and lz4 (system libraries) and the game files (extracted/romfs, or ROMFS=...). Failing shaders go to
# build/mvshader/fail/. MVH_THREADS (3): compile threads; MVH_BASE_COMPILE=1 also compiles the mv = 0 shaders;
# MVH_NOPUSH=1: no promoted push words (both builds), and the multiview GLSL (no twins) minus its multiview parts compared
# with the mv = 0 GLSL; MVH_NOCOMPILE=1: translations only. MVH_YIELD=1: also pause while a lasrecomp process runs (or ~/.cache/headset-in-use exists). It runs at idle priority.
set -euo pipefail
cd "$(dirname "$0")/../.."
[ -e "$HOME/.cache/headset-in-use" ] && { echo "headset in use: not now" >&2; exit 1; }
REF=${REF:-refs/heads/steam-frame}
ROMFS=${ROMFS:-extracted/romfs}
DATA=${LARECOMP_DATA:-$HOME/.local/share/lasrecomp}
B=build/mvshader
CC=${CC:-cc}
mkdir -p "$B/ref" "$B/inc"
rm -rf "$B/fail" && mkdir -p "$B/fail"
for f in translate.c shader.h; do git show "$REF:runtime/nvn/maxwell/$f" > "$B/ref/$f.new"; cmp -s "$B/ref/$f.new" "$B/ref/$f" || mv "$B/ref/$f.new" "$B/ref/$f"; rm -f "$B/ref/$f.new"; done
(LC_ALL=C tr -d '\r' < runtime/nvn/shader_variants.txt | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$/\\n"/'; echo '""') > "$B/inc/shader_variants.inc"
OPT="-O2 -g -fPIC -w"  # (the translator's own warnings are the build's business)
$CC $OPT -shared -Wl,-Bsymbolic -I"$B/ref" -o "$B/ref.so" "$B/ref/translate.c" tools/mvshader/wrap.c -lm
$CC $OPT -shared -Wl,-Bsymbolic -DHAVE_MV -Iruntime/nvn/maxwell -o "$B/new.so" runtime/nvn/maxwell/translate.c tools/mvshader/wrap.c -lm
$CC $OPT -Iruntime -Iruntime/nvn -I"$B/inc" -o "$B/harness" tools/mvshader/harness.c runtime/nvn/shader_prep.c \
    -lshaderc_shared -lSPIRV-Tools-shared -llz4 -ldl -lpthread
$CC $OPT -Iruntime/nvn/maxwell -o "$B/unit" tools/mvshader/unit.c -lshaderc_shared -lSPIRV-Tools-shared -lm
if [ -z "${MVH_BUILD_ONLY:-}" ]; then echo "unit (texture forms on array twins):"; chrt --idle 0 "$B/unit"; fi
if [ $# -eq 0 ]; then
    set -- runtime/nvn/shader_variants.txt
    [ -f "$DATA/cache/shader_seen.txt" ] && set -- "$@" "$DATA/cache/shader_seen.txt"
    for l in "$DATA"/lasrecomp*.log; do [ -f "$l" ] && set -- "$@" "$l"; done
fi
[ -n "${MVH_BUILD_ONLY:-}" ] && exit 0
chrt --idle 0 nice -n 19 "$B/harness" "$ROMFS" "$B/ref.so" "$B/new.so" "$@" -o "$B/fail" &
H=$!
# (MVH_YIELD=1) stopped while the game runs (a bench or a headset session on this machine), resumed after
if [ "${MVH_YIELD:-0}" = 1 ]; then
    ( stopped=0; while kill -0 $H 2>/dev/null; do
        if pgrep -x lasrecomp >/dev/null || [ -e "$HOME/.cache/headset-in-use" ]; then
            [ $stopped = 0 ] && kill -STOP $H 2>/dev/null && stopped=1 && echo "[mvshader] paused: the game runs" >&2
        elif [ $stopped = 1 ]; then kill -CONT $H 2>/dev/null; stopped=0; echo "[mvshader] resumed" >&2; fi
        sleep 2; done ) &
fi
wait $H
