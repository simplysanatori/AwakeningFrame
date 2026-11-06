# VR tour (`tools/vr_tour.sh`)

One command that checks any build of the VR renderer without a headset and writes a short report, so a regression
shows up before anyone puts the Frame on. It runs VR mode (`LARECOMP_XR=1`) under the xrmock mock OpenXR runtime
(`docs/af/TOOLING.md`) from a copy of the test save, warps through seven spots, and collects what the game and the mock
log plus a screenshot of what the game submitted to each eye and to the HUD quad.

```
tools/vr_tour.sh --quick                         # 3 spots, 90 Hz, about 2.6 minutes of runs
tools/vr_tour.sh                                 # 7 spots, 90 and 120 Hz, about 7 minutes of runs
tools/vr_tour.sh --compare build/vr_tour/<older> # run, then compare with an older report
tools/vr_tour.sh --diff <older> <newer>          # compare two reports without a run
tools/vr_tour.sh --tree ~/Games/AwakeningFrame/wt/perf --only cave
```

Needs, in the tree it checks: `build/lasrecomp`, `build/lasgame.so`, `extracted/`, the test save
(`~/.local/share/lasrecomp-smoketest/save`, never written to), the mock (`build/xrmock`, built with
`tools/xrmock/build.sh` when missing, else another worktree's), and Python with PIL (`VR_TOUR_PYTHON`; the tool looks in
`~/Games/AwakeningFrame/venv-tools`). Nothing outside `build/` is written; everything under `build/vr_tour/` is ignored by git.

## Rules it keeps

- `~/.cache/headset-in-use` exists: it refuses (exit 75); `--wait` waits for the flag to go. (`AF_HEADSET_FLAG` and
  `AF_BENCH_LOCK` name other files for the two flags, to test the waiting without touching the real ones.)
- `~/.cache/af-bench-lock` exists, or another `lasrecomp` runs: it waits (polling every 2 s, `--wait-max`, default an
  hour, then exit 75). It then takes the lock itself (the file holds `vr_tour <pid>`), keeps it for the whole run and
  removes it on exit (trap, also on interrupt); the analysis afterwards runs without it.
- `nice -n 5` for the game, `nice -n 10` for the mock's build; no builds. It checks again before each session.

## What a run does

Sessions (one game start each, from a fresh copy of the save in a scratch data root `build/vr_tour/root`, whose
`cache/` is kept between tours and seeded from the tree's own):

| session | what for |
|---|---|
| `shots_<first Hz>` | `MOCK_XR_SHOT=every:<4 s>`: the mock dumps both eye images and the HUD quad every 4 seconds. No walk (a gliding camera makes a dump show the eyes in two states). The dumps stall the loop about 2 s each, so this session's timings are not used. |
| `timing_<Hz>` for each Hz | no dumps: the numbers below. |
| `vvl_<first Hz>` with `--vvl` | the same walk under `tools/vvl/vvl.sh` (`VVL_NO_WSI=1`), validation messages counted per spot. |

Each session: login by scripted input (frames 300-780), `LARECOMP_WARP_AT` to every spot (frame 900 + k times the
dwell in game frames), `LARECOMP_CHEATS=hearts`, a scripted walk in the middle of each spot's time in the timing and
validation sessions (right, left, left, right: sideways and short, because the warp points are at doors and a door of
Link's own starts a level change that the next warp waits for; `--no-walk` drops it),
`MOCK_XR_POSE=file:` (generated: head 1.6 m up, 35 degrees down at the table, yaw +-15 degrees at 0.4 Hz, 4 cm
sideways at 0.25 Hz), `MOCK_XR_HZ`, `MOCK_XR_EYE_SIZE` 1680x1760, `LARECOMP_LOG_PACE=1`, `LARECOMP_FPS_INTERVAL=2`,
`MOCK_XR_TIMING`, `MOCK_XR_TRACE`. It runs in dev mode from the scratch root (the game's stdout then stays a pipe). Below 100 Hz the game runs at 30 fps (the app holds it
there), at 120 Hz at 60; "nominal" below is that. The game's log goes through `tools/vr_tour/stamp.py`, which puts the
wall-clock time on every line, so a spot's window is "from 1 s after the level's `[warp] level-jump request object` line
(loaded; at the latest 4 s after the `[warp] jumping to` line) to the next `jumping to`", and the screenshots (by file
time) and log lines can be matched to it.

Spots (the free-camera sweep's list): Mabe Village (Field_11C), Mysterious Forest (Field_06C), Koholint beach
(Field_09B), Marin and Tarin house, Tail Cave entrance (08D), Tail Cave room 04B, Bottle Grotto room 05D.
`--quick`: Mabe Village, Marin and Tarin house, Tail Cave 04B.

## Output (`build/vr_tour/<timestamp>/`)

- `report.md`: headline (worst verdict), sessions (exit status, xrmock ERROR lines, validation messages, mock-side late and lost
  refreshes, crash), one table row per spot, then the reasons per spot, then the sheet. Verdicts:
  - **BROKEN**: a crash or a session that ran out of time, the warp not taken, no screenshot, a black eye (in every
    screenshot of the spot: mean luma under 1 or 98.5% of pixels black), eyes differing greatly in brightness, swapped eyes,
    the two eyes showing different views in every screenshot, HUD quad empty (under 0.03% of pixels drawn; three hearts
    are 0.17%), any xrmock `ERROR` line or validation message during the spot, or an eye that drew with the game's own
    camera (the executor's camera check, `[stereo] eye N drew M draws with the game's own camera ...`, docs/af/STEREO.md).
  - **SLOW**: at any Hz, game fps under 92% of nominal, sets shown a second under 90% of nominal, more than 2% of refreshes
    missed, three or more game frames over 1.5x the period, or both eyes' GPU time over 90% of the period.
  - **OK** otherwise. Pose mismatches, shader compiles during the window, and "no disparity" are notes, not verdicts.
- `sheet.png`: every spot's eye pair (left | right) and HUD quad, labelled with the verdict and the sets per second.
  Most of an eye image is black: the diorama is a table a metre away and the rest of the view is empty.
- `spots/NN_<spot>_eyes.png` (480 px per eye) and `_hud.png`, `data.json` (all numbers, what `--compare` reads),
  `raw/<session>/` (stamped `run.log`, `timing.csv`, `trace.txt`). The full-size mock PNGs are deleted after the analysis
  (`--keep-raw` keeps them). They are the game's images: never commit any of this.

### Columns

fps = game frames a second from the `[pace]` lines (gameplay frames only); sets/s = `sets shown` of the `[xr] 5 s:` lines
inside the window (`~` = only partly inside); missed % = refreshes missed of those lines; GPU ms e0+e1 = the
`[stereo] GPU per game frame` line (the eye 0 and eye 1 passes); render CPU and busy = `[pace]`; pose mism. = `pose
mismatches X of Y refreshes` of the `[xr]` lines (the executor's eyes against their latch); HUD % = share of the quad's
pixels that are drawn; disp px = the horizontal disparity between the eyes (below).

### Eye checks

Up to four screenshots are analysed per spot (taken at least 5 s after the warp and 1.5 s before the next one). The
one shown is the one whose eyes agree best; a black eye or a disagreement in only some of them is a note ("a
transition?"), because the game camera glides after a warp and a dump can then catch the eyes in two states. The eye
residual is the left/right difference at the best shift over the image's brightness: 0.2-0.3 for a good pair, 0.8 and
more when the eyes show different views; over 0.6 counts as disagreeing.

Left and right come from the two projection views of the layer. Disparity is the shift (in full-size pixels) that makes
the left image agree best with the right image: positive for anything nearer than infinity, as parallel eyes give. A
swapped pair gives a negative number; the verdict only trusts it when the match at that shift is clearly better than at
none. Black eyes, brightness and the HUD share are checked on the downscaled images.

## Reading a report

- At 120 Hz the game's pacer drops to 30 fps when the frames do not fit 60 (both eyes' GPU time over 16.7 ms), so the
  Field spots read SLOW at 120 Hz with a GPU time of 24 ms: that is the renderer's cost, not the tour. A spot after one of
  those starts in the slower mode too (the pacer needs a minute of headroom to go back), so the Marin and Tarin house
  row at 120 Hz can read 54 fps: look at the "period avg / p95" in the per-spot text before blaming that spot.
- `~` after sets/s: only part of the 5 s `[xr]` summary line fell inside the window.
- Shader compiles in a window mean a first visit; the cache in `build/vr_tour/root/cache` is kept, so a second tour
  is cleaner.

## `--compare`

Matches spots by name and flags, per spot: a verdict that got worse; game fps or sets/s down more than 10% at the same Hz;
both eyes' GPU time up 25% (and 0.5 ms); a left or right eye whose black share moved by 35 points, whose mean colour
moved by more than 60 (of 255), whose colour histogram distance exceeds 0.55, or whose 48 px thumbnail differs by a mean
of 80; a flipped disparity sign; a HUD quad that is gone or whose drawn share fell under half (a banner can add to it). The game is not
deterministic and the head sways, so pictures are compared loosely: only black eyes, missing HUD, swapped eyes and the like
are meant to trip it. Exit status 1 when anything is flagged; the result is `compare.md` and a section of `report.md`.

## Limits

- The mock does not model SteamVR's compositor: "OK" means the app's loop and images are sound, not that a headset frame
  fits its deadline. The mock's refresh grid also puts every second (60) or third (30) refresh on a game frame.
- Shader compiles on first visit can make a spot slow once; the second tour (the cache is kept) is the one to trust.
- Screenshots are of what was submitted, at a sway phase that differs from run to run.

## Requests for the overseer (log lines the tour would like; nothing was added to `runtime/`)

- A way to get the `[xr] N s:` line at a chosen interval (say `LARECOMP_XR_STATS_INTERVAL=2`): the fixed 5 s lines
  rarely fall inside a 6-10 s spot window, so sets/s is a partial window for some spots (`~`).
- A `[stereo]` / `[xr]` line when the two eyes of the set that is shown were built from different camera states. During a
  camera glide (the first seconds after a warp, a walk in the Field) a dump showed the left and right eye with clearly
  different zoom, while `pose mismatches 0` and `0 sets with eyes of two frames` stayed 0: either the counters do not
  cover the game camera, or the dump caught the eyes of two sets. The tour only reports it from the pictures.
- A log line with the wall-clock time (or the frame number) on `[warp] jumping to` and on the `[xr] N s:` lines would
  remove the need for `stamp.py`; it is cheap to keep as is.
