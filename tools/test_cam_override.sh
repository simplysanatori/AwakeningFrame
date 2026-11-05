#!/bin/bash
# Unit test of the free-camera probe's maths and rewrite (runtime/nvn/cam_override.c): needs no game files or window.
# tools/test_cam_override.sh   (build/test_cam_override is the binary)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
CC=${CC:-clang}
$CC -O1 -g -Wall -Wextra -DCAM_STANDALONE -fsanitize=undefined -o build/test_cam_override tools/test_cam_override.c runtime/nvn/cam_override.c -lm
./build/test_cam_override
