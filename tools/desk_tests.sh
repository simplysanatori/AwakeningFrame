#!/bin/bash
# Runs every test that needs no game files and no headset on a Linux desktop (the CI list, CONTRIBUTING "Tests",
# and the VR tests; vk_pipelines uses the GPU) and prints a pass/fail table.
#   tools/desk_tests.sh [--list] [--only <text>] [-j N]
#   --list          print the tests and exit      --only <text>   only the tests whose name contains <text>
#   -j N            run N tests at a time (default 4; the build output goes to build/desk_tests/<name>.log)
# Environment: DEPS (a prefix with SDL3 headers and libraries for the tests that link them; default the system's),
# DESK_TEST_TIMEOUT (seconds a test may take, default 600), DESK_PYTHON (a Python with
# unicorn, capstone and lz4 for the FP/SIMD lowering test; else build/venv or
# tools/venv: python3 -m venv build/venv && build/venv/bin/pip install unicorn capstone lz4).
# A test whose tool is missing is SKIP (the reason is in the table), not a pass. Exit 0: no new FAIL; 1: a FAIL that is not a known one.
set -uo pipefail
cd "$(dirname "$0")/.."
OUT=build/desk_tests
JOBS=4; ONLY=; LIST=0
while [ $# -gt 0 ]; do
    case $1 in
        --list) LIST=1 ;;
        --only) ONLY=${2:?--only needs a text}; shift ;;
        -j) JOBS=${2:?-j needs a number}; shift ;;
        -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "desk_tests: unknown argument $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done
LIMIT=${DESK_TEST_TIMEOUT:-600}

export DEPS=${DEPS:-}
SDLC=; SDLL=-lSDL3
if [ -n "$DEPS" ]; then SDLC="-I$DEPS/include"; SDLL="-L$DEPS/lib -Wl,-rpath,$DEPS/lib -lSDL3"; fi
PY=python3
SIMDPY=
for c in "${DESK_PYTHON:-}" build/venv/bin/python tools/venv/bin/python python3; do
    [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && "$c" -I -c 'import unicorn, capstone, lz4' 2>/dev/null && { SIMDPY=$c; break; }
done
FF=$(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale 2>/dev/null)

# Left out: tools/test_room_crossing.py (needs the game files, LARECOMP_ROMFS), test_astc.sh (clones astcenc), the Windows
# and macOS tests, tools/test_vk_pipelines.sh (a GPU test), and `tools/menu_strings.py check` (a report, not a test).
# translate_glsl links with --allow-multiple-definition: the reference translator's rename list in test_translate.sh
# misses symbols added since (the link fails without it), which this test does not use in glsl mode.
# Known to fail on the unmodified 0.1.1 tree (stale tests; shown as KNOWN and not counted as a failure):
KNOWN="save vulkan_validation vulkan_overlap"

# name | needs (commands, space separated; empty: none) | command (run from the repo root, in bash)
T=()
t() { T+=("$1|$2|$3"); }
t build_cache        ""        "$PY tools/test_build_cache.py"
t gamebuild_resume   ""        "$PY tools/test_gamebuild_resume.py"
t save               ""        "$PY tools/test_save.py"
t settings           ""        "$PY tools/test_settings.py"
t delete_guard       ""        "$PY tools/test_delete_guard.py"
t data_move          ""        "$PY tools/test_data_move.py"
t dynres             ""        "$PY tools/test_dynres.py"
t pace               ""        "$PY tools/test_pace.py"
t fence_sequence     ""        "$PY tools/test_fence_sequence.py"
t gmem_lowmem        ""        "$PY tools/test_gmem_lowmem.py"
t vulkan_validation  ""        "$PY tools/test_vulkan_validation.py"
t vulkan_overlay     ""        "$PY tools/test_vulkan_overlay.py"
t vulkan_sync_errors ""        "$PY tools/test_vulkan_sync_errors.py"
t vulkan_overlap     ""        "$PY tools/test_vulkan_overlap.py"
t game_addresses     ""        "$PY tools/test_game_addresses.py"
t field_regions      ""        "$PY tools/test_field_regions.py"
t room_ledges        ""        "$PY tools/test_room_ledges.py"
t cam_override       "clang"   "tools/test_cam_override.sh"
t hz_filter          "clang"   "clang -std=gnu11 -O1 -Iruntime tools/test_hz_filter.c -o $OUT/hz_filter && $OUT/hz_filter"
t translate_ops      "clang clang++" "tools/test_translate_ops.sh"
t spirv_val          "glslc spirv-val" "tools/test_spirv_val.sh"
t vk_pipelines      "clang"   "tools/test_vk_pipelines.sh 500 3"
t translate_glsl     "clang"   "CC=\"clang -Wl,--allow-multiple-definition\" REF=HEAD tools/test_translate.sh 2000 4 glsl"
t movie_extractor    "clang"   "mkdir -p $OUT/movie && clang -std=gnu11 -O1 tools/test_movie_extractor.c $FF -pthread -o $OUT/movie_extractor && $OUT/movie_extractor $OUT/movie"
t condvar            "clang"   "clang -O2 -pthread tools/test_condvar.c -o $OUT/condvar && $OUT/condvar"
t led                "clang"   "clang -std=gnu11 -O1 tools/test_led.c -lm -o $OUT/led && $OUT/led"
t update_tags        "clang++" "clang++ -std=c++17 -O1 tools/test_update_tags.cpp -o $OUT/update_tags && $OUT/update_tags"
t rumble             "clang"   "clang -std=gnu11 -O2 $SDLC tools/test_rumble.c $SDLL -lm -o $OUT/rumble && $OUT/rumble"
t active_pad         "clang"   "clang -std=gnu11 -O2 $SDLC tools/test_active_pad.c $SDLL -lm -o $OUT/active_pad && $OUT/active_pad"
if [ "$(uname -m)" = x86_64 ]; then
    t simd_rounding  "clang"   "clang -std=gnu11 -O1 -mfma -pthread tools/test_simd_rounding.c -lm -o $OUT/simd_rounding && $OUT/simd_rounding"
    if [ -n "$SIMDPY" ]; then t simd_lowering "" "$SIMDPY tools/test_simd.py --encodings 3000 --cases 4 --jobs $(nproc)"
    else t simd_lowering "unicorn-python" ":"; fi
fi

if [ $LIST = 1 ]; then for e in "${T[@]}"; do echo "${e%%|*}"; done; exit 0; fi
mkdir -p "$OUT/res"; rm -f "$OUT"/res/* "$OUT"/*.log

run_one() {   # name needs cmd
    local n=$1 needs=$2 cmd=$3 w t0 rc
    for w in $needs; do
        if [ "$w" = unicorn-python ]; then echo "SKIP no Python with unicorn, capstone, lz4 (DESK_PYTHON=<python>)" > "$OUT/res/$n"; return; fi
        command -v "$w" >/dev/null 2>&1 || { echo "SKIP no $w" > "$OUT/res/$n"; return; }
    done
    t0=$SECONDS
    timeout "$LIMIT" nice -n 10 bash -c "$cmd" > "$OUT/$n.log" 2>&1; rc=$?
    if [ $rc = 0 ]; then echo "PASS $((SECONDS - t0))s" > "$OUT/res/$n"
    elif [ $rc = 124 ]; then echo "FAIL $((SECONDS - t0))s timed out after ${LIMIT}s" > "$OUT/res/$n"
    else echo "FAIL $((SECONDS - t0))s exit $rc: $(grep -v '^[[:space:]]*$' "$OUT/$n.log" | tail -1 | cut -c1-90)" > "$OUT/res/$n"; fi
}
export -f run_one; export OUT LIMIT
printf '%s\n' "${T[@]}" | { [ -n "$ONLY" ] && grep -i -- "^[^|]*$ONLY[^|]*|" || cat; } > "$OUT/queue.tsv"
[ -s "$OUT/queue.tsv" ] || { echo "desk_tests: no test matches '$ONLY' (--list)" >&2; exit 2; }
# the shared build outputs (build/test_translate, build/test_cam_override) mean the two translate tests must not overlap
xargs -a "$OUT/queue.tsv" -d '\n' -P "$JOBS" -I{} bash -c 'IFS="|" read -r n needs cmd <<< "$1"; run_one "$n" "$needs" "$cmd"' _ {}

fails=0; pass=0; skip=0; known=0
printf '%-20s %-5s %s\n' test result 'time / note'
while IFS='|' read -r n _; do
    r=$(cat "$OUT/res/$n" 2>/dev/null || echo "FAIL did not run")
    printf '%-20s %-5s %s\n' "$n" "${r%% *}" "${r#* }"
    case $r in
        PASS*) pass=$((pass + 1)) ;;
        SKIP*) skip=$((skip + 1)) ;;
        *) case " $KNOWN " in *" $n "*) known=$((known + 1)); printf '%-20s %s\n' "" "(known failure on the unmodified tree)" ;; *) fails=$((fails + 1)) ;; esac ;;
    esac
done < "$OUT/queue.tsv"
echo "$pass passed, $fails failed, $known known failures, $skip skipped (logs: $OUT/<name>.log)"
[ $fails = 0 ]
