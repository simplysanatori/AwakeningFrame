#!/bin/bash
# Makes the Dev entry's library art from the images in steam-frame/art/: each gets a yellow DEV banner, so it is never
# taken for the released AwakeningFrame. ImageMagick (magick or convert) and the DejaVu font of the tree.
#   steam-frame/dev/make-dev-art.sh OUTDIR      (p.png wide.png hero.png logo.png icon.png; kept outside git)
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${1:?usage: make-dev-art.sh OUTDIR}
IM=magick; command -v magick > /dev/null || IM=convert
command -v "$IM" > /dev/null || { echo "make-dev-art: no ImageMagick (magick or convert)" >&2; exit 1; }
FONT=$PWD/third_party/fonts/DejaVu/DejaVuSans-Bold.ttf
mkdir -p "$OUT"
# banner NAME CORNER-HEIGHT-PERCENT: a band across the bottom (height as a share of the image) with DEV on it
banner() {
    local name=$1 pct=$2 w h bh
    [ -f "steam-frame/art/$name.png" ] || { echo "make-dev-art: no steam-frame/art/$name.png" >&2; return 1; }
    read -r w h < <($IM identify -format '%w %h\n' "steam-frame/art/$name.png")
    bh=$((h * pct / 100))
    $IM "steam-frame/art/$name.png" \
        -fill '#f2c200' -draw "rectangle 0,$((h - bh)) $w,$h" \
        -font "$FONT" -fill '#101010' -gravity south -pointsize $((bh * 75 / 100)) -annotate +0+$((bh / 10)) 'DEV' \
        "$OUT/$name.png"
}
banner p 14
banner wide 22
banner hero 18
banner logo 20
banner icon 24
echo "$OUT: $(ls "$OUT" | tr '\n' ' ')"
