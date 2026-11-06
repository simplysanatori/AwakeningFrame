#!/bin/bash
# Builds the mock OpenXR runtime and the test client:
#   tools/xrmock/build.sh [out dir]      (default build/xrmock of the checkout)
# Writes libxrmock.so and runtime.json (select it with XR_RUNTIME_JSON=<out>/runtime.json)
# and test_client.  Needs clang and the OpenXR headers + loader from tools/fetch_openxr.sh
# (OPENXR_PREFIX, default build/linux/aarch64/deps).  Refuses to run while ~/.cache/headset-in-use exists.
# nice, one job.  Prints the path of runtime.json.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${1:-$REPO/build/xrmock}
PREFIX=${OPENXR_PREFIX:-$REPO/build/linux/aarch64/deps}
[ -f "$PREFIX/include/openxr/openxr.h" ] || { "$REPO/tools/fetch_openxr.sh" >&2; }
[ -f "$PREFIX/include/openxr/openxr.h" ] || { echo "no OpenXR headers in $PREFIX: tools/fetch_openxr.sh" >&2; exit 1; }
if [ ! -f "$OUT/libxrmock.so" ] || [ -n "$(find "$HERE" -newer "$OUT/libxrmock.so" \( -name '*.c' -o -name '*.h' \) | head -1)" ] || [ ! -f "$OUT/test_client" ]; then
  [ -e "$HOME/.cache/headset-in-use" ] && { echo "xrmock: headset in use, not building" >&2; exit 75; }
  mkdir -p "$OUT"
  nice -n 10 clang -O1 -g -Wall -Wextra -Wno-missing-field-initializers -shared -fPIC -fvisibility=hidden -o "$OUT/libxrmock.so" "$HERE/xrmock.c" "$HERE/xrmock_vk.c" -I "$PREFIX/include" -I "$HERE" -lm -lz -lpthread
  nice -n 10 clang -O1 -g -Wall -Wextra -Wno-missing-field-initializers -o "$OUT/test_client" "$HERE/test_client.c" -I "$PREFIX/include" -I "$HERE" -L "$PREFIX/lib" -Wl,-rpath,"$PREFIX/lib" -lopenxr_loader -lvulkan -lm
fi
sed "s#@LIB@#$OUT/libxrmock.so#" "$HERE/runtime.json.in" > "$OUT/runtime.json"
echo "$OUT/runtime.json"
