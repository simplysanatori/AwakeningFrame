#!/bin/bash
# "AwakeningFrame (Dev)": the test build in app/, with its own data folder, session.env and logs, so the released
# version's saves, settings and shader caches are never touched. This is what the Dev Steam entry runs.
#   data folder   ~/.local/share/lasrecomp-dev (AFDEV_DATA); made by setup-dev-home.sh at each launch (once in effect)
#   settings      session.env next to this script; logs in logs/ next to it (the wrapper's: run-, lasrecomp-, xr_timing-,
#                 vrtiming-, pose-<time>.log and session-<time>.env per launch, the last 10 of each)
HERE=$(dirname "$(readlink -f "$0")")
export LARECOMP_DATA_ROOT=${AFDEV_DATA:-$HOME/.local/share/lasrecomp-dev}
export AF_KEEP=${AF_KEEP:-10}
mkdir -p "$HERE/logs"
"$HERE/setup-dev-home.sh" >> "$HERE/logs/setup.log" 2>&1
exec "$HERE/app/awakeningframe.sh" "$@"
