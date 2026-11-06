# VR test tooling (Steam Frame, native arm64)

Everything here runs without a headset and never touches SteamVR, except `tools/vrtiming.py`, which only
makes sense on the headset with a real session running. Builds refuse to run while
`~/.cache/headset-in-use` exists, use `nice` and at most two jobs.

| Tool | Use |
|---|---|
| `tools/fetch_openxr.sh` | Downloads the OpenXR SDK 1.1.63 (pinned URL and SHA-256), builds the loader and installs headers and `libopenxr_loader.so` into `build/linux/aarch64/deps` (`OPENXR_PREFIX` to change). Idempotent. |
| `tools/xrmock/build.sh [out]` | Builds `libxrmock.so`, `runtime.json` and `test_client` into `build/xrmock` and prints the path of `runtime.json`. |
| `tools/xrmock/test_client` | Minimal OpenXR + Vulkan 1.3 client: a colour per eye, a quad, optional depth (`--depth`), mutable-format eyes (`--mutable`), Vulkan 1.2 instance (`--vk12`), a deliberate validation error (`--bad-layout`). |
| `tools/xrmock/trace-compare.sh A B` | Diffs the call traces of two runs (setup lines, then the steady-state frames). |
| `tools/vvl/vvl.sh <cmd>` | Runs a command with the Khronos validation layer on. `tools/vvl/build-vvl.sh` builds it if it is missing (it is installed on this Frame already, 1.4.309). |
| `tools/session-report.py` | One table of frame pacing from the logs of one or more sessions. |
| `tools/vrtiming.py SECONDS [out]` | SteamVR compositor timing, a line a second (headset only). |

## Selecting the mock

```
tools/fetch_openxr.sh && tools/xrmock/build.sh
export XR_RUNTIME_JSON=$PWD/build/xrmock/runtime.json     # the OpenXR loader then uses the mock
```

`XR_RUNTIME_JSON` is the only variable that selects it; the app links the real `libopenxr_loader.so`
(from `build/linux/aarch64/deps/lib`, or any 1.x loader). Unset it to use SteamVR again. The mock
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
| `MOCK_XR_POSE=sway` (default) / `static:x,y,z,yaw[,pitch[,roll]]` / `file:<path>` | Head pose in STAGE (metres, degrees). A file has lines `t x y z yaw pitch roll`, t in seconds from `xrBeginSession`, linear between lines. LOCAL is the head's starting position and yaw, LOCAL_FLOOR the same on the floor, VIEW the head; two hands are action spaces. |
| `MOCK_XR_EYE_SIZE=1680x1760`, `MOCK_XR_FOV=l,r,u,d`, `MOCK_XR_IPD=0.064` | Recommended eye size, fov in degrees, eye separation. |
| `MOCK_XR_INPUT="t0-t1:path[=v[,v]];..."` | Scripted controller input: that input path (`/user/hand/right/input/a/click`, `.../thumbstick=0,1`, `.../trigger/value=0.8`) is held from t0 to t1 seconds after `xrBeginSession`. Actions are checked like a runtime would (bindings only on the Frame profile's inputs, nothing changes after the attach, sync before state); haptic pulses and input changes go to the trace (`input <action> = ...`, `haptic hand=... amplitude=...`) and the summary counts them. Game side: `LARECOMP_XR_INPUT_LOG=1` logs each change, `LARECOMP_XR_YAW=<deg>` sets the table yaw, `LARECOMP_XR_HAPTIC_TEST=t0-t1` forces a rumble. |
| `MOCK_XR_VK_MAX=1.3` | Highest Vulkan version offered; `1.2` is what SteamVR offers, and an instance above it is refused (`VK_ERROR_INCOMPATIBLE_DRIVER`), to test the 1.2 fallback. |
| `MOCK_XR_BUSY_MS=<ms>` | At each release, wait for the app's queue to go idle and report a release that had to wait longer than this (work not finished when the image was released). |

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

## Log format for the app (what lane 3 should emit)

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

- Not modelled: foveation, XR_FB_space_warp, visibility mask, eye gaze data, controller input (actions
  exist and are inactive; action spaces return fixed hand poses), multiple sessions, multisampling
  (sample count above 1 is refused), cube faces.
- The pacing is a fixed clock plus a loop that sleeps. SteamVR's real wake-up prediction, reprojection and
  compositor GPU load are not there: a pass here does not prove a frame fits SteamVR's deadline.
- The mock cannot say whether images are the right way up or the eyes the right way round, only that
  the app's submitted pixels are what a PNG shows; look at the PNGs.
- PNG dumps copy through the app's queue and wait for idle; on Turnip a 1680x1760 image takes roughly half a second.
- The Gamescope WSI layer is loaded by the environment; set `ENABLE_GAMESCOPE_WSI=0` if you want it out of the way.
