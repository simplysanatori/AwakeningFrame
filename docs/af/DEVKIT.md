# The Dev kit

Test builds are made on the desktop, copied to the Frame, and run next to the released version without touching it. The kit
is `~/Games/AwakeningFrame-dev/` on the desktop; the scripts are in `steam-frame/dev/`.

## Make an app folder (desktop)
```
steam-frame/dev/make-dev-app.sh
```
Cross-builds the Arm64 program (`steam-frame/cross-build.sh`) and the Arm64 game code (`make ARCH=aarch64 gen`, then
`lasgame.so`) against the glibc 2.39 root filesystem, and packs `~/Games/AwakeningFrame-dev/app-<commit>/` like the release's app
folder: the program, `lib/`, the launcher, fonts, `lasgame.so` and `lasgame.so.id`. `VERSION` and the program's log say
`<version>-dev-<commit>` (`[info] build ...`). It refreshes the kit's scripts and the DEV art, and builds the mock runtime of the bench.
The game code comes from the owner's game files (`extracted/` in the tree): `generated-aarch64/`, `build/aarch64/` and the app
folder stay out of git and out of every release. The first run takes about half an hour (the game code); later ones minutes.
It holds `~/frame-build/build-af-dev.lock`, runs at nice 19 and uses at most 8 jobs.

The library is accepted without a rebuild on the Frame: its id is `<id of the generator and the flags>-<the game's build id>`, the
same text the program computes (`runtime/gamelib.c`), and the flags are the Makefile's `GENFLAGS`, which `runtime/gamebuild.c` compiles
with too. `lasrecomp --game-code-status` says "game code ready" for it.

## On the headset
```
steam-frame/dev/install-to-frame.sh --owner-ok ~/Games/AwakeningFrame-dev/app-<commit>
```
Over ssh, one connection a step, `FRAME_SSH` (default `ssh frame`) and `FRAME_HOME` to change them: checks that no `lasrecomp` runs, copies the
folder (tar, rate limited) to `~/Games/AwakeningFrame-dev/app.incoming`, checks every file against the sums made here, moves `app` to
`app.bak-<date>` and the new one to `app` (the last two backups are kept), and puts the kit's scripts next to it. `--dry-run` prints the
commands. It writes nothing under `~/Games/AwakeningFrame/` or `~/.local/share/lasrecomp/` and compiles nothing.

Once, on the headset: `~/Games/AwakeningFrame-dev/setup-dev-home.sh` (also run at each launch) and `add-dev-shortcut.py` (the Steam entry
"AwakeningFrame (Dev)", DEV art, the same SteamVR per-app keys as the released entry).

## What is separate
| | Release | Dev |
|---|---|---|
| Program | `~/Games/AwakeningFrame/app` | `~/Games/AwakeningFrame-dev/app` |
| Data folder | `~/.local/share/lasrecomp` | `~/.local/share/lasrecomp-dev`: `extracted/` linked from the release's, `save/`, `larecomp.ini`, `vr_rig.ini` copied once, shader caches built again |
| Settings per launch | `~/Games/AwakeningFrame/session.env` | `~/Games/AwakeningFrame-dev/session.env` (from `session.env.example`) |
| Logs | `~/Games/AwakeningFrame/logs` | `~/Games/AwakeningFrame-dev/logs` |

## Logs
`session.env.example` turns on the cheap logs: the pose log (`pose-<launch time>.log`), `LARECOMP_LOG_PACE`, `LARECOMP_VR_EDGE_LOG`,
`LARECOMP_VR_BACKDROP_LOG`, `LARECOMP_HUD_LOG`; the compositor log is on by default. The A/B switches (`AF_FDM`, `AF_THROTTLE`,
`LARECOMP_VR_CULL_TABLE`, `LARECOMP_VR_CULL`, `LARECOMP_VR_SCENE`, `LARECOMP_VR_HEADROOM`) are commented, one a line. Each launch keeps
`run-`, `lasrecomp-`, `xr_timing-`, `vrtiming-`, `pose-<time>.log` and `session-<time>.env` in `logs/` (the last 10); a game log that no
run copied (a killed launch) is kept at the next launch as `...-unsaved.log`. The run log starts with the version and the commit.

From the desktop: `steam-frame/dev/collect-logs.sh --owner-ok [--sessions N]` reads the last N sessions (default 3), the Dev
`session.env`, `larecomp.ini` and `vr_rig.ini` in one ssh connection, writes them to `~/Games/AwakeningFrame-dev/logs/<date>/` and prints
a line a session (start, length, version, exit status, A/B switches set). It changes nothing on the headset.

## The bench
`frame-bench.sh [--owner-ok] APP_DIR [APP_DIR ...]` runs on the headset without SteamVR: each program in turn under the mock runtime at
120 Hz, in a scratch data folder with a copy of the Dev save, at Mabe Village, the Mysterious Forest, Marin and Tarin's house and Tail
Cave room 04B (15 s each), and writes one file: GPU, render CPU and game CPU ms per game frame, FPS, p95 period, peak memory, and the
difference of each later program to the first. It refuses while `~/.cache/headset-in-use` exists unless `--owner-ok`, runs one program at
a time under `~/.cache/af-bench-lock`, stops each at a time limit, and leaves nothing running.

## Checks
`steam-frame/dev/test-dev-kit.sh` runs the install, setup, log collection, shortcut and bench refusals against a scratch folder that
stands in for the headset (a fake `ssh`, `FRAME_SSH`). It connects to nothing.
