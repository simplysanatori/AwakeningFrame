# Installing AwakeningFrame on the Steam Frame (demo install)

Local install from a source tree; a release archive with prebuilt binaries comes later (its top-level `install.sh`
is this one, which is what Frametanium's installer mode runs).

## Layout
| What | Where |
|---|---|
| Program: `lasrecomp`, `lasgame.so` (+ `.id`), `awakeningframe.sh`, `steam-cef.py`, `set-art.py`, `vrtiming.py`, `art/` | `~/Games/AwakeningFrame/app` |
| Game files, saves, settings, caches, the game's own logs (`lasrecomp.log`, `xr_timing.log`) | `~/.local/share/lasrecomp` (never copied elsewhere; keys are never used by the installer) |
| Wrapper logs | `~/Games/AwakeningFrame/logs`: `run-<time>.log`, and per session `xr_timing-`, `lasrecomp-`, `vrtiming-<time>.log` (12 sessions kept, `AF_KEEP`) |
| Per-launch settings | `~/Games/AwakeningFrame/session.env` (`VAR=value` lines; Steam launches with no environment of its own) |

## Install
```
./install.sh [--src DIR] [--no-build] [--skip-steam] [--no-art]
```
`--src` is the tree to build and install from (default `~/Games/AwakeningFrame/src`, or `AWAKENINGFRAME_SRC`). The
script refuses to build while `~/.cache/headset-in-use` exists (exit 75), fetches the OpenXR headers / VMA header if the
tree lacks them, runs `make gen` once if there is no game code yet (needs the extracted game in the data folder and
`tools/venv`), runs `make` (ccache if present), checks the binary has the VR mode, and copies each file in place in one
step. Steam must be running (or `--skip-steam`). Run again to update: the shortcut is found by name and kept.

Steam side, as in GalaxyFrame: a non-Steam shortcut "AwakeningFrame" through Steam's CEF port (`steam-cef.py`; appid is
what `AddShortcut` returns, `crc32(exe + name) | 0x80000000`), no Steam restart. SteamVR per-app keys through `vrcmd`
for `steam.app.<appid>.`: `preferredRefreshRate=120` (SteamVR does not offer 120 Hz through the OpenXR refresh-rate
extension; this key is what sets it), `framesToThrottle=0`, `additionalFramesToPredict=0`, `motionSmoothingOverride=2`
(no motion smoothing). The wrapper sets the same four at every launch, because SteamVR drops them at times; they need
SteamVR running. Art (placeholders, `steam-frame/art`): grid files for the shortcut's user, set live with
`SetCustomArtworkForApp`, dock icon `app/art/icon.png`.

## Launch
From the Steam library with SteamVR running. The wrapper: VR on (`LARECOMP_XR=1`, `LARECOMP_XR_HZ=120`), `--play` (no start
screen) when the game code and files are there, `LARECOMP_DATA_ROOT` set, `~/.cache/headset-in-use` held while the game
runs and removed on exit or when Steam stops it.

Options: `awakeningframe.sh --flat` (no VR, no SteamVR keys, no marker: debugging), `--launcher` (start screen).
Settings in `session.env`: `AF_REFRESH_HZ` (72, 90, 120; default 90, or the VR tab's choice), `AF_FPS=60` (a test: 60 fps at 90 Hz), `AF_VRTIMING=0`, `AF_KEEP`, and any `LARECOMP_*` variable
(`LARECOMP_XR_EYE_SCALE`, `LARECOMP_XR_QUAD`).

## Uninstall
`./uninstall.sh [--purge]`: removes the shortcut, its art and `~/Games/AwakeningFrame/app`; `--purge` also the wrapper's
logs and the data folder (game files, saves).

## Checks
- `steam-frame/test-install.sh`: install, update, refusals, wrapper (flags, environment, TERM) and uninstall against a
  scratch folder, scratch Steam folder, a stand-in for Steam's debugging port and a stand-in `vrcmd`. Touches nothing real.
- `steam-frame/privacy-check.sh`: personal data in what this branch added (see its header; `AF_PRIVACY_PATTERNS`).
- `steam-frame/art/gen-art.py` regenerates the placeholder art (original shapes and the name only).
