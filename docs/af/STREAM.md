# Overworld streaming and the diorama's black voids (AwakeningFrame lane "stream")

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

1. **The game's CPU frustum culling** (the visibility pass culls with the game camera's aspect `+0x570` and fovy `+0x564`,
   patched to `rt_cull_aspect` / `rt_fovy`). In VR, `runtime/nvn/vr_cull.c` widens that frustum to fit a table rectangle
   (1.2 x 1 stock screens). Geometry that is loaded but outside this rectangle is not drawn. This made most of
   phase 2's "zone edge" cuts: the forest's rooms 05C/04C are resident but drawn black.
2. **The streamer's 7 x 7 block**. Past +-3 rooms nothing is loaded. You only see this once the culling is widened.
3. The camera far plane (100 outdoors, 45 indoors; floats 72-75 of the camera buffer). This matters only for long
   pull-backs.

## Levers

| Variable | Effect | Default |
|---|---|---|
| `LARECOMP_VR_CULL_EXTENT=<x>,<z>` | the VR culling rectangle becomes the world-axis box of these half-extents about the table centre; `52,42` covers the resident 7 x 7 | off (the 1.2 x 1 table fit) |
| `LARECOMP_VR_CULL_TMAX=<tan>` | the widest half-angle tangent the fit may ask for | 4 (76 degrees) |
| `LARECOMP_VR_RING=<margin>` | ring draws (below): draws whose world box misses the table fit's footprint grown by this go into the eye colour passes only | off |
| `LARECOMP_VR_RING_CAST=<margin>` | ring draws within this of the footprint still cast shadows | 0 (none) |
| `LARECOMP_VR_RING_LOWRES=0` | ring draws also stay out of the eyes' smaller colour passes (the 704 x 396 one) | 1 (drawn there) |
| `LARECOMP_FREECAM_CULL=<aspect>:<fovy scale>` | flat free-camera probe: a fixed widened culling (docs/af/FREECAM.md) | stock |

Both VR overrides change only what is drawn. Nothing extra is loaded and no game logic sees them.

### Measured (xrmock, 90 Hz, 1680 x 1760 eyes, head static 45 degrees down; `tools/stream_vr.sh`)

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

## Ring draws: the world past the table at a lower cost (lane ring)

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
- The 704 x 396 row of the stream lane's breakdown is not the casters: with fs d3e83e0301de7e42 skipped (the casters
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
