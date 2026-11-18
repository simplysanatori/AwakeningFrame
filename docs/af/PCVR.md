# PC VR (Linux and Windows)

The VR mode on a PC headset through SteamVR or another OpenXR runtime: a Quest over Link, Air Link, Virtual Desktop
or ALVR, an Index. One code path with the Frame build (`runtime/xr/`,
`runtime/nvn/vulkan/`); the Frame's packaging (`steam-frame/`) is not involved.

Status: Linux tested under the mock runtime only (`tools/xrmock`), not yet with a headset. Windows: prepared and
compile-checked with a MinGW cross compiler, **not run on Windows**.

## Packages

| Package | Make | Result |
|---|---|---|
| Linux AppImage (x86-64) | `make appimage PCVR=1` | `build/dist/LAS Recomp-<version>-linux-x86_64-pcvr.AppImage` |
| Windows zip (x64) | `pcvr/fetch-openxr-windows.sh`, then `make dist PCVR=1` (the steps of docs/BUILD-windows.md "Package") | `build/dist/LAS Recomp-<version>-windows-pcvr.zip` |

Without `PCVR=1` both targets build upstream's flat packages, without the VR mode, whatever the deps folder holds.
`.github/workflows/pcvr.yml` builds both on GitHub; it runs only by hand and only when the repository variable
`PCVR_CI` is `1`.

The Makefile switch, for any build: `XR=auto` (default: the VR mode when `DEPS` has the OpenXR headers, as the Frame
and desktop test builds have always done), `XR=0` (never), `XR=1` (stop if the headers are missing). `PCVR=1` adds
`LARECOMP_PCVR`: VR by default (below). A build folder holds one kind: make does not rebuild an object when only its
flags change, so the packages use `build/linux/x86_64/game-pcvr` and `build/win-release-pcvr`.

## The OpenXR loader

The program opens the loader at run time; nothing is linked.

- Linux: bundled, `usr/lib/libopenxr_loader.so.1` (Khronos 1.1.63, built in the AppImage's Ubuntu 22.04 container by
  `tools/fetch_openxr.sh`, so it needs only glibc and libstdc++). The program's RUNPATH (`$ORIGIN/../lib`) comes before
  the system's library path, so the bundled one is used. Why bundled: not every distribution installs a loader, SteamVR
  does not put its own on the library path, the loader's ABI is stable, and the runtime is still the player's
  (SteamVR's `active_runtime.json`, or `XR_RUNTIME_JSON`).
- Windows: Khronos' release build `openxr_loader.dll` (the OpenXR.Loader NuGet package of 1.1.63, pinned by SHA-256)
  next to the exe. It imports only KERNEL32 and ADVAPI32. `LoadLibrary` looks in the exe's folder first. The runtime
  comes from the registry (`ActiveRuntime`), as for every OpenXR game.
- `LARECOMP_XR_LOADER=<file>` picks another loader. Licence: Apache 2.0 (`OpenXR-LICENSE.txt` in the package, and
  `OpenXR-jsoncpp-LICENSE.txt` for the JSON reader built into the loader; THIRD_PARTY_NOTICES.md).

## Starting

- A PC VR package starts in VR. `--flat` (or `LARECOMP_XR=0`) starts it in the window; `--vr` (or `LARECOMP_XR=1`)
  insists on VR and stops with the error if there is no headset.
- In VR with the game code built, the game starts straight away, without the start screen, as on the Frame (its
  launcher's `--play`). With game files but no game code yet, the start screen comes first and builds it, as on the
  Frame. `--flat` gives upstream's start screen in the window.
- No game files yet: the first start opens the start screen in the window, where its folder picker can be seen. Set up
  the game files there, quit, and start again: VR from then on.
- No OpenXR runtime, or none with a headset (SteamVR not running, the headset off): the game starts in the window, and
  `lasrecomp.log` says why (`[xr] PC VR package: no OpenXR runtime with a headset`).
- `--vr` and `--flat` work in every build with the VR mode, not only the packages. The Frame build is unchanged: VR only
  with `LARECOMP_XR=1`, which its launcher sets.
- In VR the window stays (it keeps the keyboard, mouse and gamepads) but shows nothing; there is no mirror yet. Closing
  it quits. The start screen's update check is off in the package (it would point at upstream's flat releases).

Settings, saves and logs are where the flat game keeps them: `~/.local/share/lasrecomp` on Linux, next to the exe on
Windows. `lasrecomp.log` and `xr_timing.log` are there. The VR tab works as on the Frame (docs/af/VR_SETTINGS.md).

Environment variables (as on the Frame: docs/af/TOOLING.md) go in front of the command, or into Steam's launch options
as `VAR=value %command%` for a non-Steam game entry.

## SteamVR settings

The Frame's launcher sets four of SteamVR's per-application settings through `vrcmd` before every start
(`steam-frame/awakeningframe.sh`). The program does not do this on a PC: the player sets them once in SteamVR's
settings, Video, Per-Application Video Settings, for this program (a manual step; checklist below):

| Setting | Value | Why |
|---|---|---|
| Refresh rate | 120 Hz (or 90) | the game's rate follows the display's (below) |
| Motion Smoothing | off | the game paces its own 60 FPS; a smoothed frame ghosts |
| Throttling | none | the program decides how many refreshes a game frame is shown for |
| (additional frames to predict) | 0 | not in the dashboard; `vrcmd` only |

For a Steam library entry the same keys can be set with SteamVR's `vrcmd`, as the Frame's launcher does:
`vrcmd --set-settings-int steam.app.<app id>.<key> <value>` with `preferredRefreshRate`, `framesToThrottle`,
`additionalFramesToPredict` and `motionSmoothingOverride` (2: off). Not tried on a PC yet.

The program also asks the runtime for a rate where the runtime takes such requests (`XR_FB_display_refresh_rate`;
Meta's runtime for Link has it, SteamVR may not): `LARECOMP_XR_HZ`, else the VR tab's Refresh Rate, else 120 Hz, else
90 Hz when 120 is not offered.

## Refresh rates

The game is a 60 FPS game (`runtime/xr/hz_filter.h` `hz_rule`, tested by `tools/test_hz_filter.c`):

| Display | Game | Each game frame |
|---|---|---|
| a multiple of 60 Hz (60, 120) | 60 FPS | the same number of refreshes (2 at 120 Hz) |
| a multiple of 30 Hz that is not one of 60 (90, 150) | held at 30 FPS | 3 refreshes at 90 Hz |
| anything else (72, 80, 144) | 60 FPS, uneven | 1 or 2 refreshes (72, 80), 2 or 3 (144): motion judders a little |

At the even rates the game's vsync counts refreshes, so it stays locked to the display; at the uneven ones it follows
the display time. On a Quest: 120 Hz is the best choice (60 FPS, even); 90 Hz is even at 30 FPS and asks the least of the
GPU; 72 and 80 Hz give 60 FPS with an uneven cadence. `LARECOMP_XR_FPS=60` at 90 Hz is the uneven 60 FPS test.

## What a PC headset does not have

- No Frame controller profile: the Touch profile is used, with a layout of its own (docs/af/CONTROLS.md "Touch
  controllers"). Other controllers work through the runtime's binding of Touch (SteamVR's controller bindings).
- No eye gaze (a Quest 2 or 3; `XR_EXT_eye_gaze_interaction` missing or not tracked): no foveation, everything renders at
  full density, as on the Frame when the gaze is lost (docs/af/STEREO.md "Foveation"). Neither is an error.
- No Valve foveation layer, no headset-in-use marker, no compositor timing log, no `session.env` (the Frame
  launcher's parts, `steam-frame/awakeningframe.sh`); SteamVR's frame timing graph shows the compositor's side.

The GPU defaults are the desktop GPU's (docs/vulkan-gpu-defaults.md), not the Frame's Adreno choices; the pacer's VR
rules (`PACE_VR_GPU`, runtime/nvn/pace.c) apply as on the Frame. Timings measured on the Frame do not carry over.

## Windows (untested)

From an MSYS2 UCRT64 shell set up as in docs/BUILD-windows.md:

    pcvr/fetch-openxr-windows.sh   # once: OpenXR headers and openxr_loader.dll into build/deps-win
    make -j B=build/pcvr DEPS="$PWD/build/deps-win" XR=1 PCVR=1 build/pcvr/lasrecomp.exe   # the app alone
    make deps-windows && make rando-helper && make gen-helper && tools/fetch_zig.sh
    make dist PCVR=1               # -> build/dist/LAS Recomp-<version>-windows-pcvr.zip

To test without a headset, the mock runtime is Linux only for now (`tools/xrmock` uses Linux's dlopen and clocks); a
first Windows run needs SteamVR with a headset (or SteamVR's null driver, for a session without one). What to check
on a first run:

1. `lasrecomp.log`: `[xr] runtime <name> <version> (...)`, and no `[xr] no time conversion from the runtime` line
   (that line means the runtime lacks `XR_KHR_win32_convert_performance_counter_time`; the poses are then predicted
   from the vsync).
2. `[xr] controller profile in use:` names the profile; the buttons as in docs/af/CONTROLS.md (`LARECOMP_XR_INPUT_LOG=1`).
3. `xr_timing.log` is written and replaced on the next start.
4. `--flat` gives the window; closing the window in VR quits.

What was checked on Linux: every C file of the runtime compiled for x86_64-w64-mingw32 (clang, MSYS2 UCRT64's headers
and libraries: SDL3, FFmpeg, Vulkan, shaderc, VMA, winpthreads) with the VR mode on and off; weak symbols resolving to
NULL in a PE program. Not checked: linking, the resources, running.
