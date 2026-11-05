#!/bin/bash
# steam-frame/build-hactool.sh [dest-dir]: builds hactool (SciresM/hactool, with its bundled mbedtls) from source for
# steam-frame/prepare-game.sh. SteamOS has no Java and no hactool package. Default: ../tools-bin/hactool next to the
# checkout (outside the repo). Needs git, gcc (or cc) and make.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-$here/../tools-bin}
mkdir -p "$dest"
dest=$(cd "$dest" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
git clone -q --depth 1 https://github.com/SciresM/hactool.git "$W/hactool"
cd "$W/hactool"
cp config.mk.template config.mk
make -j"$(nproc)" >"$W/build.log" 2>&1 || { tail -20 "$W/build.log" >&2; exit 1; }
cp hactool "$dest/hactool"
echo "$dest/hactool ($(git rev-parse --short HEAD))"
