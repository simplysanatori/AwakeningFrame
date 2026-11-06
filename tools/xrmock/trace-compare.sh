#!/bin/bash
# Compares the call traces (MOCK_XR_TRACE) of two runs against the mock runtime: what the app does
# with the session, swapchains and frames.  Frame numbers and swapchain image indices are
# dropped (their rotation depends on how long the start took), session events too; the setup
# lines (up to the first xrWaitFrame) are compared as they are, then the steady-state frames
# from the second xrWaitFrame (the first often carries one-time work), up to the length
# of the shorter run less 8 lines.
#   tools/xrmock/trace-compare.sh trace_a.txt trace_b.txt
#   IGNORE_EXT=XR_FB_display_refresh_rate tools/xrmock/trace-compare.sh a.txt b.txt   (extension names in one build only)
# Produce a trace with:  XR_RUNTIME_JSON=build/xrmock/runtime.json MOCK_XR_TRACE=a.txt <app>
# Exit status 0 when the setup and the frames match.
set -e
A=${1:?trace A}; B=${2:?trace B}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
norm() { # trace out-prefix
  sed 's/^f[0-9]* //; s/ idx=[0-9]*//' "$1" | grep -v '^event' > "$2.norm"
  if [ -n "$IGNORE_EXT" ]; then
    for e in ${IGNORE_EXT//,/ }; do sed -i "/^createInstance/s/ $e\\b//" "$2.norm"; done
  fi
  awk '/^waitFrame/ {exit} {print}' "$2.norm" > "$2.setup"
  # the frames, from the second waitFrame (the first one often carries one-time work)
  awk '/^waitFrame/ {n++} n >= 2 {print}' "$2.norm" > "$2.frames"
}
norm "$A" "$T/a"; norm "$B" "$T/b"
status=0
if ! diff "$T/a.setup" "$T/b.setup" > "$T/setup.diff"; then echo "SETUP DIFFERS"; head -n 10 "$T/setup.diff"; status=1; fi
la=$(wc -l < "$T/a.frames"); lb=$(wc -l < "$T/b.frames")
n=$((la < lb ? la : lb)); n=$((n > 8 ? n - 8 : n))
if [ "$n" -lt 20 ]; then echo "traces too short ($n frame lines)"; exit 1; fi
if diff <(head -n "$n" "$T/a.frames") <(head -n "$n" "$T/b.frames") > "$T/frames.diff"; then
  echo "frames identical over $n lines ($(grep -c '^endFrame' "$T/a.frames") / $(grep -c '^endFrame' "$T/b.frames") frames)"
else
  echo "FRAMES DIFFER"; head -n 10 "$T/frames.diff"; status=1
fi
exit $status
