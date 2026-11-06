#!/bin/bash
# Removes AwakeningFrame's Steam shortcut and program folder. The data folder (your game files, saves, settings
# and the game's logs in ~/.local/share/lasrecomp) and the logs of the launch wrapper stay unless --purge.
#   ./uninstall.sh [--purge]
# Environment: AWAKENINGFRAME_APP, LARECOMP_DATA_ROOT, AWAKENINGFRAME_SKIP_STEAM, AF_CEF_PORT, AF_STEAM_DIR as in install.sh.
# If Steam is not running the shortcut stays; run this again with Steam up, or remove it in Steam.
# SteamVR's per-app settings for the entry are left alone.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$HERE/steam-frame
[ -d "$TOOLS" ] || TOOLS=$HERE
DEST=${AWAKENINGFRAME_APP:-$HOME/Games/AwakeningFrame/app}
DATA=${LARECOMP_DATA_ROOT:-$HOME/.local/share/lasrecomp}
CEFPORT=${AF_CEF_PORT:-8080}
STEAM=${AF_STEAM_DIR:-$HOME/.local/share/Steam}
NAME="AwakeningFrame"
PURGE=0
case "$1" in --purge) PURGE=1 ;; "") ;; *) echo "usage: $0 [--purge]" >&2; exit 2 ;; esac
[ -f "$TOOLS/steam-cef.py" ] || TOOLS=$DEST
if pgrep -f "^$DEST/lasrecomp" > /dev/null 2>&1; then echo "the game is running; quit it first" >&2; exit 1; fi

if [ -n "$AWAKENINGFRAME_SKIP_STEAM" ]; then
  echo "AWAKENINGFRAME_SKIP_STEAM set: Steam left alone"
elif curl -s --max-time 3 "127.0.0.1:$CEFPORT/json" > /dev/null; then
  CEF="python3 -I $TOOLS/steam-cef.py"
  ID=$($CEF "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$NAME' && a.app_type === 1073741824) return a.appid; } return 0; })()" 2> /dev/null || echo 0)
  if [ -n "$ID" ] && [ "$ID" != 0 ]; then
    $CEF "(() => { SteamClient.Apps.RemoveShortcut($ID); return 1; })()" > /dev/null
    echo "removed the '$NAME' shortcut (appid $ID)"
    for cfg in "$STEAM"/userdata/*/config/grid; do   # its art files
      [ -d "$cfg" ] && rm -f "$cfg/${ID}.png" "$cfg/${ID}p.png" "$cfg/${ID}_hero.png" "$cfg/${ID}_logo.png" "$cfg/${ID}_icon.png"
    done
  else
    echo "Steam has no '$NAME' shortcut"
  fi
else
  echo "Steam is not reachable, so the '$NAME' shortcut stays; remove it in Steam, or run this again with Steam up."
fi

if [ -d "$DEST" ]; then rm -rf "$DEST"; echo "removed $DEST"; fi
if [ "$PURGE" = 1 ]; then
  echo "--purge: removing the wrapper's logs and the data folder $DATA (game files, saves, settings, logs)"
  LOGS=$(dirname "$DEST")/logs
  rm -f "$LOGS"/run-*.log "$LOGS"/xr_timing-*.log "$LOGS"/lasrecomp-*.log "$LOGS"/vrtiming-*.log   # (the logs folder has other things too)
  rm -rf "$DATA"
else
  echo "kept your data in $DATA; --purge removes it"
fi
