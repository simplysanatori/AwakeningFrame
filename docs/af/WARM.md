# Shader warm-up

Goal: no shader or pipeline compile during play, in VR above all, where one slow frame is a freeze in the player's view.

## What was cached, and why the first visit still hitched

The game builds three things the first time a draw needs them, each on a worker thread while the draw is skipped
(pop-in) and the game thread waits for the translation (a hitch):

| step | cached on disk? | where |
|---|---|---|
| Maxwell code -> GLSL (translator, `maxwell_translate`) | no | |
| GLSL -> SPIR-V (shaderc) | yes, by hash of the GLSL source and stage | `cache/spirv/<hash>.spv` (`shaders.c`) |
| SPIR-V + state -> pipeline (Turnip) | yes, the driver's VkPipelineCache | `cache/vk_pipelines.bin` + `.tag` (`device.c`) |

So a second visit still translated every shader (CPU, on the render path) and made every pipeline from the driver's
cache. And, with `--play` (how the Steam shortcut starts the game), nothing was prepared in advance at all:

- LAS Recomp ships a list of 1585 variants (`runtime/nvn/shader_variants.txt`, recorded by a tour of every warp spot) and
  prepares them before the game starts (`shader_prep.c`). Only the **start screen** ran that (a progress bar on the first
  start, in the background on later ones). `--play` skips the start screen, so the list was never prepared: the first
  visit to anywhere compiled ~300 variants (flat, five spots: 292 first visits, 2184 draws skipped, frames of 100-150 ms).
- The player's own list, `cache/shader_seen.txt` (variants the game built that the shipped list lacks; the manifest of
  everything met), is written as the game runs and prepared on the next start, but also only by the start screen.
- The pipeline cache was saved at a clean exit and every three minutes. A kill (SteamVR ending the app, a crash) lost
  up to three minutes of compiles.
- The pipeline cache's tag held the build's translator hash (a hash of the translator sources), so every build that touched
  `translate.c` threw the whole cache away: about a minute of compiles to get it back (cold, preparing the list takes 54 s).
- VR: the stereo executor changes the blend state of the composite (alpha factors) and runs more draws into the eye
  outputs; 12 of the variants it builds (blended UI / fade draws and one two-stage particle program) are not in the
  shipped list. They were first-visit compiles in VR only.

## What changed

- `shader_prep_boot` (`shader_prep.c`): the preparation now runs at every start without the start screen (`--play`),
  before the game's first frame; off for scripted test runs (`LARECOMP_NO_LAUNCHER`); `LARECOMP_SHADER_PREP=1/0` forces it
  either way; skipped when the start screen already did it in the process. It has a time budget,
  `LARECOMP_SHADER_PREP_BUDGET` seconds (default 20; 90 in VR, below): out of time, it stops where it is, the rest compiles on demand and
  what was built is in the pipeline cache, so the next start gets further. It logs `[shader] preparing: N %` every 10 %.
  Warm it takes about 7 s (translation is one thread; the pipelines come from the cache).
- Pipeline cache (`device.c`): the tag is the GPU, driver and cache format only (the driver keys entries by SPIR-V and state, so
  a new translator just stops hitting old entries); a file over 192 MB starts over. Saved by the saver thread one second
  poll at a time: when pipelines were made and none for 3 s (a level's compiles are over), not on a timer, and still at exit
  and at the end of the preparation.
- `tools/warm_shaders.sh`: the tour (below). `install.sh --warm` runs it.

## The warm-up tour

`tools/warm_shaders.sh` plays the game headless, from a copy of the test save, and warps through all 410 spots of
`runtime/ui/warp_data.h` (overworld, houses and caves, dungeons; `LARECOMP_SHADER_TOUR`, `runtime/ui/shader_tour.mm`),
150 frames each, in 90-spot sessions (a crash or interruption costs one session). Two passes:

1. VR under the mock OpenXR runtime (`XR_RUNTIME_JSON` from `tools/xrmock/build.sh`, `LARECOMP_XR=1`, eye 1728x1728),
2. flat.

Both with `SDL_VIDEODRIVER=offscreen ENABLE_GAMESCOPE_WSI=0`, `LARECOMP_CHEATS=hearts`, a scratch copy of the user's
`larecomp.ini` (the shaders depend on anti-aliasing, blur removal and frame interpolation) and the preparation on. The
real saves and settings are never written. What it learns lands in the data folder's `cache/`: the pipeline cache, the
SPIR-V cache and `shader_seen.txt`, so the next start prepares all of it in seconds.

```
tools/warm_shaders.sh [--app DIR] [--root DIR] [--save DIR] [--flat-only | --vr-only] [--per N] [--chunk N] [--start I --end I]
install.sh --warm        # after the install, the same against the installed program and ~/.local/share/lasrecomp
```
Before each launch it waits (polling every 2 minutes) while `~/.cache/headset-in-use` or `~/.cache/af-bench-lock` exists or
any other `lasrecomp` is running, and it stops and redoes a session if one of the two files appears while it runs.
Needs the test save (`~/.local/share/lasrecomp-smoketest/save`, or `LARECOMP_SMOKE_SAVE`: a folder with `save_0/`).
Logs: `~/.cache/awakeningframe-warm/logs/`.

Re-run it after a new build that changes what the renderer draws (a new pass or format makes new pipeline variants; the
caches survive translator changes, but variants nobody has built are first visits). After a GPU driver update the
pipeline cache starts over (tag), and the next start prepares what it can in 20 s and finishes over the following starts,
or run the tour again.

## First start in VR

A first start on the headset after an install (log of 2026-11-06 08:03) showed what a cold cache did with the
20 s budget: the preparation went through all 1587 records in 20 s but most of their builds were still queued on the
worker threads when it gave up ("out of time after 20 s at -6 %"), so the game started with ~1000 pipelines still
building. For the next 35 s the title, the file select and the first room drew with pieces missing (up to 6570 draws
skipped per 2 s, `[vk] t=...` lines); that was a scuffed panel. The second start prepared everything in 6 s and
skipped nothing. Meanwhile the headset showed nothing at all for those 20 s (the XR loop had no layers to submit).

- **Progress:** `[shader] preparing: N %` counted records gone through minus builds pending; a record queues one or two
  builds, so a cold start went negative. Now it is the records gone through times the share of their queued builds that
  are done (`renderer_prepare_finished`, both backends), never falling back.
- **Label:** while the preparation runs, a thread of its own draws a frame of the overlay alone about 30 times a second
  (`backend_boot_frame`, device.c; `menu_boot_notice`, menu.mm): "Preparing shaders... N %" (the start screen's own
  string) centred on a dark plate. In VR that frame is the HUD / menu panel, cleared transparent around the label, with
  the stub's dark eyes behind it; flat, the window. On screen only. A last frame without the label is drawn when it ends.
  `LARECOMP_SHADER_PREP_LABEL=1` turns it on (off by default until it has been seen in the headset).
- **VR budget:** 90 s in VR mode (`LARECOMP_XR=1`), 20 s flat; `LARECOMP_SHADER_PREP_BUDGET` still sets either. A cold
  start on the Frame takes about a minute, so the whole list is built before the game starts and the game's first
  minute is clean; warm starts are unchanged (6-7 s, with the label up).

## Limits

- Settings that change shaders (anti-aliasing, blur removal, frame interpolation, upscaler) are warmed for the values in
  `larecomp.ini` when the tour ran. Another value is a first visit once, then in `shader_seen.txt`.
- The tour renders every spot for 150 frames: an effect that only appears in a fight or a cutscene may not be met.
  Whatever is met later is recorded and prepared on the next start.
- Steam also runs its own Fossilize shader cache for the shortcut (`steamapps/shadercache/<appid>`); that is separate.
