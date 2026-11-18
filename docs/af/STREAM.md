# Overworld streaming and the diorama's black voids

In VR you look around the table and see black where the game shows nothing. This page says what limits the visible world
on the overworld, how to measure it and which levers exist. Addresses are for v1.0.0 (build AE16F71E) only.

## How the overworld is loaded

- The overworld (level `Field`) is a 16 x 16 grid of rooms, `Field_<row><col>`: row `01`-`16` from north to south,
  column `A`-`P` from west to east. Each room is one `map/Field_XXX.bfres` (about 1 MB: model and textures) and one
  `level/Field/Field_XXX.leb` (actors, collision). There are 256 of each, 252 MB of map files in all.
- World coordinates: a room is 15 x 12 units (x = column index x 15, z = row index x 12; x grows east, z grows south).
  The whole overworld is 240 x 192. Mabe Village's Field_11C covers x 30-45, z 120-132 (Link spawns at 38.25, 128.25).
- **The game keeps a 7 x 7 block of rooms around Link's room resident.** On a level load it reads the 49 `.leb` files and
  then the 49 `.bfres` files, nearest first. At the map's edges the block slides so it stays 7 wide (column C gives A-G).
  When Link crosses into a new row or column, the 7 rooms of the new strip load within a few frames, also nearest
  first, on a loader thread: guest `f_7000fbdcd0` -> `f_7000fbddd0` -> `f_7000fcbf60` -> sdk fs (`LARECOMP_STREAM_BT`).
  Measured with `LARECOMP_LOG_FILES=1` and `LARECOMP_PLACE` teleports. So about +-45 x and +-36 z around Link is always
  in memory: 3 rooms on each side of Link's room.
- The place that picks the block (Link's room -> 7 x 7) has not been found yet. Leads are below.

## What makes the voids

There are three limits, from the nearest to the farthest:

1. **The game's CPU culling.** The game culls once per frame with two frustums, both built by `f_700112f5c0` from a
   camera matrix and a fovy, aspect, near and far: the visibility pass's (`f_70008529b0`, the camera's `+0x610`, the
   `CULL_ASPECT` / `FOVY_SITES_1` values), which culls the level's own models (a room's ground and walls are a few big
   models), and the engine main camera's (its `+0xe0`, built at `0x7001026830`), which culls everything else: trees,
   houses, characters, props, effects. In VR `runtime/nvn/vr_cull.c` decides what both take in (below). Geometry that is
   loaded but outside it is not drawn.
2. **The streamer's 7 x 7 block**. Past +-3 rooms nothing is loaded. You only see this once the culling is widened.
3. The camera far plane (100 outdoors, 45 indoors; floats 72-75 of the camera buffer). This matters only for long
   pull-backs.

## What the table draws (`vr_cull.c`)

A frustum from the game's own camera is symmetric about that camera's line of sight, and the camera stands 17.7 units
south of its floor point and as high, looking down at 45 degrees. Fitted to the stock screen's footprint as both eyes see
it, its floor reached 23 units north of the table's centre and 7 to 10 south; to reach as far south it would have had to
open to 37 units north. From the game's own side that was enough. From the table's other sides, or over a flat tilt, the
world ended in a straight cut close behind Link, and what it showed north changed with the head (looking down at the near
edge widened the far one). With Link in the north part of a room, the whole room south of his was missing: Mabe
Village's weathercock plaza (Field_10C, Link at 30.4, 108.6) seen from the north had black where Marin's house is.

So the default hands both frustums another camera (`table_fit`, `vr_cull_camera`; `tools/patches.py`
`CULL_FRUSTUM_SITE`, `MAIN_FRUSTUM_SITE`): 200 units above the table, looking straight down, just wide enough for a
rectangle about the table's centre along the game camera's ground axes.

- The footprint: 1.5 x the stock screen's widest half-width and 2 x its far reach on the floor, on every side
  (`LARECOMP_VR_CULL_TABLE`; outdoors +-22.3 x +-23.6 units, 3 x 4 rooms). 23.6 is what the game camera's fit showed north.
- Each frame it is cut down to the part both eyes can see (their frusta widened by `LARECOMP_VR_CULL_MARGIN`, 5% and half
  a unit more), and never below the stock screen about the game camera's floor point: the main camera's frustum also
  culls characters and effects, and those must not come and go with the head.
- It is kept relative to the game camera's floor point, so the frame or two between the fit and its use, and a camera
  cut, do not move it. `vr_cull_drawn` gives the edge fade the footprint (the cut only leaves out what no eye sees).
- The main camera's frustum is built for every camera of the engine's kind; only one whose matrix is the visibility
  pass's camera (within a frame's movement) is replaced. A reflection's keeps its own.
- The fitted tangents of the game camera's frustum are still made and still widen the game camera's own projection, as
  before; nothing is culled with them.
- `LARECOMP_VR_CULL=camera` is the fit before this (the game camera's frustum on the stock screen's footprint). The
  Surroundings' box (`LARECOMP_VR_CULL_EXTENT`, Low, Full, Backdrop's capture frames) and side views keep the game
  camera's frustum too.
- `LARECOMP_LOG_CULL=<n>` logs every n frames (1: every 120): the game camera, the floor rectangles, each eye's position
  and view-projection, the fitted tangents and the table's rectangle with the frustums built from it.

### Checking it (`tools/cull_check/`)

`sweep.sh <out> <Field locator>` runs one mock session at a spot through 32 views: table yaw 0 / 90 / 180 / 270, tilt
0 / 15 / 30 / 50, the head where a seated player's is (0.8 m from the table's centre, looking at it) and leaning low
(20 cm nearer, 10 cm lower). The rig stays at tilt 30 and yaw 0; the head is moved instead, which puts the eyes at the
same place in the game world (`cases.py`). `report.py <test> <reference> <base>` then gives, per view and eye, the share
of the pixels whose floor point lies inside a footprint that are black here and drawn in the reference (every resident
room: `LARECOMP_VR_CULL_TABLE_UNITS=52,42 LARECOMP_VR_CULL_EYES=0`). A dump counts only while the cull log shows a
live camera latch and a still head. `overview.sh` shows the table from 2 m above.

Measured (xrmock, 120 Hz, 1728 x 1728 eyes, fov +-48 degrees, dynamic resolution off, RTX 3080):

| spot | the game camera's fit: worst views, missing in the table's footprint | the table's camera, same views |
|---|---|---|
| Mabe's weathercock plaza, Link at its north side (30.4, 108.6) | yaw 180 tilt 50: 9.5%; yaw 90 tilt 50: 6.0%; yaw 180 tilt 30: 4.3% (the room south, Field_11C, not drawn) | 0.0%, 0.1%, 0.0% |
| the woods' edge, Field_09C (38.3, 104) | yaw 180 tilt 50: 11.2%; yaw 180 tilt 30: 6.3%; yaw 90 tilt 50: 5.8% (Mabe's houses and trees past the old frustum) | 0.7%, 0.6%, 0.0% |
| Mabe's centre (38.3, 128.3) | 1.4% at most | 1.0% at most |
| Field_07C | 0.6% at most | 0.4% at most |

Inside the old fit's own floor quadrilateral (the stock screen x 1.2 x 1) neither misses anything: what was missing lay
south of it and beside its narrow south end. A whole room south of Link's goes missing only while Link stands in the
north part of his room (more than about 8 units from its south side): the level's models are a room big, so nearer
the boundary the room south reached into the old frustum anyway, but its trees and houses still did not. Views with the
head inside a tree read up to 5% from near-black pixels in either fit (checked by eye: nothing missing).

Cost at the stock view (yaw 0, tilt 30, the seated head; three sessions each, mean and range):

| spot | draws per game frame, before | after |
|---|---|---|
| Mabe's centre | 941 (934-954) | 1001 (994-1005), +6% |
| woods' edge, Field_09C | 1012 (999-1021) | 988 (981-993), -2% |
| Mysterious Forest, Field_06C | 747 (741-757) | 683 (681-686), -9% |

The game thread's time moved with the draws and stayed inside the spread between sessions on a shared machine (Mabe:
2.7 to 4.8 ms before, 2.9 to 5.8 after; the quietest sessions 2.68 and 2.90).

From the other sides the table now holds what the old fit left out, and that costs. At the weathercock plaza, where the
old fit drew nothing south of Link (`views_cost.py`, each view held three times, no dumps):

| views | draws per game frame, before -> after | game thread ms on this desktop, before -> after |
|---|---|---|
| from the south (yaw 0) | 773-777 -> 807-939, +4 to +21% | 1.9 -> 2.3-2.7 |
| from the north (yaw 180) | 713-752 -> 889-957, +20 to +28% | 1.8 -> 2.6-2.7 |
| from east or west (yaw 90, 270) | 773-782 -> 1001-1036, +29 to +33% | 1.9 -> 2.9-3.2 |

`LARECOMP_VR_CULL_TABLE=1.25,1.5` (+-18.6 x +-17.7 units, about the old fit's floor area made symmetric) is the cheaper
table: the same views take 3 to 24% fewer draws than the old fit and 1.8-2.4 ms of game thread, at the price of 6 units
less to the north and 4 to each side than the default. Draws undercount the village's instanced grass and its actors,
which is why the game thread rises more than the draws.

Limit: the game's shadow map covers the stock view. South-east of it (from about 12 units out) the ground now drawn
samples past the map's edge and shows the edge's shadows drawn out in stripes, as with the Surroundings' box before.

## Levers

| Variable | Effect | Default |
|---|---|---|
| `LARECOMP_VR_CULL=fit\|camera\|wide\|stock` | the table's culling camera; the game camera's frustum fitted to the stock screen's footprint; a fixed 3.2 : 1.5 widening; the game's own | `fit` |
| `LARECOMP_VR_CULL_TABLE=<kx>,<kz>` | the table's half-extents: the stock screen's widest half-width and far reach times these (`_TABLE_UNITS=<x>,<z>`: in units; `52,42` with `LARECOMP_VR_CULL_EYES=0` draws the resident 7 x 7) | 1.5,2 |
| `LARECOMP_VR_CULL_EXTENT=<x>,<z>` | the game camera's frustum fitted to the world-axis box of these half-extents about the table centre; `52,42` covers the resident 7 x 7 | off |
| `LARECOMP_VR_CULL_TMAX=<tan>` | the widest half-angle tangent the fit may ask for | 4 (76 degrees) |
| `LARECOMP_VR_RING=<margin>` | ring draws (below): draws whose world box misses the table fit's footprint grown by this go into the eye colour passes only | off |
| `LARECOMP_VR_RING_CAST=<margin>` | ring draws within this of the footprint still cast shadows | 0 (none) |
| `LARECOMP_VR_RING_LOWRES=0` | ring draws also stay out of the eyes' smaller colour passes (the 704 x 396 one) | 1 (drawn there) |
| `LARECOMP_FREECAM_CULL=<aspect>:<fovy scale>` | flat free-camera probe: a fixed widened culling (docs/af/FREECAM.md) | stock |

Both VR overrides change only what is drawn. Nothing extra is loaded and no game logic sees them.

### Measured (xrmock, 90 Hz, 1680 x 1760 eyes, head static 45 degrees down; `tools/stream_vr.sh`)

These tables are from the game camera's fit ("table fit" is `LARECOMP_VR_CULL=camera` now).

| spot | culling | draws / game frame | GPU ms / game frame (both eyes) | game fps | eye void % | RSS MB |
|---|---|---|---|---|---|---|
| Mabe Village | table fit (default) | 766 | 21.9 | 30 | 21.4 | 1689 |
| Mabe Village | EXTENT 52,42 | 944 | 33.3 | 15 | 18.2 | 1691 |
| Mysterious Forest | table fit | 614 | 21.5 | 30 | 28.2 | 1794 |
| Mysterious Forest | EXTENT 52,42 | 1087 | 36.2 | 15 | 21.0 | 1799 |

`TMAX=8` gives the same numbers as `TMAX=4`: the fit reaches tan 1.5 at most from the table pose. With EXTENT, the
black patches beside the near field and the straight cut past the north rows are filled (Mabe's library and the north
tree rows show). What stays black is the band past the far edge of the geometry, which is limit 2 or 3. The cost is the
problem: the extra draws push the game from its 30 fps mode to 15 fps in both spots.

### Where the extra GPU time goes (`LARECOMP_GPUPROF_AT`, Mabe, VR, 3 frames each)

| group (per game frame) | table fit: ms / draws | EXTENT 52,42: ms / draws | delta |
|---|---|---|---|
| shared depth-only pass (fs d3e83e0301de7e42, shadow casters?) | 0.79 / 282 | 1.56 / 562 | +0.8 |
| shared 704x396 pass right after it (no draws of its own: shadow mask / low-res lighting?) | 4.06 | 9.82 | +5.8 |
| per eye x2: depth prepass (fs d853b93d3755364b) | 1.2 / 238 each | 4.1 / 600 each | +5.8 |
| per eye x2: main colour (fs f0b2d796cbb7ffce) | 4.4 / 238 each | 6.2 / 600 each | +3.7 |
| per eye x2: low-res pass with depth (fs 6804ba6e5340c6f9) | 0.26 / 56 each | 0.56 / 127 each | +0.6 |
| post, copies, dispatches | 7.4 | 7.6 | +0.2 |
| total | 24.7 | 43.4 | +18.7 |

The game culls once per frame (the visibility pass in `f_70008529b0`, the `CULL_ASPECT` site 0x7000852ad4) and every
pass takes its list from that cull. So the widened frustum also feeds the shadow-caster pass and both eyes' depth
prepasses: about 70 % of the extra time is outside the eye colour passes. Keeping the game's cull for shadow / depth-only
passes needs either a second cull in that pass (one list for the shadow and depth passes, one for colour) or a per-draw
position test in the executor, and a depth prepass that misses draws breaks the colour pass's depth test. Neither is
done here.

## Ring draws: the world past the table at a lower cost

`LARECOMP_VR_RING=4` with `LARECOMP_VR_CULL_EXTENT` draws the extra rooms in the eyes' colour passes only. The executor
(`exec.c` `ring_route`, before `begin_pass`) sorts each draw into core or ring:

- Its world box is the box of the vertices it uses (vertex attribute 0, memoised per mesh) through its model matrix.
  Level draws (vertex block 4 of 912 / 928 bytes) carry that matrix in block 2 (3 x 4 rows, the room's centre as the
  translation: room-local vertices). Characters and props (1056 / 1072) have a zero translation there and, as u32 12, the
  index of their matrix in block 3, the world-space bone palette (Link's first bone is his position). Block 3 on a level
  draw is stale state from an earlier character draw and means nothing.
- Ring: the box misses the table fit's footprint (`vr_cull_fit`'s rectangle before the EXTENT box replaces it, as a
  world-axis box, taken once per frame) grown by the margin. Instanced draws, blended draws in colour passes and
  anything without one of those blocks (HUD, post, effects) stay core.
- Routing: in a shared depth-only pass (no twinned attachment: the shadow casters), a ring draw is skipped (`LARECOMP_LOG_SKIPS`
  reason "ring draw"), unless it is within `LARECOMP_VR_RING_CAST` of the footprint. In an eye's depth-only pass (the
  pre-pass and the 704 x 396 pass's own) it is skipped. In an eye's colour pass its EQUAL test becomes LESS_OR_EQUAL
  with depth write: right in any order, since the core draws were in the pre-pass and a ring draw nearer the eye wins
  either way.
- `LARECOMP_VR_RING_LOG=1`: per pass kind the draws of each class every 300 frames (`[ring]`).
  `LARECOMP_VR_RING_DUMP=<frame>`: that frame's draws with their blocks, local and world boxes.

At Mabe with EXTENT 52,42, each eye's colour pass has about 225 core draws (the table fit drew 238) and 560 ring draws.
The shared and pre-pass rows get none, and the pre-pass is back to 139 draws, 1.15 ms an eye.

### Measured (xrmock 90 Hz, 1680 x 1760, head static 45 degrees down; `tools/stream_vr.sh` timing; fps = the game's)

| spot | config | GPU ms / game frame | game fps | render CPU ms | eye void % |
|---|---|---|---|---|---|
| Mabe | table fit | 22.1 | 28.8 | 12.7 | 21.3 |
| Mabe | EXTENT 52,42 | 33.3 | 14.6 | 28.4 | 18.2 |
| Mabe | EXTENT 52,42 + RING 4 | 27.6 | 19.5 | 21.2 | 18.0 |
| Mabe | EXTENT 52,42 + RING 4 + CAST 12 | 27.9 | 19.3 | 22.9 | 18.0 |
| Mabe | EXTENT 52,42 + RING 4 + LOWRES 0 | 27.0 | 19.3 | 19.7 | 18.0 |
| Mabe | EXTENT 36,30 | 32.7 | 14.8 | 26.4 | 18.1 |
| Mabe | EXTENT 36,30 + RING 4 | 27.1 | 19.3 | 20.6 | 18.0 |
| forest | table fit | 21.4 | 28.6 | 10.9 | 28.2 |
| forest | EXTENT 52,42 | 36.3 | 14.7 | 35.5 | 20.9 |
| forest | EXTENT 52,42 + RING 4 | 33.2 | 15.0 | 27.3 | 21.1 |
| forest | EXTENT 52,42 + RING 4 + CAST 12 | 33.5 | 14.6 | 27.8 | 21.0 |
| forest | EXTENT 52,42 + RING 4 + LOWRES 0 | 29.8 | 19.3 | 22.0 | 21.1 |
| forest | EXTENT 36,30 | 35.9 | 14.6 | 32.0 | 21.1 |
| forest | EXTENT 36,30 + RING 4 | 32.8 | 15.1 | 26.8 | 21.4 |

- The void % is the EXTENT run's in every ring configuration: nothing goes missing, and the eye images show no holes
  or z-fighting at the table edge. What changes is the shadows: ring objects cast none, so the ground just past the
  footprint (behind Mabe's north wall, under the north trees and the library) is lit. `CAST 12` brings them back for
  +0.3 ms (about 120 more casters).
- 36,30 costs the same as 52,42 from this pose: the eye frusta, not the box, bound what the fit asks for.
- Turnip's LRZ is not a factor: `TU_DEBUG=nolrz` gives 28.0 (ring) and 33.1 (EXTENT only) at Mabe.
- Where the time goes (forest, both eyes): the pre-pass 5.2 -> 1.6 ms and the casters 1.7 -> 0.6, but the main colour
  pass 15.6 -> 17.3 (ring draws are shaded in draw order, without the pre-pass's EQUAL: overdraw). `LOWRES 0` also
  keeps them out of the 704 x 396 pass (1.7 ms an eye): no visible difference in the dumps, but what that pass feeds
  is not known, so it is opt-in.
- The 704 x 396 row of the breakdown above is not the casters: with fs d3e83e0301de7e42 skipped (the casters
  went from 565 to 147 draws) it stayed at 11 ms. It is the last row before a submission boundary in eye 0 only, so it
  holds the GPU's idle time while the render thread records (render CPU went 12.7 -> 28.4 ms with EXTENT). That fs is
  also the depth-only shader of 422 of the 600 eye pre-pass draws, so skipping it by hash leaves holes (63 % void).
- Render CPU (Mabe): the executor runs 1593 draws a game frame with the table fit, 3702 with EXTENT (about 7.4 us of
  render CPU per extra draw) and 2084 with ring. The classification itself (`ring_route` / `ring_early`, timed by
  `LARECOMP_VR_RING_LOG`) is about 1.2 ms a game frame (eye 0 0.75, eye 1 0.45; about 30 vertex boxes are made again each
  frame), 1.5 - 2 ms in the forest. Ring draws in depth-only passes are skipped at the top of `draw_` (`ring_early`,
  about 1950 draws a frame at Mabe, 2400 in the forest), before the program lookup and the bookkeeping; what they must
  still leave behind (the frame's first scene camera for the stereo patch, widescreen and the weather, the weather's
  and the composite's depth, the backdrop check, eye 1's counters, the snow's pre-pass signatures) is kept there. That
  changed nothing measurable (21.4 ms at Mabe, 27.4 in the forest, as with the route after the bookkeeping): `draw_`'s
  work before the route is cheap. The rest of the gap to the draw count's prediction (about 4 ms at Mabe) is outside
  `draw_`, presumably the command list's state commands around the skipped draws (binds, the camera-patched uniform
  copies), which run in both eyes either way; not measured.
- With snow (`LARECOMP_WEATHER=snow`) the ring rooms get their cover as with EXTENT only (the signatures are kept).
- Neither spot reaches 30 fps with EXTENT. The game holds 30 only with both the GPU and the render thread well under
  33 ms; Mabe with ring alternates (19 fps).

## Probes (runtime/hle/stream.c; research only, off by default)

- `LARECOMP_STREAM_BT=<path substring>[:<count>]`: the host backtrace (guest functions are `f_<addr>`) of the first
  romfs reads of matching files.
- `LARECOMP_STREAM_SCAN=<frame>:<v>[/<v>...],...`: a memory search. Every aligned u16/u32 in resident guest memory that
  holds each track's value at the first frame and that frame's value at the later frames survives. A host thread does
  it, so no game code changes. Used with `LARECOMP_PLACE` teleports between rooms to find the variable that holds Link's
  room / the block centre.
- `LARECOMP_STREAM_PEEK=<hex addr>,<bytes>`: log a guest range's u16 words whenever they change.
- `tools/stream_run.sh <out> <last frame> [VAR=v ...]`: one headless session from a scratch copy of the test save.
  It waits while `~/.cache/headset-in-use` or `~/.cache/af-bench-lock` exists or another lasrecomp runs.
- `tools/stream_sweep.sh <out> [VAR=v ...]`: flat free-camera orbits (yaw 180 / 90 / 0, distance 50) at Mabe, the forest
  and Field_09B, with void %, draws, GPU ms and memory per step.
- `tools/stream_vr.sh <out> [VAR=v ...]`: the same on the VR path (xrmock), with eye dumps.

Safe teleport spots on the Field (no pit or water): (38.25, 128.25) room 11C, (69.75, 126.8) room 11E,
(96.75, 108.75) room 10G.

## Leads for a bigger load radius (not done)

- Room loads on a crossing come from a main-thread task tree under `f_7000854cd0` (per frame) -> `f_7000fb4e70` ->
  `f_70008e6650` -> `f_7000fa7d40` -> `f_7000fa8410` -> `f_7000fb5c10` -> `f_7000faba60` (called once per room of the new
  strip; `LARECOMP_FNPROF_BT`). These are mostly std::function trampolines. The test that picks the 7 x 7 sits above or
  beside them.
- `f_70008f5300..f_70008f5700` turns an AABB into a 15 x 12 cell grid (x / 15, (z - 108) / 12 + 9; 101 columns x
  4 bytes per row) and consults two 4-byte room records at object `+0x2e0` / `+0x2e4` (row, column, floor). That is a cell
  query (collision / actors), and a candidate place for "is this cell live".
- Memory search result: no variable holds Link's column, but **the block's origin column** clamp(col - 3, 0, 9)
  (0 / 1 / 3 / 0 over teleports to columns 2 / 4 / 6 / 2) survived in 5 places: one heap object word and two words that
  also appear 0x400000 higher (double-buffered). Heap addresses change from run to run (a second run with the same
  inputs had zeros there), so the next step has to find the survivor and act on it in the same run: run the search, then
  arm `LARECOMP_WATCH_ADDR`'s page watch on the survivor from the probe thread, and read the writer's code for the
  -3 / clamp to 9 (the 7). Teleports: use `LARECOMP_PLACE_Y=10.6`, and end the title input by frame 1500.
- With the writer found, a radius of 4-5 would mean 9 x 9 = 81 or 11 x 11 = 121 rooms: about 80-120 MB of files plus
  GPU memory. Actors in those rooms would wake too, so the `.leb` half needs care (see Risks). Given the cost of
  drawing the 7 x 7 already, a larger radius only pays off once the outer ring is cheap to draw.

## Risks to game logic

- The culling overrides are draw-only. Game logic is unchanged. Actors already run in all 49 resident rooms whatever
  is drawn.
- A bigger load radius loads `.leb` files too (actors, collision). Enemies and scripts in the extra rooms would wake and
  run their logic far from Link, and the per-level actor and heap limits (`runtime/hle/heaps.c`) may overflow. A safe
  version would load the extra rooms' `.bfres` (models) only, which needs the place where the two halves are requested.
- Loading the whole overworld (256 rooms, 252 MB of files, more once decoded on the GPU) is not realistic at the frame
  costs above. The draw cost already shows that drawing 49 rooms per eye pair is over budget.
