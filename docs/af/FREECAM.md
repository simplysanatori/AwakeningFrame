# Free-camera probe (AwakeningFrame lane 1)

The headset-free GO / NO-GO test for the diorama design: on the flat window, replace the game's camera with an orbiting
or pulled-back one and look at whether the world holds up from other angles (geometry present, lighting, shadows, water
and fog plausible) and which shaders use camera data outside the camera buffer. Design: `~/.claude/plans/awakeningframe-vr-design.md` section 7.

Status: implemented and unit-tested; **never run against the game** (no game files when it was written). Items marked
**[G]** are guesses the first real run has to check.

## What it does

The game's main camera is the first perspective 320-byte vertex uniform buffer of a frame (guest cbuf slot 5 = hardware
cbuf 8 = GLSL binding 8). Floats 0-15 are the view, 16-31 the projection, 32-47 the view-projection, row-major, column
vectors (`clip = P * V * x`, GL depth [-1, 1], view looks down -z, projection row 3 = (0, 0, -1, 0)).

- `exec.c` `cbuf_info`: for any bound range that is the latched camera's address with unchanged matrices, bind a host
  copy (`vk_transient_copy`) with floats 0-47 replaced. Guest memory is never written.
- `exec.c` draw path, next to `widescreen_scene_ubo`: `cam_override_latch` (the frame's camera) and, with the log on,
  `cam_override_draw` (what each draw binds).
- `widescreen.c`: `rt_cull_aspect`, `rt_fovy`, `rt_skysea_margin` take the culling override; `widescreen_frame_end`
  runs the schedule and publishes Link's position and the zone box.
- `runtime/nvn/cam_override.c/.h`: everything else (backend neutral; the stereo executor reuses `cam_override_latch` /
  `cam_override_ubo` / `cam_build`). Off, the cost is one cached flag test at each hook.

## Environment variables

| Variable | Meaning |
|---|---|
| `LARECOMP_FREECAM=identity` | V, P as the game has them, VP rebuilt as P*V. Must give a bit-identical image |
| `=passthrough` | the game's 320 bytes verbatim through the same path (tests the plumbing alone) |
| `=orbit:<yaw>:<pitch>:<dist>[:<fovy>]` | yaw 0 = from +z (south, the game's own side), degrees; pitch above the horizon; dist in game units from the pivot; fovy degrees (default: the game's own) |
| `=spin:<deg/frame>:<pitch>:<dist>` | yaw = rate * frame |
| `LARECOMP_FREECAM_PIVOT=screen\|link` | screen = centre of the screen on the floor (y 0; side-view zones, thinner than 6 in z: the zone's middle plane); link = Link's PlayerCollision position (falls back to screen) |
| `LARECOMP_FREECAM_CULL=<aspect>:<fovy scale>` | widened CPU culling: `rt_cull_aspect` returns the absolute aspect (times the game's own per-zone ratio), `rt_fovy` multiplies the angle. `stock` / unset = the game's |
| `LARECOMP_FREECAM_AT=<frame>:<mode>[/<cull>],...` | schedule (present frame numbers). `<mode>` is any FREECAM value or `off`; `<cull>` as above (`stock` when omitted: the env value) |
| `LARECOMP_FREECAM_FAR=<units>` | push the far plane out (default 0: the game's depth terms kept bit for bit) |
| `LARECOMP_FREECAM_FIX48=1` | also rewrite camera-derived blocks recognised in floats 48-79 (inverse matrices, eye / forward / right / up) for the substitute camera |
| `LARECOMP_FREECAM_LOG=1`, `_LOG_EVERY=<n>` | probe log, full scans every n frames (60) |

Orbit keeps the game's projection (depth terms, aspect) unless `fovy` is given or the culling fovy scale is on (then the
orbit angle is the game's angle divided by the scale, so widened culling doesn't change the picture). The aspect is the
game's own (P11 / P00), which is the output's. Pitch is clamped to +-89 degrees.

## Log (`LARECOMP_FREECAM_LOG=1`, lines start `[freecam]`)

Every n frames: the previous frame's draw count; the camera (eye, forward, near, far, fovy, aspect); the buffer's floats
48-79 as 8 rows of 4; what 48-63 and 64-79 turned out to be (view, projection, view-projection, their inverses, each
also transposed; or "all zero" / "unknown") and which 4-float groups equal the eye position, forward, right or up; and
whether the rebuilt P*V equals the game's VP bit for bit (plain or fused product, how many frames each).
Every draw of a scan frame, once per distinct case (a table of keys, so the log stays short):

- `SUSPECT matrix`: a UBO other than the main camera holds a 4x4 equal (1e-4 relative) to V, P, VP or an inverse, or the
  transpose; also "rows 0-2 only" for a 3x4 copy. Fields: fragment and vertex shader hash (as `LARECOMP_SKIP_FSHASH`
  takes; vertex hashed the same way over the code at +0x30, 0x200 bytes), stage, cbuf number, offset.
- `SUSPECT other camera`: another 320-byte perspective buffer (reflection, cube, mirror cameras), or the main address
  with other contents (rewritten mid-frame: left unsubstituted).
- `SUSPECT screen-space`: a draw into a 16:9 scalable colour target that samples a depth texture and binds no main camera.
- `main camera bound at stage s cbuf k`: the main camera bound anywhere but the vertex stage's slot 5 (still substituted).

Bisect a suspect with `LARECOMP_SKIP_FSHASH=<fs hash>` (exec.c); a vertex-stage-only suspect needs a skip by fragment
hash of the same draws or a look at the shader with `LARECOMP_LOG_FSHASH`.

## Running the sweep (needs the game)

```
steam-frame/prepare-game.sh game.nsp prod.keys --dest $PWD/extracted     # once (see BUILD-frame.md)
~/Games/AwakeningFrame/afbuild.sh                                        # make gen + make; refuses while ~/.cache/headset-in-use exists
tools/test_cam_override.sh                                               # unit test, no game files
tools/freecam_sweep.sh --plan                                            # what it will do, no game files needed
tools/freecam_sweep.sh --identity-check                                  # identity vs off, same warp, same frame
tools/freecam_sweep.sh --quick --only mabe                               # a first look (6 shots)
tools/freecam_sweep.sh                                                   # 7 spots x 34 steps = 238 shots, ~26000 frames
```
It needs a test save (`LARECOMP_SMOKE_SAVE`, default `~/.local/share/lasrecomp-smoketest/save`, never written to) and
Python with PIL (`FREECAM_PYTHON=...`, or `tools/venv/bin/pip install pillow`). It warps (`LARECOMP_WARP_ON_LOAD` /
`LARECOMP_WARP_AT`) through Mabe Village, Mysterious Forest, the beach path (Field_09B), Marin and Tarin's house,
Tail Cave (entrance and 04B), Bottle Grotto 05D (the side view), steps the camera with `LARECOMP_FREECAM_AT` (stock view,
identity, yaw 0/45/90/180 x pitch 35/60 x distance 30/50, each with stock and widened culling `3.2:1.5`), and takes the
game image with `LARECOMP_SHOT_AT`. It forces `LARECOMP_BLUR_REMOVAL=1`. Output in `build/freecam/` (header of the script):
`sheet.png` overview, `sheet_<n>_<spot>.png`, `report.txt` (void fraction per shot: the share of black pixels, a cheap
hole detector), `suspects.txt`, `floats48.txt`, `game.log`. `tools/montage.sh` is macOS-only (sips), so the sheets are
made with PIL. The spot list and the Ukuku Prairie / north sea edge spots of the design are easy to extend (`SPOTS=`
at the top of the script); locators are from `runtime/ui/warp_data.h` **[G]**, as is the settle time of 480 frames
(`FREECAM_SETTLE`) and that a warp keeps the schedule's frame numbers (a present count) in step with the warp's.

## Gate (section 7) and where to look

1. Geometry complete inside about 1.2 x 1 screens around the pivot with widened culling: compare `_stock` and `_wide`
   shots, `report.txt` void column, the edges of the diorama rectangle.
2. Lighting, shadows, water, fog plausible off-axis: yaw 90/180 shots; outdoor shadows are fitted to the game's frustum **[G]**.
3. The suspects list short enough to handle: `suspects.txt`; floats 48-79 identified in `floats48.txt`.
4. `identity` bit-exact: `--identity-check` (the log line says whether VP's rebuild reproduced the game's bits; if not,
   identity keeps the game's VP bits and the orbit's VP is merely close). A game that is not deterministic frame to frame
   would show noise there; then compare `passthrough` and `identity` runs frame against frame instead.

## Guesses and limits

- The spec put the module at `runtime/nvn/cam_override.c`, the task at `runtime/hle/`; it is under `nvn/` (backend neutral, built by the existing wildcard).
- Link's position is read directly (`[[game manager + 0x4b8] + 0x80] + 0x80`, as doorjump.c finds it) in
  `widescreen_frame_end`, not published from `input_frame` (input.c is not this lane's). **[G]** untested without the game.
- Orbit's up vector is world +y; "floor" is y 0 (as widescreen.c's `view_point`); pivot on the side view assumes the zone box.
- Far plane stays as the game's by default, so a long pull-back can clip the far geometry (`LARECOMP_FREECAM_FAR`); anything linearising depth with the game's constants from another buffer then stays consistent, which is the point.
- Rewriting by address: a buffer re-written with another camera mid-frame (`CMD_UPDATE_UNIFORM_BUFFER`) is detected by its
  changed V and P and left alone; interpolation (`g_interp_log`) copies are bypassed for the camera.
- Only the exec thread touches the latch; the schedule and published state are plain flags (a probe, not shipping code).
