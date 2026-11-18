# VR test tooling (Steam Frame, native arm64)

Everything here runs without a headset and never touches SteamVR, except `tools/vrtiming.py`, which only
makes sense on the headset with a real session running. Builds refuse to run while
`~/.cache/headset-in-use` exists, use `nice` and at most two jobs.

| Tool | Use |
|---|---|
| `tools/fetch_openxr.sh` | Downloads the OpenXR SDK 1.1.63 (pinned URL and SHA-256), builds the loader and installs headers and `libopenxr_loader.so` into `build/linux/<uname -m>/deps` (`build/linux/aarch64/deps` on the Frame; `OPENXR_PREFIX` to change). Idempotent. |
| `tools/xrmock/build.sh [out]` | Builds `libxrmock.so`, `runtime.json` and `test_client` into `build/xrmock` and prints the path of `runtime.json`. |
| `tools/xrmock/test_client` | Minimal OpenXR + Vulkan 1.3 client: a colour per eye, a quad, optional depth (`--depth`), mutable-format eyes (`--mutable`), Vulkan 1.2 instance (`--vk12`), a deliberate validation error (`--bad-layout`). |
| `tools/xrmock/trace-compare.sh A B` | Diffs the call traces of two runs (setup lines, then the steady-state frames). |
| `tools/vvl/vvl.sh <cmd>` | Runs a command with the Khronos validation layer on. `tools/vvl/build-vvl.sh` builds it if it is missing (it is installed on this Frame already, 1.4.309). |
| `tools/desk_tests.sh [--list] [--only text]` | Every unit test that needs no game files, headset or GPU (CI's Linux list, the pacer, dynres, settings, translator, camera override, display period filter), one pass/fail table. Three stale tests are marked as known failures. Logs in `build/desk_tests/`. |
| `tools/session-report.py` | One table of frame pacing from the logs of one or more sessions. |
| `tools/cull_check/` | What the culling draws on the table, measured (docs/af/STREAM.md "Checking it"): `sweep.sh` (32 view cases at a spot with eye dumps), `report.py` (per case: the share of each footprint's pixels that are black here but drawn in a reference run), `overview.sh` (the table from above), `sheet.py` (one case's images side by side), `cost.sh` / `cost.py` (draws and thread times at the stock view), `flicker.py` (a moving head: the culled floor's steps, cells that come and go). |
| `tools/vrtiming.py SECONDS [out]` | SteamVR compositor timing, a line a second (headset only). |

## Selecting the mock

```
tools/fetch_openxr.sh && tools/xrmock/build.sh
export XR_RUNTIME_JSON=$PWD/build/xrmock/runtime.json     # the OpenXR loader then uses the mock
```

`XR_RUNTIME_JSON` is the only variable that selects it; the app links the real `libopenxr_loader.so`
(from `build/linux/<uname -m>/deps/lib`, or any 1.x loader). Unset it to use SteamVR again. The mock
makes the Vulkan instance and device through `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR`
with the app's own loader, so Turnip (and validation layers) are what the app really runs on. The
mock uses the app's queue (from the session binding) for its setup and PNG copies, as a runtime may,
so the app's queue lock has to cover OpenXR calls as well as its own submits.

Settings are `MOCK_XR_*` environment variables; the full list is the header comment of
`tools/xrmock/xrmock.c`. The ones used most:

| Variable | Meaning |
|---|---|
| `MOCK_XR_HZ=120` | Display refresh (`90`, `72`). `xrWaitFrame` is paced on that clock; predicted display time is `MOCK_XR_PIPELINE` (2) refreshes after it returns. A frame loop that falls behind loses refreshes (`MOCK_XR_SKIP=0` makes it catch up instead). |
| `MOCK_XR_SECONDS=20`, `MOCK_XR_FRAMES=n` | Session length; then STOPPING is sent (`0` = until the app quits). |
| `MOCK_XR_TRACE=<file>` | Call trace, no times (diffable). |
| `MOCK_XR_TIMING=<file>` | CSV, one line per frame: wait call / return / blocked ms, predicted display ms, period, begin, end, begin-to-end, end margin before display time, layers, refreshes lost, late. |
| `MOCK_XR_SHOT=30` / `10,20` / `5-8` / `every:60` / `all`, `MOCK_XR_SHOT_DIR=<dir>` | PNG of every image the app submitted at those frames: `f<frame>_proj<layer>_eye<0\|1>.png`, `f<frame>_quad<layer>.png` (the image rect of the layer, the image released last, sRGB bytes as stored; fp16 is encoded to sRGB, depth D32/D16 as grey). A dumped frame stalls the loop about 0.5 s per image. The trace lines `shot ...` carry the mean colour. |
| `MOCK_XR_POSE=sway` (default) / `static:x,y,z,yaw[,pitch[,roll]]` / `file:<path>` | Head pose in STAGE (metres, degrees). A file has lines `t x y z yaw pitch roll [gaze_yaw gaze_pitch]`, t in seconds from `xrBeginSession`, linear between lines, any length; the gaze columns (relative to the head, `nan`: no gaze) are what `MOCK_XR_GAZE=file` plays. The game's `LARECOMP_XR_POSE_LOG` writes this format and `tools/xrmock/headpose.py` makes it ("Flicker" below). LOCAL is the head's starting position and yaw, LOCAL_FLOOR the same on the floor, VIEW the head; two hands are action spaces. |
| `MOCK_XR_EYE_SIZE=1680x1760`, `MOCK_XR_FOV=l,r,u,d`, `MOCK_XR_IPD=0.064` | Recommended eye size, fov in degrees, eye separation. |
| `MOCK_XR_FOV_RIGHT=l,r,u,d` / `mirror`, `MOCK_XR_CANT=<deg>` | The right eye's own fov (`mirror`: `MOCK_XR_FOV` mirrored, an asymmetric fov that sees further outward than inward); each eye's view turned outward by that many degrees (canted displays). The game logs what it got once: `[xr] eye fov (degrees left right up down): ...; views N degrees apart`. |
| `MOCK_XR_GAZE=yaw,pitch[,ay,ap,hz[,jitter]]` / `file` / `file:<path>` | The eye gaze (XR_EXT_eye_gaze_interaction's pose action space) relative to the head, degrees: a direction plus a Lissajous path (amplitudes, `hz` in yaw, 0.7 hz in pitch) and a fixation jitter new every 1/240 s; or the gaze columns of the `MOCK_XR_POSE` file (`file`) or of another pose file. Unset, the gaze space is a hand pose (foveation then follows a fixed point). |
| `MOCK_XR_INPUT="t0-t1:path[=v[,v]];..."` | Scripted controller input: that input path (`/user/hand/right/input/a/click`, `.../thumbstick=0,1`, `.../trigger/value=0.8`) is held from t0 to t1 seconds after `xrBeginSession`. Actions are checked like a runtime would (bindings only on the inputs of the profile they name, nothing changes after the attach, sync before state); haptic pulses and input changes go to the trace (`input <action> = ...`, `haptic hand=... amplitude=...`) and the summary counts them. Game side: `LARECOMP_XR_INPUT_LOG=1` logs each change, `LARECOMP_XR_YAW=<deg>` sets the table yaw, `LARECOMP_XR_HAPTIC_TEST=t0-t1` forces a rumble. |
| `MOCK_XR_PROFILE=frame\|touch[,s:p]` | The controller profile in use, as `xrGetCurrentInteractionProfile` names it and an `XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED` announces at the first sync; `,6:touch` switches to Touch 6 s after `xrBeginSession`. Default: the Frame's when the app bound it, else Touch. Scripted input reaches an action only through a binding of the profile in use. |
| `MOCK_XR_EXTENSIONS=a,b` | The extensions offered instead of the Frame's list. A PC runtime with Touch controllers and no eye tracker: `XR_KHR_vulkan_enable2,XR_KHR_convert_timespec_time,XR_FB_display_refresh_rate` (docs/af/PCVR.md). |
| `MOCK_XR_VK_MAX=1.3` | Highest Vulkan version offered; `1.2` is what SteamVR offers, and an instance above it is refused (`VK_ERROR_INCOMPATIBLE_DRIVER`), to test the 1.2 fallback. |
| `MOCK_XR_BUSY_MS=<ms>` | At each release, wait for the app's queue to go idle and report a release that had to wait longer than this (work not finished when the image was released). |
| `MOCK_XR_SHIFT=<file>`, `MOCK_XR_SHIFT_REF=30` | Image against pose. Per refresh (`disp`): the layer's eye-0 pose against the head's at the display time, and which release of each eye's swapchain it shows. Per new eye image (`img`): its centre is copied without stalling the loop (a fenced copy on the app's queue, read back later), and its shift against each eye's first image after `MOCK_XR_SHIFT_REF` seconds is measured by correlating gradient profiles, converted to degrees with the eye's focal length. `tools/xrmock/shift-report.py <file> [--axis pitch\|yaw] [--from s] [--to s]` compares the rotation the content was rendered for with the rotation its layer claims, per eye and between the eyes of a set. |
| `MOCK_XR_SHIFT_DUMP=<dir>`, `MOCK_XR_SHIFT_DUMP_AT=a,b` | The probed images (luma, half size, centre 2/3) as PGM for display times a-b s. With a pose file of instant steps, `tools/xrmock/step-report.py <shift file> <dir>` compares each image of a hold with the hold's last one: content left over from the frame before the step (a pass reading an earlier frame, an eye with an older camera) shows as a larger difference in the first images. |
| `MOCK_XR_FLICK=<dir>`, `MOCK_XR_FLICK_AT=a,b`, `MOCK_XR_FLICK_SHIFT=2`, `MOCK_XR_FLICK_EYES=0\|1\|both` | A run of consecutive eye images for display times a-b s: every new image a layer names, whole, reduced on the GPU by 2^shift per side with exact 2x2 box steps (a quarter by default), read back later without stalling the loop, and written as `e<eye>_<seq>.pgm` (luma) by a writer thread. `index.txt` has an `img` line per image (sequence number per eye, frame, display and locate time, the layer view's pose and fov), a `disp` line per refresh (which image of each eye is shown) and a `drop` line for an image that found no free copy slot. Cost on the desktop: about 0.3 ms more in `xrEndFrame` per refresh (the luma of the small copy), a few ms once while its slots are made, no refreshes lost. `MOCK_XR_FLICK_BG=<luma>` composites each image over a background of that luma by its alpha, as a compositor blending the layer over the room would (the mock offers no ALPHA_BLEND, so Passthrough's alpha is otherwise invisible). `tools/flicker.py` reads it. |
| `MOCK_XR_COMPOSITOR=lead[,from]`, `MOCK_XR_COMPOSITOR_PRIO`, `MOCK_XR_COMPOSITOR_LOG` | A compositor's share of the GPU: a second VkDevice (its own kernel context, global priority realtime, else high, medium; `_PRIO` picks) submits two eye-sized blits `lead` ms before each vsync and waits for them. The summary gives the job's latency and how many finished after their vsync (counted from `from` s on); `_LOG` writes one line per refresh. |

Checks, as `ERROR` lines in the trace and on stderr and a summary at `xrEndSession`: call order of
wait / begin / end frame and acquire / wait / release; mip levels above 1 (SteamVR on the Frame
allocates level 0 only); layers that use a swapchain still acquired, never released, outside
its size, with a bad fov / pose / array index / depth info; both eyes from one `xrLocateViews`
(and for which display time); Vulkan handles not made through the runtime; instance version above
`MOCK_XR_VK_MAX`; extensions the app enables that the mock does not offer (the default list is a
guess at SteamVR's: vulkan_enable2, convert_timespec_time, display_refresh_rate,
performance_settings, VALVE_frame_controller_interaction, composition_layer_depth,
eye_gaze_interaction; widen it with `MOCK_XR_EXTENSIONS=a,b`). The summary also prints the
`xrWaitFrame` interval (avg, min, p99, max; `120.0 Hz` / `8.33 ms` is what to expect), begin-to-end,
how many frames were late and how many refreshes were lost.

Swapchains: three images each, any format Turnip supports of R8G8B8A8_SRGB, B8G8R8A8_SRGB,
R16G16B16A16_SFLOAT, A2B10G10R10_UNORM, R8G8B8A8_UNORM, B8G8R8A8_UNORM, D32_SFLOAT, D16_UNORM,
D24_UNORM_S8_UINT, D32_SFLOAT_S8_UINT; array layers, transfer/sampled/storage usage, and mutable
format with `XrVulkanSwapchainFormatListCreateInfoKHR`. Images are returned in
COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL and must be left in it at release.

## Validation layers

```
tools/vvl/vvl.sh build/lasrecomp [args]           # any command; build/xrmock/test_client works too
VVL_NO_WSI=1 tools/vvl/vvl.sh build/xrmock/test_client 60   # offscreen: drops gamescope WSI-layer noise
```

`VVL_LOG=<file>` writes the messages to a file, `VVL_SYNC=1` adds synchronization validation. The
game's `LARECOMP_VK_VALIDATION_STOP=1` (device.c) stops at the first error. Thread safety is part
of the default checks. `test_client --bad-layout` must produce `VUID-vkCmdClearColorImage-imageLayout-00004`
(proves the layer is on); a clean run under gamescope shows two instance/device extension VUIDs
from the WSI layer, which `VVL_NO_WSI=1` removes.

## Flicker

The Surroundings and Table Edge flicker were seen in the headset only ([SCENE](SCENE.md), [EDGE](EDGE.md), "Open:
headset flicker"). These pieces bring the headset's conditions to the desktop and put a number on flicker.

**Head and gaze.** `tools/xrmock/headpose.py <out> [--seed N] [--still] [--base x,y,z,yaw,pitch]` writes a pose file
(with gaze columns) of a head on a neck: a tremor of a quarter of a degree and a body sway of millimetres, never still;
turns to look elsewhere every second or so with a minimum-jerk profile peaking at up to 200 degrees a second, now and
then a wide look away and back; nods; the eyes 10 cm in front of and 7 cm above the neck's pivot, so a rotation moves
them. The gaze fixates world points (it counter-rotates while the head turns) and jumps every 0.2-0.5 s. Seeded: the
same arguments give the same file. `--still` holds the head and moves only the gaze. A recording replaces it: the game's
`LARECOMP_XR_POSE_LOG=<file>` writes, per refresh, the display time, the head (VIEW in LOCAL) and the gaze (relative to
the head) in the same format; nothing happens when it is unset. `MOCK_XR_POSE=file:<log> MOCK_XR_GAZE=file` plays it
back; the mock's LOCAL is then the recording's first pose, which is where the game measures the table from anyway.

**The headset's eyes and settings.** `MOCK_XR_FOV=-52,44,50,-54 MOCK_XR_FOV_RIGHT=mirror` (assumed Frame values: the
logs of 2026-11-06 do not print the fov; the `[xr] eye fov` line now does), `MOCK_XR_CANT` if the real views turn
apart. Game-side test switches, all off by default:

| Variable | Meaning |
|---|---|
| `LARECOMP_VR_DYNRES_STEP=<s>[,px,...]` | the eye size on a schedule instead of by GPU time: each size held `s` seconds (default the floor and the top: 1536 and 1728 with `LARECOMP_VR_HEADROOM=0`). A desktop GPU never leaves full size by itself; the headset sat at 1536. `LARECOMP_VR_DYNRES_MAX=1536` pins it there. |
| `LARECOMP_VR_FOVEATION_TEXEL=16` | 16 px shading-rate texels instead of 8: NVIDIA's attachments take no other size, so without it foveation is off on such a GPU and the gaze changes nothing. The rings are then twice as coarse as the Frame's; the edge pass's block smoothing follows. |
| `LARECOMP_XR_FPS=30` | the game held at 30 fps at 120 Hz (4 refreshes a frame), as the pacer held it in the headset sessions where the flicker was seen (GPU 22 ms a frame). |

**Images.** `MOCK_XR_FLICK` (above) takes every new eye image of a window whole, a quarter size, without the half-second
stall of `MOCK_XR_SHOT`.

**The metric.** `tools/flicker.py` (`footprint` and `score`): flicker is a pixel whose luma goes up by more than 24 (of
255) and back down by more than 24 within three consecutive images of one eye, or down and back up. Before comparing,
each image is brought into a reference view by a rotation from the pose it was submitted with (rotation only; a still
head needs none). Regions in the reference view: `edge`, a band along the outline of the table's footprint (8% of the
eye's width inside it, 5% outside: a fade darkens the table inside the outline, Surroundings draw past it); `table`,
the footprint inside that band; `far`, the rest. Per region: the share of its pixels that flicker per image (mean,
99th percentile, maximum) and the same against the region's drawn pixels only (a black void cannot flicker). The
footprint is where a run with Surroundings and Table Edge off draws at all. `strip.png` shows the worst triples with the
flickering pixels in red; `series.tsv` has one line per image (display time, frame), to line up with the game's
`[scene] backdrop capture`, `[vrdyn]` and `[edgelog]` lines.

**The runner.** `tools/flicker_run.sh <config> [--runs N] [--head still|move] [--seed S] [--dynres step|pin|auto]
[--fps30] [--pose FILE] [--set VAR=VAL] [--keep]`; configs `default`, `backdrop`, `full`, `low`, `fade`,
`passthrough`. Each run: a fresh copy of the test save, login, warp to Mabe Village (Field_11C), 120 Hz, 1728 x 1728
eyes with the fov above, dynamic resolution with `LARECOMP_VR_HEADROOM=0` stepping 1536 / 1728 every second, foveation
High with 16 px texels and the moving gaze, the copies variant of the Frame profile (`tools/frame_profile.env` then
`tools/frame_profile_copies.env`), Follow Smooth, the table 0.8 m away; the head at rest looks 10 degrees down so the
table's far side, its edge and the void are all in view. Images from 24 to 32 s into the session (the warp has loaded
by about 18 s; the run checks it). It takes `~/.cache/af-bench-lock` around each program run only (about 45 s) and
prints each run's numbers, then the mean and range. `--head still` (the default) is the run to trust: nothing but the
gaze, the eye size and the game moves. `--head move` plays a head on a neck; its numbers carry what the rotation-only
registration leaves (the eyes' few centimetres of travel), so compare it only with a `default` run of the same seeds
and the same frame rate: the pacer can drop a moving run to 30 fps, which doubles the motion between images, so moving
comparisons use `--fps30`.
Output in `build/flicker/` (never commit it: the images are the game's).

## Log format for `xr_timing.log`

One line per displayed frame, from the frame loop, in the app's log or a log of its own
(`xr_timing.log`, in the data folder, previous session as `xr_timing.log.prev`). Times are
milliseconds on one clock (`CLOCK_MONOTONIC` minus the time of `xrBeginSession`; the converted
`XrTime` of `xrConvertTimespecTimeToTimeKHR` gives the predicted display time on the same clock):

```
HH:MM:SS.mmm xrt f=<n> t=<ms> pdt=<ms> period=<ms> wait=<ms> begin=<ms> end=<ms> gpu0=<ms> gpu1=<ms> [hud=<ms>] [late=0|1]
```

| Field | Meaning |
|---|---|
| `f` | frame counter |
| `t` | time `xrWaitFrame` returned |
| `pdt` | `predictedDisplayTime` of that wait |
| `period` | `predictedDisplayPeriod` |
| `wait` | time blocked in `xrWaitFrame` |
| `begin`, `end` | times of the `xrBeginFrame` and `xrEndFrame` calls |
| `gpu0`, `gpu1`, `hud` | GPU ms of the eye 0, eye 1 and HUD work of the set shown (timestamp queries; leave the field out or use -1 if unknown) |
| `late` | the app's own verdict, 1 when its set was not finished when due (optional; otherwise `end` later than `pdt - period/2` counts as late) |

Anything else on a line is ignored, and so is any other line. GalaxyFrame's ten-second lines are
read as well when present (`vr: N of M game frames shown twice`, `vr: the frame loop missed N
refreshes in the last S s (K with its own work late); A of B sets not finished when due`,
`vr: render scale a-b (now c, N changes); eye GPU X ms avg Y max`).

```
tools/session-report.py xr_timing.log vrtiming.log previous.log ...   # one column per session
tools/session-report.py <data folder>                                 # current and previous
tools/session-report.py build/mock-timing.csv                         # the mock's MOCK_XR_TIMING file
```

A `vrtiming.log` (name contains `vrt`) after a log adds the SteamVR compositor rows to its column.
The report shows frame rate, frame gap avg / p99 / max, refreshes lost, wait, begin-to-end, end
margin before display time, late frames, GPU per eye over the period, shown-twice counts and hitches
over 100 ms. `tools/vrtiming.py` is GalaxyFrame's: it reads the compositor timing of the scene app
through SteamVR's OpenVR library at `/opt/steamvr/bin/linuxarm64` (`VRTIMING_OPENVR` overrides), once a
second, read-only.

## Limits

- Not modelled: foveation, XR_FB_space_warp, visibility mask, controller input (actions exist and are inactive;
  action spaces return fixed hand poses), multiple sessions, multisampling (sample count above 1 is refused), cube
  faces. Eye gaze only with `MOCK_XR_GAZE`.
- The pacing is a fixed clock plus a loop that sleeps. SteamVR's real wake-up prediction, reprojection and
  compositor GPU load are not there: a pass here does not prove a frame fits SteamVR's deadline.
- The mock cannot say whether images are the right way up or the eyes the right way round, only that
  the app's submitted pixels are what a PNG shows; look at the PNGs.
- PNG dumps copy through the app's queue and wait for idle; on Turnip a 1680x1760 image takes roughly half a second.
- The Gamescope WSI layer is loaded by the environment; set `ENABLE_GAMESCOPE_WSI=0` if you want it out of the way.
