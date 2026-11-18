#!/bin/bash
# Prepares the Dev build's data folder from the released version's, once. Runs on the headset (run.sh calls it at every
# launch; it only fills in what is missing, so it never overwrites a Dev save or setting).
#   extracted/   a link to the release's: the game files are shared, no copy (the program never writes there)
#   save/, larecomp.ini, vr_rig.ini   copied from the release's if the Dev folder has none yet
#   cache/       not copied: the shader caches are built again for the Dev build
#   session.env  (next to this script) made from session.env.example if missing, with the release's own lines added
# Never writes under the release's folders. AFDEV_DATA / AF_REL_DATA / AF_REL_HOME name other folders (tests).
KIT=$(dirname "$(readlink -f "$0")")
REL=${AF_REL_DATA:-$HOME/.local/share/lasrecomp}
DEV=${AFDEV_DATA:-${LARECOMP_DATA_ROOT:-$HOME/.local/share/lasrecomp-dev}}
RELHOME=${AF_REL_HOME:-$HOME/Games/AwakeningFrame}
say() { echo "$(date '+%F %T') $*"; }
mkdir -p "$DEV"
if [ ! -e "$DEV/extracted" ] && [ ! -L "$DEV/extracted" ]; then
    if [ -d "$REL/extracted" ]; then ln -s "$REL/extracted" "$DEV/extracted" && say "linked extracted/ to the release's"
    else say "no game files in $REL/extracted: nothing to link"; fi
fi
for f in save larecomp.ini vr_rig.ini; do
    if [ ! -e "$DEV/$f" ] && [ -e "$REL/$f" ]; then cp -a "$REL/$f" "$DEV/$f" && say "copied $f from the release"; fi
done
if [ ! -e "$KIT/session.env" ] && [ -f "$KIT/session.env.example" ]; then
    cp "$KIT/session.env.example" "$KIT/session.env"
    if grep -qsE '^[A-Z_]+=' "$RELHOME/session.env"; then
        { echo; echo "# the release's own session.env, as it was when this file was made:"
          grep -E '^[A-Z_]+=' "$RELHOME/session.env"; } >> "$KIT/session.env"
    fi
    say "made session.env"
fi
exit 0
