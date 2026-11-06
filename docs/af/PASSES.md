# VR render passes and the pass diet (AwakeningFrame diet lane)

Where a VR game frame's GPU time goes, both eyes, and what VR mode leaves out (`runtime/nvn/vulkan/vr_diet.c`).

## How it was measured

`LARECOMP_GPUPROF_AT=<frame>` under xrmock (1728² eyes, 90 Hz, game at 30 fps), `tools-local/dietbench.sh` (a copy of
the perf lane's `vrbench.sh`). Each `[gpuprof]` row is the GPU time from one mark (a pass, copy or dispatch) to the next.
VR rows carry `e0` / `e1` and the first draw's fragment shader address, target and textures.
`tools/gpuprof_groups.py run.log [n]` sums them into the groups below by fragment shader hash. Eye 0 also holds the
shared work (shadow map, compute, the HUD) and the GPU idle between submission chunks.

## Mabe Village (Field_11C), before the diet

| group | eye 0 ms | eye 1 ms | passes (e0 / e1) | what |
|---|---|---|---|---|
| main colour | 4.51 | 4.48 | 1 / 1 | f0b2d796, ~245 draws, plus 8-11 water / transparent draws after it |
| idle gap | 4.16 | 0 | 1 / 0 | GPU waiting for the next submission chunk inside eye 0 (translation pacing) |
| depth prepass | 1.21 | 1.21 | 1 / 1 | d853b93d, the same ~245 draws, depth only |
| scene copies | 0.87 | 0.81 | 0 | scene -> RGBA16F 0.45, depth x2 0.15 each, eye output copy 0.08 |
| DOF (tilt-shift) | 0.70 | 0.70 | 5 / 5 | 7b537cfc downsample x4 (1/2, 1/4) + 86a3fa6a full-size composite |
| compute | 1.00 | 0 | - | dispatches (eye 0 only) |
| AO | 0.49 | 0.47 | 5 / 5 | depth copies to 1/2 and 1/4, d7fe0c3e at 480x270 (guest size, not scaled), 589f0038 blur x4 |
| pre-scene 704x396 | 0.44 | 0.44 | 2 / 2 | 01b5fc5f depth + 6804ba6e colour, 64 draws, samples the shadow map |
| shadow | 0.80 | 0.01 | 2 / 1 | d3e83e03, ~266 casters, shared |
| empty passes (clears) | 0.33 | 0.37 | 11 / 12 | passes with no draw |
| AO apply | 0.29 | 0.28 | 1 / 1 | bbb2410a, full size |
| tonemap -> eye output | 0.29 | 0.30 | 1 / 1 | ca3fbe0c, full size (a filmic curve: no anti-aliasing in it) |
| bloom composite | 0.26 | 0.25 | 1 / 1 | 5bcc5bba, full size |
| bloom | 0.15 | 0.12 | 9 / 7 | f2f5df1c bright pass at 480x270, 589f0038 blur pyramid to 60x33, 6d742408 upsample |
| HUD | 0.21 | 0 | 18 / 0 | each HUD draw its own pass (redirected into the HUD clone, the pass ended after it) |
| total | 15.73 | 9.50 | 59 / 39 | [stereo] GPU per game frame: 15.46 + 9.42 ms; [pace] gpu 22.0 ms |

The forest (Field_06C) has the same post chain (DOF 0.67 ms per eye) under a costlier scene (11.2 ms of other draws,
shadow 3.0). Marin & Tarin's house has no tilt-shift and ~9 ms for both eyes; Tail Cave ~11 ms.

Findings against the plan:
- The tilt-shift chain still ran with blur removal on: its five passes per eye, 0.7 ms, for an image equal to its input.
- The game has no SMAA of its own, and the runtime's SMAA never ran on the eyes (only on the flat present).
- AO is already at 480x270 (guest size, not scaled with the eyes): 0.2 ms; bloom is already at 1/4 size and below:
  0.13 ms. Halving them gains nothing; what is left of them is full-size passes (AO apply, bloom composite).
- About 11 ms of the 22 scales with the pixel count: main colour and prepass are draw bound, the post chain is ~2.5 ms
  per eye of full-size passes and copies.

## The diet (VR mode only, on by default; flat unchanged)

`LARECOMP_VR_DIET`: unset / `1` all, `0` none, or a list of `dof`, `merge`, `smaa`.

- **dof**: the downsample draws whose targets only feed the composite are left out (learned each frame from the
  composite's inputs; the post targets alternate between two sets from frame to frame). The composite is left out too:
  the AO apply, which wrote its sharp input, draws into the composite's target instead (only an overwrite: blending
  that keeps the target, or a view, is not moved). A draw that reads the moved texture gets it back first (a copy), and a
  frame whose writes weren't moved gets a copy in the composite's place.
- **merge**: a draw on the same attachments as the open pass, with nothing in between (copies, dispatches, feedback
  snapshots and uploads end a pass), joins it: no barrier, tile load / store or binning of its own. The HUD's 18 passes
  become one.
- **smaa**: no SMAA on the window image in VR (menus on the HUD quad).

Mabe, one profiled frame after the diet: DOF 0 (was 0.70 + 0.70), HUD 0.04 ms in 1 pass (was 0.21 in 18), passes
71 (was 98), total 23.8 ms of rows (was 25.2). Eye images at Mabe, the forest, the house and Tail Cave match the
diet-off ones but for what moves between runs (the game isn't deterministic): <1% of pixels differ by more than 24 / 255
outdoors, none in the house and the HUD; bloom glow and AO contact shadows are there.

Measured with `dietbench.sh` (xrmock, 1728², 4 spots in one run; medians after the first 40% of each spot). GPU = the
game's GPU time per game frame (`[pace] gpu`), eyes = `[stereo]` eye 0 + eye 1 (eye 0 includes the idle gap).

| spot | Hz | GPU off | GPU diet | eyes off | eyes diet | fps |
|---|---|---|---|---|---|---|
| Mabe (Field_11C) | 90 | 22.09 | 20.63 | 25.01 | 23.57 | 30 / 30 |
| forest (Field_06C) | 90 | 21.65 | 20.25 | 23.59 | 22.63 | 30 / 30 |
| Marin & Tarin's house | 90 | 8.70 | 8.80 | 7.96 | 7.90 | 30 / 30 |
| Tail Cave (04B) | 90 | 11.63 | 11.51 | 10.71 | 10.59 | 30 / 30 |
| Mabe | 120 | 22.17 | 20.69 | 25.23 | 23.64 | 30 / 30 |
| forest | 120 | 21.65 | 20.37 | 23.52 | 23.02 | 30 / 30 |
| house | 120 | 8.05 | 7.87 | 7.02 | 6.89 | 60 / 60 |
| Tail Cave | 120 | 11.62 | 11.54 | 10.93 | 10.73 | 60 / 60 |

At 90 Hz the game is held at 30 fps (pace_xr_lock), so only the GPU time moves. Outdoors at 120 Hz the game stays in its
30 fps mode: 20.7 ms of GPU is over a 60 fps frame, and the render thread's CPU time (12.7 ms per game frame at Mabe,
~12 of them translating eye 0 while the GPU waits: the idle gap) is close to it too.

Open: with the diet on and `LARECOMP_GPUPROF_AT` profiling a frame in Tail Cave after the field (the four-spot bench),
the eyes 50 frames later showed the room with most of its geometry missing, twice; the same run without the profile, the
diet parts alone, and Tail Cave reached from the house were all clean. A profiling-only interaction, not found yet.

Not done: AO / bloom at half size (they already are, see above); the idle gap - `LARECOMP_CHUNK_PASSES=4` made it longer
(eye 0 16.2 vs 15.0 ms at Mabe), it is the render thread translating eye 0, not the chunking.
Left for later: the scene -> RGBA16F copy (0.45 ms per eye) and the full-size depth copy (0.15) whose readers are not in
the profiled frame (the next frame's water, or the menus' backdrop?); the depth prepass (1.2 ms per eye: Turnip's LRZ
may make it redundant, but the main pass tests depth EQUAL against it).

## Dynamic eye resolution (off by default)

`LARECOMP_VR_DYNRES=1`: the eyes are drawn at a factor of the eye size inside the same targets, and the eye present
scales that part up to the whole eye image (the flat Dynamic mode's plumbing: allocation scale = the eye's, render
scale = factor times it; `output_scale.c output_scale_vr_factor`). Width `LARECOMP_VR_DYNRES_MIN` (1152; 1536 with `LARECOMP_VR_HEADROOM=0`; measured below with an earlier default, 1400) to
`LARECOMP_VR_DYNRES_MAX` (the eye width) in 32-pixel steps. Budget: the game's GPU time per game frame, 10 ms at
120 Hz (60 fps; `LARECOMP_VR_DYNRES_MS`; 14.5 with `LARECOMP_VR_HEADROOM=0`, and STEREO.md "Frame end" says why it sits well under the 16.7 ms frame) and 29 ms at 90 Hz (30 fps: fill it, so the eyes stay at full size).
Decisions every half second on the median frame; down when 2 of the last 3 were over (straight to the predicted fit
with 8% room), up one step when it is predicted under 85% for 2 s and 3 s after the last change. The HUD's box is drawn
at the same factor. The VR tab's Dynamic Resolution row sets `LARECOMP_VR_DYNRES=1` and switches it live through
`diet_set_dynres()` (on starts at full size).

| spot | Hz | GPU fixed 1728 | GPU dynamic | eye width chosen | fps |
|---|---|---|---|---|---|
| Mabe | 120 | 20.69 | 17.64 | 1400 (floor) | 30 |
| forest | 120 | 20.37 | 16.86 | 1400 (floor) | 30 |
| house | 120 | 7.87 | 7.16 | climbing 1400 -> 1560 (32 px / 3 s) | 60 |
| Tail Cave | 120 | 11.54 | 11.06 | climbing 1560 -> 1656 | 60 |
| all four | 90 | (as diet) | (as diet) | 1728: the 29 ms budget always fits | 30 |

(diet on in both columns.) The floor still misses 60 fps outdoors (17-18 ms + the CPU above), so at 120 Hz it only
costs sharpness there. Indoors it drops to the floor on the way in from the field and climbs back slowly. One extra step
at a level change: a load's light frames measured 8 ms, a raise, and the next half second's 20 ms lowered it again
(during the fade).

## CPU / GPU overlap in VR (on by default)

Turnip has no `VK_EXT_external_memory_host`, so the guest heap reaches shaders as copies (exec.c `gmir_on`), and the
overlap (the game's frame fence waited one frame late, exec.c `overlap_on`) used to turn itself off there: every frame
the game thread waited for its own frame's translation and then its GPU work (`nvnSyncWait` -> `rq_wait_item` ->
`renderer_wait_seq`), so game CPU + translation + GPU ran in series (gpu wait 6-9 ms a frame at 120 Hz, busy 18-22 ms).
In the VR mode (`stereo_wanted`) it is now on with copies:

- Nothing of the game's memory is read in place: every range is copied at translation, and the game's SyncWait still
  waits for translation (both eyes: a list's fences are signalled after eye 1), so the deferral cannot give the GPU, or
  eye 0, the next frame's data. The storage snapshots (`snap_storage`) are therefore off with copies
  (`LARECOMP_OVERLAP_SNAP=1` brings them back): with them on, the write watch took the copies' merges into guest memory
  for CPU writes and copied BowWow's skinning outputs ("-> copy (CPU writes keep coming on its own pages)").
- A deferred SyncWait merges the completed submissions' GPU stores into guest memory (`gmir_writeback`); the CPU sees
  GPU results (skinning read back, counters) one frame later, as with the overlap on any other GPU.
- Flat mode keeps the full wait: on the Frame it already runs at 60 (GPU 5-6 ms); `LARECOMP_GPU_OVERLAP=1` cut its busy
  time 8.5 -> 5.4 ms at Mabe with no fps to gain, and it was not checked by eye there. `LARECOMP_GPU_OVERLAP=0` turns
  the VR overlap off.

The pacer (pace.c, `pace_xr_frame_ms`): in the VR mode at 100 Hz and up a 30 fps hold ends when the GPU fits 90% of the
two-refresh frame (15.0 ms at 120 Hz; 84%, 14.0, once a return failed), whatever started the hold - the desktop's
13 / 10 ms limits for holds started by late frames leave room for a window image VR doesn't have, and kept the forest at
30 for good. The CPU limits scale with the frame (unchanged at 120 Hz); the 90 Hz lock is untouched.

Measured (xrmock 1728x1728, diet + foveation medium with the gaze mocked at the centre, `LARECOMP_LOG_PACE=1`; before
-> after; gpu = the pacer's GPU time per game frame, rcpu = render thread CPU, busy = game thread):

| spot | Hz | dynres | fps | gpu ms | rcpu ms | busy ms | gpu wait ms |
|---|---|---|---|---|---|---|---|
| Mabe | 120 | off | 30 -> 30 | 17.1 -> 17.0 | 12.5 -> 12.1 | 22.0 -> 10.1 | 9.4 -> 0.1 |
| forest | 120 | off | 30 -> 30 | 16.2 -> 16.1 | 10.9 -> 10.8 | 20.2 -> 9.1 | 9.3 -> 0.0 |
| house | 120 | off | 60 -> 60 | 7.2 -> 7.2 | 2.9 -> 2.8 | 7.9 -> 2.4 | 5.4 -> 0.0 |
| Tail Cave | 120 | off | 60 -> 60 | 10.0 -> 9.8 | 3.9 -> 3.9 | 10.9 -> 3.7 | 7.2 -> 0.0 |
| Mabe | 120 | on | 30 -> 60 | 15.2 -> 15.6 | 12.5 -> 11.8 | 20.2 -> 12.2 | 7.6 -> 0.0 |
| forest | 120 | on | 30 -> 60 | 14.3 -> 14.3 | 11.0 -> 10.3 | 18.3 -> 10.6 | 7.4 -> 0.0 |
| house | 120 | on | 60 -> 60 | 6.6 -> 6.6 | 2.8 -> 2.8 | 7.3 -> 2.5 | 4.8 -> 0.0 |
| Tail Cave | 120 | on | 60 -> 60 | 9.5 -> 9.3 | 3.9 -> 3.9 | 10.4 -> 3.7 | 6.8 -> 0.0 |
| Mabe | 90 | off | 30 -> 30 | 17.0 -> 17.0 | 12.5 -> 12.1 | 21.9 -> 10.1 | 9.3 -> 0.1 |
| forest | 90 | off | 30 -> 30 | 16.2 -> 16.1 | 10.9 -> 10.5 | 20.2 -> 9.0 | 9.3 -> 0.0 |
| house | 90 | off | 30 -> 30 | 8.8 -> 8.8 | 3.2 -> 3.3 | 2.8 -> 2.9 | 0.1 -> 0.0 |
| Tail Cave | 90 | off | 30 -> 30 | 9.8 -> 9.8 | 4.4 -> 4.5 | 4.1 -> 4.2 | 0.1 -> 0.1 |

At 90 Hz dynamic resolution keeps the full size (29 ms budget), so its rows equal these. At 120 Hz with dynres, Mabe at
the 1400 floor shows 296-299 of 300 sets in 5 s; the forest is held for ~5 s after the warp from Mabe (late frames
during the load), then released ("enough headroom ... gpu 14.3 ms") and stays at 60 (started there: 60.0 fps for 70 s).
Without dynres outdoors still doesn't fit 60 (GPU 16-17 ms) and is held at 30 at 120 Hz. At 60 the render thread's
~11-12 ms of translation is what the game thread still waits for (queue wait ~4.5 ms at Mabe): the next serial part.
`LARECOMP_VR_DYNRES_MS=13.5` changed nothing: outdoors the eye width is at the 1400 floor either way.

Far drawing (lane/ring, `LARECOMP_VR_CULL_EXTENT=52,42 LARECOMP_VR_RING=4`) at 90 Hz, overlap off -> on:

| spot | ring | fps | gpu ms | rcpu ms | busy ms |
|---|---|---|---|---|---|
| Mabe | 4 | 23.0 -> 30.0 (150 of 150 sets / 5 s) | 22.3 -> 22.6 | 21.6 -> 20.8 | 32.0 -> 21.7 |
| forest | 4 | 20.0 -> 28.4 (140-146 of 150) | 26.4 -> 26.9 | 27.8 -> 27.9 | 38.4 -> 29.0 |
| Mabe | 4, cast 12 | 30.0 (150 of 150) | 22.6 | 20.9 | 21.7 |
| forest | 4, cast 12 | 29.1 (140-147 of 150) | 27.4 | 27.4 | 28.1 |
| house / Tail Cave | 4 | 30 / 30 | 8.7 / 9.2 | 3.5 / 6.3 | 2.8 / 4.4 |

With the overlap the GPU and the render thread run side by side: Mabe holds 30 with the far world drawn; the forest is
render-CPU bound (27-28 ms of translation, queue wait ~9.5 ms) and drops a few frames.
