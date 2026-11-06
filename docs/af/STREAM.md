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
