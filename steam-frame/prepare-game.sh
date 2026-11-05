#!/bin/bash
# steam-frame/prepare-game.sh <game.nsp|game.xci> <prod.keys> [title.keys] [--dest DIR] [--force]
#
# Turns your own dump of Zelda: Link's Awakening (Switch, 2019) into the extracted/ folder LAS Recomp reads:
#
#     extracted/exefs/   main, main.npdm, sdk, ...    (what `make gen` and the start screen read)
#     extracted/romfs/   region_common/, ...
#
# No Java and no extra tools: it uses hactool (https://github.com/SciresM/hactool), found in $HACTOOL, next to this
# script's project (../../tools-bin/hactool, where steam-frame/BUILD-frame.md builds it), or in PATH.
#
# Needs your own keys: prod.keys from your console (Lockpick_RCM). Retail NSPs also carry their ticket, so title.keys is
# usually not needed; give it when the NSP has no .tik file (stripped dumps). The keys are copied to a private temporary
# folder for hactool and removed again; nothing of them or of the game is written anywhere else.
#
# Where it writes: --dest DIR (the extracted/ folder itself), else $LARECOMP_DATA_ROOT/extracted, else
# ${XDG_DATA_HOME:-~/.local/share}/lasrecomp/extracted (what the Linux build uses). For a source checkout that has its
# own extracted/ ("dev mode", make gen), use --dest <checkout>/extracted. An existing destination is kept unless --force.
#
# Checks: the package must hold the base game's Program NCA (update-only packages, DLC and compressed .nsz/.xcz are
# refused); the main program's build ID (exefs/main, bytes 0x40..0x53) must be one runtime/game_addresses/versions.h
# lists. 1.0.0 is the preferred version.
set -euo pipefail

me=$(basename "$0")
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
versions_h=$repo/runtime/game_addresses/versions.h

die() { echo "$me: $*" >&2; exit 1; }
usage() {
    sed -n '2,/^set -euo/p' "$0" | sed -e '/^set -euo/d' -e 's/^# \{0,1\}//' >&2
    exit "${1:-2}"
}

# ---- arguments
src= keys= tkeys= dest= force=0
pos=()
while [ $# -gt 0 ]; do
    case $1 in
        -h|--help) usage 0 ;;
        --dest) [ $# -ge 2 ] || die "--dest needs a folder"; dest=$2; shift 2 ;;
        --dest=*) dest=${1#--dest=}; shift ;;
        --force) force=1; shift ;;
        --) shift; pos+=("$@"); break ;;
        -*) die "unknown option $1 (see --help)" ;;
        *) pos+=("$1"); shift ;;
    esac
done
[ ${#pos[@]} -ge 2 ] && [ ${#pos[@]} -le 3 ] || usage
src=${pos[0]} keys=${pos[1]} tkeys=${pos[2]:-}

case ${src,,} in
    *.nsp|*.xci) ;;
    *.nsz|*.xcz) die "$src is compressed (NSZ/XCZ): decompress it to .nsp/.xci first" ;;
    *) die "$src: expected a .nsp or .xci file" ;;
esac
[ -f "$src" ] || die "$src: no such file"
[ -r "$src" ] || die "$src: not readable"
[ -f "$keys" ] || die "$keys: no such file (prod.keys from your console, e.g. dumped with Lockpick_RCM)"
grep -q '^master_key_00[[:space:]]*=' "$keys" || grep -q '^header_key[[:space:]]*=' "$keys" ||
    die "$keys doesn't look like a prod.keys file (no header_key / master_key_00 line)"
if [ -n "$tkeys" ]; then [ -f "$tkeys" ] || die "$tkeys: no such file"; fi
[ -f "$versions_h" ] || die "$versions_h not found: run this from the LAS Recomp checkout"

hactool=${HACTOOL:-}
if [ -z "$hactool" ]; then
    for c in "$repo/../tools-bin/hactool" "$HOME/Games/AwakeningFrame/tools-bin/hactool"; do
        [ -x "$c" ] && { hactool=$c; break; }
    done
fi
[ -n "$hactool" ] || hactool=$(command -v hactool || true)
[ -n "$hactool" ] && [ -x "$hactool" ] || die "hactool not found (set HACTOOL=, or build it: see steam-frame/BUILD-frame.md)"
command -v python3 >/dev/null || die "python3 is needed (ticket parsing)"

if [ -z "$dest" ]; then
    root=${LARECOMP_DATA_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/lasrecomp}
    dest=$root/extracted
fi
dest=${dest%/}
if [ -e "$dest/exefs/main" ] && [ "$force" != 1 ]; then
    die "$dest already holds game files (exefs/main): use --force to replace them, or --dest elsewhere"
fi
mkdir -p "$(dirname "$dest")"

# ---- private workspace (keys live here only for the run)
umask 077
work=$(mktemp -d "$(dirname "$dest")/.prepare-game.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/home/.switch" "$work/pfs" "$work/out"
cp "$keys" "$work/home/.switch/prod.keys"
: > "$work/home/.switch/title.keys"
[ -n "$tkeys" ] && cat "$tkeys" >> "$work/home/.switch/title.keys"
export HOME=$work/home
unset XDG_CONFIG_HOME
hact() { "$hactool" --disablekeywarns --suppresskeys "$@"; }

echo "package: $src ($(du -h "$src" | cut -f1))"

# ---- unpack the package (NSP = PFS0, XCI = a HFS0 tree: its secure partition holds the NCAs)
case ${src,,} in
    *.nsp) hact -t pfs0 --pfs0dir="$work/pfs" "$src" >"$work/log.pfs" 2>&1 ||
               { tail -5 "$work/log.pfs" >&2; die "$src isn't a readable NSP (PFS0)"; } ;;
    *.xci) hact -t xci --securedir="$work/pfs" "$src" >"$work/log.pfs" 2>&1 ||
               { tail -5 "$work/log.pfs" >&2; die "$src isn't a readable XCI"; } ;;
esac
shopt -s nullglob nocaseglob
ncas=("$work"/pfs/*.nca)
tiks=("$work"/pfs/*.tik)
shopt -u nocaseglob
[ ${#ncas[@]} -gt 0 ] || die "no .nca files inside $src"
echo "found: ${#ncas[@]} NCA files, ${#tiks[@]} ticket(s)"

# tickets -> title.keys lines (rights id = encrypted title key; hactool decrypts it with the titlekek from prod.keys).
# Common-key tickets only hold the key in the clear; a personalised ticket's is RSA-wrapped: give title.keys then.
if [ ${#tiks[@]} -gt 0 ]; then
    python3 -I - "${tiks[@]}" >>"$work/home/.switch/title.keys" <<'PY'
import struct, sys
BODY = {0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80, 0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}  # signature block sizes
for path in sys.argv[1:]:
    d = open(path, "rb").read()
    if len(d) < 0x2C0:
        continue
    sigtype = struct.unpack_from("<I", d, 0)[0]
    if sigtype not in BODY:
        continue
    body = BODY[sigtype]
    if d[body + 0x141] != 0:              # titlekey_type 1: personalised (RSA-wrapped), not usable here
        sys.stderr.write("note: %s is a personalised ticket: pass title.keys\n" % path.rsplit("/", 1)[-1])
        continue
    print("%s = %s" % (d[body + 0x160:body + 0x170].hex(), d[body + 0x40:body + 0x50].hex()))
PY
fi

# ---- find the Program NCA of the base game
prog= ptid= nprog=0
for f in "${ncas[@]}"; do
    case ${f,,} in *.cnmt.nca) continue ;; esac
    if hact -t nca "$f" >"$work/log.nca" 2>&1 && grep -q '^Content Type:' "$work/log.nca"; then
        ctype=$(sed -n 's/^Content Type: *//p' "$work/log.nca" | head -1)
        tid=$(sed -n 's/^Title ID: *//p' "$work/log.nca" | head -1)
        printf '  %-40s %-10s title id %s\n' "$(basename "$f")" "$ctype" "$tid"
        if [ "$ctype" = Program ]; then
            nprog=$((nprog + 1))
            low=$((16#${tid: -3}))
            if [ "$low" = $((16#800)) ]; then
                die "this is an update package (title id $tid ends in 800), not the base game: it holds only the changed files. Use the base game's NSP/XCI (1.0.0 preferred)"
            fi
            if [ "$low" != 0 ]; then
                die "Program NCA with title id $tid isn't a base game (base ids end in 000)"
            fi
            prog=$f ptid=$tid
        fi
    elif grep -qi 'rights id\|titlekey' "$work/log.nca"; then
        :
    else
        echo "  $(basename "$f"): could not be read (wrong keys?)" >&2
    fi
done
if [ "$nprog" = 0 ]; then
    if grep -qi 'Unable to match rights id' "$work/log.nca" 2>/dev/null; then
        die "the game's NCAs are title-key encrypted and no matching title key was found: pass title.keys as the third argument"
    fi
    die "no Program NCA found in $src (wrong keys, or not the game: DLC and updates alone don't contain it)"
fi
[ "$nprog" = 1 ] || die "$nprog Program NCAs in one package: not a single base game"
[ "${ptid,,}" = 01006bb00c6f0000 ] || echo "note: title id $ptid is not the Link's Awakening (2019) one I know (01006bb00c6f0000); the build ID check below decides"

# ---- extract exefs + romfs
echo "extracting the Program NCA (exefs, romfs) ..."
hact -t nca --exefsdir="$work/out/exefs" --romfsdir="$work/out/romfs" "$prog" >"$work/log.x" 2>&1 ||
    { grep -iE 'error|warn|unable' "$work/log.x" | head -5 >&2; die "extraction failed (wrong keys? a key generation newer than this hactool?)"; }
[ -f "$work/out/exefs/main" ] || die "no exefs/main came out of the NCA (key problem?)"
[ -f "$work/out/exefs/main.npdm" ] || die "no exefs/main.npdm came out of the NCA"
[ -d "$work/out/romfs/region_common" ] || die "no romfs/region_common came out of the NCA (an update-style NCA, or a key problem)"

# ---- the build ID: bytes 0x40..0x53 of exefs/main (the NSO's module ID)
[ "$(stat -c %s "$work/out/exefs/main")" -ge 96 ] || die "exefs/main is too small to be an NSO"
[ "$(head -c 4 "$work/out/exefs/main")" = NSO0 ] || die "exefs/main isn't an NSO (magic is not NSO0)"
bid=$(dd if="$work/out/exefs/main" bs=1 skip=64 count=20 2>/dev/null | od -An -v -tx1 | tr -d ' \n' | tr a-f A-F)
label= key=
while IFS='|' read -r k l b; do
    [ "${b^^}" = "$bid" ] && { label=$l key=$k; break; }
done < <(sed -n 's/^GA_VERSION(\([A-Za-z0-9_]*\), *"\([^"]*\)", *"\([0-9A-Fa-f]*\)").*/\1|\2|\3/p' "$versions_h")
echo "main build ID: $bid"
if [ -z "$label" ]; then
    echo "supported build IDs (runtime/game_addresses/versions.h):" >&2
    sed -n 's/^GA_VERSION([A-Za-z0-9_]*, *"\([^"]*\)", *"\([0-9A-Fa-f]*\)").*/  \2  \1/p' "$versions_h" >&2
    die "this game version isn't supported (no address tables for build ID $bid)"
fi
rel=$(sha256sum "$work/out/exefs/main.npdm" | cut -d' ' -f1)
relname=$(sed -n "s/^GA_RELEASE($key, *\"\([^\"]*\)\", *\"$rel\").*/\1/p" "$versions_h")
echo "game version: ${relname:-$label} (table set: $label)"
[ "$key" = v1_0_0 ] || echo "note: 1.0.0 is the preferred version; this one is supported too"

# ---- publish: replace dest only after everything above worked
if [ -e "$dest" ]; then
    [ "$force" = 1 ] || die "$dest exists"
    for e in exefs romfs; do rm -rf "${dest:?}/$e"; done
else
    mkdir -p "$dest"
fi
mv "$work/out/exefs" "$dest/exefs"
mv "$work/out/romfs" "$dest/romfs"
nfiles=$(find "$dest/romfs" -type f | wc -l)
echo "done: $dest (exefs + romfs, $nfiles romfs files, $(du -sh "$dest" | cut -f1))"
