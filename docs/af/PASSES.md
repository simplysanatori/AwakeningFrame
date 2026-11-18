# VR render passes and the pass diet

Where a VR game frame's GPU time goes, both eyes, and what VR mode leaves out (`runtime/nvn/vulkan/vr_diet.c`).

## How it was measured

`LARECOMP_GPUPROF_AT=<frame>` under xrmock (1728² eyes, 90 Hz, game at 30 fps), a local bench script around it
(not in this repository). Each `[gpuprof]` row is the GPU time from one mark (a pass, copy or dispatch) to the next.
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
Left for later: the depth prepass (1.2 ms per eye: Turnip's LRZ may make it redundant, but the main pass tests depth
EQUAL against it). The copies' readers are in "Clears and copies" below.

## Clears and copies (VR mode only)

What a game frame asks of the GPU, counted, and four savings on the clears and copies it holds, each behind its own
switch (`vr_diet.c` `vr_saving`; the `[diet] VR savings:` line at start names the ones on).

Tools:
- `LARECOMP_PASSLOG=<frame>[,<frame>...]` (`passlog.c`; `LARECOMP_PASSLOG_FRAMES`, default 2): `[plog]` lines for
  every pass (size, attachments, load ops, view mask, draws), clear, copy, snapshot and dispatch of those frames.
  `tools/passlog.py run.log [--md] [--counts] [--frame N]` makes the per-pass and per-copy tables and the counts.
- `LARECOMP_COPY_READERS=1`: every 10 s a `[copyread]` line per kind of copy: whether the destination was read (in the
  same frame, the next, later) or overwritten first, and by whom, over the whole run.
- `tools/mv_check.sh --mode env --base-env '...' --cand-env '...' [--cand-env '...'] [--env '...'] [--more]`: a switch
  on and off, several candidates against one base and noise run; `--base-only` for an audit run. `--xr` compares the
  eye images as the headset gets them (Table Edge applied, `MOCK_XR_SHOT_REQ`) instead of the eye outputs before the
  edge pass; `--spot 'NAME|LEVEL:LOCATOR|'` (repeatable) replaces the spot list; `--base-bin PATH` runs the base and
  noise sessions on another binary (keep a copy next to `lasgame.so`, e.g. `build/lasrecomp.base`: the game code and
  the fonts are found beside the binary).

Mabe at frame 1320 (Frame-like profile, 1728² eyes): 41 passes (37 multiview), 15 of them only clears, 1087 draws, 14
copies (7 full-size layer copies), 5 dispatches. The copies and their readers over a whole run:
- main depth -> depth copy A: read every frame at Mabe (the water), in the forest and in Tail Cave; not in the house.
- main depth -> depth copy B: its depth is cleared right after (stencil kept) and nothing reads it.
- scene -> RGBA16F: read only in Tail Cave 04B (every frame there), made everywhere; its source is cleared at the next
  frame's start, before the next copy, so a lazy copy would be made at that clear anyway; formats differ, so no alias.
- window image -> the presented texture (full size): never read in VR gameplay (the HUD clone is presented).
- window image -> thumbnails (682x154, 777x243) -> 136x30 / 155x48: the last ones never read; small.

The savings (all VR only, flat unchanged; on by default: the alias):
- `LARECOMP_VR_CLEAR_DEFER=1` (off by default; it turns `LARECOMP_VR_CLEAR_FOLD` on, and `LARECOMP_VR_CLEAR_FOLD=0` turns
  both off): a pass that only clears is not begun. Its clears wait and become the load op of the next pass on the same
  view, view mask and area; otherwise they are made, as the pass they were, before anything else touches the texture
  (a draw sampling it, a copy, a dispatch, an upload, the eye composite, weather, the end of each eye's list).
  Mabe 41 -> 35 passes, forest 44 -> 37, house 31 -> 28, Tail Cave 38 -> 34; full-size passes at Mabe 14 -> 8.
- `LARECOMP_VR_CLEAR_FOLD=1` alone (off by default): the pass begin waits for its first draw, so a whole clear before
  it becomes that attachment's load op, and a pass with nothing in it is not recorded. It removes no pass by itself
  (every empty pass holds a clear); an earlier test on the Frame found load ops alone gain nothing on Turnip.
- `LARECOMP_VR_COPY_ALIAS` (on; `0` off): the flat mode's depth copy alias (`LARECOMP_VK_COPY_OPT`) in a multiview
  run: both layers of a layered texture at once, never with per-eye twins. At Mabe depth copy A becomes a rename at
  the main depth's next clear and copy B a stencil-only copy at its own depth clear (7 -> 4 full-size layer copies).
  In the forest the main depth is written after copy A, so A is copied then (no saving there).
- `LARECOMP_VR_PRESENT_COPY=1` (off by default): the game's copy of its window image into the texture it presents is
  left out while the HUD clone is presented (one full-size copy a frame). Off: the window is not compared by the image
  checks, and the frames that present the texture itself (warps, loading) are not covered.

Checked with `mv_check --mode env --more` (eight spots: Mabe, forest, the house, Tail Cave, the beach, the menu open at
Mabe, in the house and in Tail Cave; the Frame-like profile `tools/frame_profile.env` + `frame_profile_copies.env`):
the deferral and the alias each alone, and all four together, pass at every spot, both eyes; again with Table Edge
Fade and Surroundings Backdrop, and in snow at Mabe (`LARECOMP_WEATHER=rain` drew no rain under the mock: the same
image as without weather, so rain is not covered). The fold alone and the present copy alone failed spots the same
way, on moving actors. Under the validation layer, a tour with all four on and the eye size stepping through 1152,
1536 and 1680 gives the same messages as with all four off. `LARECOMP_CHUNK_PASSES=0` passes too.
`LARECOMP_SHADOW_SCALE=0.5` changes the image (shadow edges: 11 % of the forest's pixels). The game is not deterministic:
a Mabe spot can fail any of these checks by up to ~2 % on chickens, a fox and butterflies standing elsewhere (rerun it).
The counts are of the game's own commands; the runtime's own passes (weather, the edge, the eye composite) are not in
the pass log.
Only the headset can show: the time saved (see "Measured on the Frame" below), the held pass begin with
Valve's density map layer (this desktop has no foveation), and Turnip's tiles.

Measured on the Frame (2026-11-18, `steam-frame/dev/frame-bench.sh` variants: the mock runtime, no SteamVR, eyes pinned
at 1728x1728, Table Edge Fade, foveation Medium with a centred gaze; GPU ms of a whole game frame, one 9 s sample each;
the same settings run twice differed by 0.1 ms at Mabe and 0.2 ms in the forest):

| | Mabe | Mysterious Forest |
|---|---|---|
| every saving off | 14.96, 14.89 | 13.36, 13.13 |
| the alias alone | 14.57 | 13.26 |
| the deferral (with the fold) | 15.86 | 14.00 |
| the deferral and the alias | 15.69, 15.58 | 13.89, 14.10 |
| those two and the present copy | 15.53 | 14.12 |

So the deferral costs 0.7 to 0.9 ms there although it removes six passes, and it is off by default; the alias saves
0.1 to 0.4 ms. With the deferral and the alias on: `LARECOMP_CHUNK_PASSES=0` 15.37 / 13.98 (0.7 ms less render CPU at
Mabe), `LARECOMP_SHADOW_SCALE=0.5` 15.51 / 13.70, Table Edge off 14.86 / 13.51, the edge reading the quarter-size depth
(`LARECOMP_VR_EDGE_DEPTH=quarter`) 15.37 / 13.71, FXAA 16.41 / 14.61, SMAA 17.12 / 15.18.

## Draws the eyes cannot see (VR, overworld; on by default)

The game culls on its game thread with one frustum: in VR the table's culling camera (`vr_cull.c`, STREAM.md), a
rectangle about the table's centre (+-22.3 x +-23.6 units outdoors) cut to the bounding rectangle of what both eyes see
of it. A frustum can only give a rectangle. The executor (`exec.c`, "the draws the eyes cannot see") knows more for a
draw with a world box (`ring_world_box`: the vertex box through block 2's model matrix, or the bone palette's matrix
for a skinned or attached mesh): this frame's own eye cameras (`cam_override_eye_refs`) and, with the round fade
(EDGE.md "The fade's rows"), where Table Edge Fade is black (`edge_fade_disc`). A draw whose box (padded 0.25 units,
2 for a palette mesh) is

- **view**: outside both eyes' frusta (left, right, bottom, top, near planes of each eye), or
- **fade** (round fade only): wholly past the black circle plus 1 unit, while both eyes are inside the circle (then
  nothing past it can stand in front of anything inside: the fade is a cylinder). A box reaching under the floor plane
  is taken where the eyes' rays through its bottom meet the plane (cleared depth reads that point; water drawn over it),

is left out of the eyes' full-size passes, the depth pre-pass and the colour pass alike (the class is memoised per
frame, so the EQUAL test of the colour pass meets the same set), and kept in the small passes. A **shadow** caster
(the 2048 x 2048 shadow pass, shared, drawn before the frame's camera is latched) is left out when its box, swept along
the light (the shadow pass's orthographic block 1: its depth row) down to the lowest box the eyes kept last frame less
1 unit, lies outside both of the previous frame's eye frusta widened by 0.3 NDC (as `vr_cull.c`'s margin), or past the
round fade's circle. Kept: blended, instanced and unplaceable draws, a box whose matrix sits at the world's origin (a
mesh placed another way), everything indoors and in side views, the Surroundings' ring draws (Low, Full) and the frames
around a Backdrop capture. Table Edge Off and Passthrough get view and shadow (both leave the picture as it was).

| Variable | Meaning |
|---|---|
| `LARECOMP_VR_CULL_DRAWS` | unset / `1`: all; `0`: none; or a list of `view`, `shadow`, `fade` (any separator: `view+shadow`). `fade` acts only with `LARECOMP_VR_EDGE_ROUND=1` |
| `LARECOMP_VR_CULL_DRAWS_MARGIN=<units>` | the fade's margin (1) |
| `LARECOMP_VR_EDGE_ROUND=1` | (off; EDGE.md) the fade made round: then the fade cull, and the table's culling camera cut to the square the fade can light (`vr_cull_note_fade`, `LARECOMP_VR_CULL_FADE=0`: not) |
| `LARECOMP_VR_EDGE_BLACK=<units>` | (off) a smaller round table: with the round fade, the game's culling follows it |
| `LARECOMP_VR_DRAWLOG=<frame>[,...]`, `LARECOMP_VR_DRAWLOG_FRAMES=<n>` | every draw of eye 0's run of those frames: pass, count, instances, blend, box, class; the frame's table, fade, light and eyes. `tools/cull_check/drawlog.py <log> [--mirror]`: per pass, a histogram by distance from the table's centre, what lies past the clear / black radius or outside both views, the casters' sweep, the trade for smaller radii (`--mirror`: the fade as drawn without the round fade) |

A `[seen]` line every 10 s gives the draws left out per frame. Measured (xrmock, the owner's rig: size 3, distance 1.4,
tilt 25, height -0.31, Follow Smooth; Table Edge Fade, eyes 1728 x 1728, `MOCK_XR_FOV=-52,44,50,-54` mirrored, head
seated 25 degrees down; draws of the eyes' full-size depth pass left out of all, indices in thousands per pass; casters
of the shadow pass):

| spot | default (view, shadow) | with `LARECOMP_VR_EDGE_ROUND=1` |
|---|---|---|
| Mabe (Field_11C) | eyes 77 of 260 (216k of 857k), casters 18 of 282 | eyes 109 of 258 (325k of 850k), casters 101 of 280 |
| Mysterious Forest (Field_06C) | 51 of 184 (117k of 641k), 20 of 185 | 74 of 184 (219k of 641k), 43 of 185 |
| beach (Field_16E) | 60 of 220 (142k of 514k), 15 of 234 | 81 of 218 (172k of 508k), 39 of 232 |
| Animal Village (Field_10M) | 31 of 257 (54k of 490k), 11 of 270 | 49 of 261 (149k of 503k), 32 of 274 |

With the head along the table (45 degrees left or right) more lies outside the views: Animal Village 100-137 of about
260 eye draws, Mabe 93-111 (drawlog.py on the cull-off runs). The game thread is unchanged (its cull is the table's);
the render thread records that many fewer draws. Checked with `mv_check --mode env --xr` (the eye images after Table
Edge) at Mabe, the forest, the beach, Animal Village, the house and Tail Cave 04B, seated and along the table, with
Fade, Off and Passthrough and with Backdrop: equal but for the run-to-run noise (moving characters, flames; a null
candidate, the base's own settings again, differs as much).

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
30 for good. The CPU limits scale with the frame (unchanged at 120 Hz). Under the 90 Hz lock (`pace_xr_lock`) the pacer
judges nothing: its frames are 33 ms because the game is held, and counted as shown late they started holds of the
pacer's own that backed off under the lock (to 8x in a session on the Frame on 2026-11-18, which then switched to 120 Hz
and stayed at 30 with the GPU at 11 ms). Each change of the display's rate or of the VR tab's 90 Hz choice (a
`pace_xr_lock` / `pace_xr_frame_ms` call) starts the pacer afresh: hold, backoff and counted frames cleared, 60 tried,
a second of grace for the switch's own frames. `tools/pace_replay.sh <log>` plays a `LARECOMP_LOG_PACE=1` log's frames
through the pacer (`--as-logged`: as they were; by default a frame whose hold differs from the session's is remade from
its work).

The 30 fps cadence in VR (pace.c `vr_cadence`, vi.c `g_vsync_game_from`): held at 30 (the 90 Hz lock,
`LARECOMP_XR_FPS=30`, or the pacer's hold at 120 Hz) a game frame is due two vsyncs after the one before. The game
works, waits for its next vsync and presents; the pacer's old cadence started each frame a vsync after its present, so
the work had one vsync, not two, and a frame whose work crossed it was shown a vsync late (50 ms), the next 16.7 ms after
it. On the Frame at Mabe Village that was 1.3% of frames, 8.1% with `LARECOMP_XR_REPROJ=pos`. Now the game's vsync
event is not signalled at the vsync between (the frame due at S + 2 keeps working through S + 1), and a frame starts as
early as its work needs: its window to S + 2 is the third-highest busy time of the last 120 frames plus 3 ms, at least
the old late start (no latency added) and at most both vsyncs (the start is the present); the hold's first 120 frames
and the 120 after a late one take both. The start moves the game's input (read at the frame's start), not the eye
poses: those are located at the present (0.1 to 0.7 ms after it, either start). A frame over two vsyncs is shown at the
vsync after its work and the next is due two vsyncs after that (33.3 ms, not 16.7). The `[pace] VR 30 fps:` lines say
when frames start before the late start and when late again. The flat window keeps the old cadence. Test switches:
`LARECOMP_PACE_CADENCE=early|late` (always both vsyncs / always the late start), `LARECOMP_PACE_TEST_WORK_MS=<ms>[,<every>]`
(the game thread busy-waits that long at the start of every frame or every n-th). `tools/pace_replay.sh <log> --cadence
--idle <ms> --from <text>` replays a log's 30 fps frames on a simulated vsync clock.

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

Far drawing (ring draws, `LARECOMP_VR_CULL_EXTENT=52,42 LARECOMP_VR_RING=4`) at 90 Hz, overlap off -> on:

| spot | ring | fps | gpu ms | rcpu ms | busy ms |
|---|---|---|---|---|---|
| Mabe | 4 | 23.0 -> 30.0 (150 of 150 sets / 5 s) | 22.3 -> 22.6 | 21.6 -> 20.8 | 32.0 -> 21.7 |
| forest | 4 | 20.0 -> 28.4 (140-146 of 150) | 26.4 -> 26.9 | 27.8 -> 27.9 | 38.4 -> 29.0 |
| Mabe | 4, cast 12 | 30.0 (150 of 150) | 22.6 | 20.9 | 21.7 |
| forest | 4, cast 12 | 29.1 (140-147 of 150) | 27.4 | 27.4 | 28.1 |
| house / Tail Cave | 4 | 30 / 30 | 8.7 / 9.2 | 3.5 / 6.3 | 2.8 / 4.4 |

With the overlap the GPU and the render thread run side by side: Mabe holds 30 with the far world drawn; the forest is
render-CPU bound (27-28 ms of translation, queue wait ~9.5 ms) and drops a few frames.
