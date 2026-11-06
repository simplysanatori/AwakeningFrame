#!/bin/bash
# steam-frame/build-hactool.sh [dest-dir]: builds hactool (SciresM/hactool, ISC, with its bundled mbedtls, Apache 2.0)
# for steam-frame/prepare-game.sh. SteamOS has no Java and no hactool package. Default: ../tools-bin/hactool next to
# the checkout (outside the repo). Needs git, cc and make.
# Pinned to one commit, so a release ships a known hactool. mbedtls is linked in (hactool builds it as static
# libraries); glibc stays the system's (every SteamOS has it, and a static glibc would bring LGPL relinking terms).
# The binary is stripped, and its licence is copied next to it as hactool-LICENSE.txt.
set -euo pipefail
COMMIT=1d64a83450e025622f3468c28fc4164dad2c5ef6
here=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-$here/../tools-bin}
mkdir -p "$dest"
dest=$(cd "$dest" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
git init -q "$W/hactool"
cd "$W/hactool"
git fetch -q --depth 1 https://github.com/SciresM/hactool.git "$COMMIT"
git checkout -q FETCH_HEAD
# (no -g, and no paths of this machine in the binary: the build folder is a temporary one, removed below)
sed "s|^CFLAGS *=.*|CFLAGS = -O2 -std=gnu11 -fPIC -ffile-prefix-map=$W=.|" config.mk.template > config.mk
make -j"$(nproc)" >"$W/build.log" 2>&1 ||
    { tail -20 "$W/build.log" >&2; exit 1; }
strip -o "$dest/hactool" hactool
cp LICENSE "$dest/hactool-LICENSE.txt"
cp mbedtls/apache-2.0.txt "$dest/mbedtls-LICENSE.txt"
echo "$dest/hactool (hactool $(git rev-parse --short HEAD), needs only: $(readelf -d "$dest/hactool" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' '))"
