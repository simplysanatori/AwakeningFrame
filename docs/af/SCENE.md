# The world around the table (AwakeningFrame lane "scene")

The VR tab's **Surroundings** row draws the overworld past the table instead of leaving it black. Code:
`runtime/nvn/vr_cull.c` (the culling box, the modes), `runtime/nvn/vulkan/exec.c` (ring routing, the coarse shading of
Low Quality), `runtime/nvn/vulkan/scene.c` (Backdrop), `runtime/nvn/vulkan/foveation.c` (the coarse pipeline chain).
How the overworld is loaded and what limits it: `docs/af/STREAM.md`.

## What is in memory

The game keeps the 7 x 7 rooms around Link's room resident (`STREAM.md`); rooms are 15 x 12 units. Everything in that
block is loaded and only culled on the CPU, once per frame, by a frustum from the game's own camera
(`CULL_ASPECT` / `FOVY_SITES` in `tools/patches.py`, `rt_cull_aspect` / `rt_fovy`). So a reach of up to 3 rooms needs
nothing loaded. Past 3 rooms nothing exists until Link gets closer.

## The modes (`LARECOMP_VR_SCENE`, ini `vr_surroundings`)

| Row choice | Value | What it does |
|---|---|---|
| Off | `off` | the table fit as before (1.2 x 1 screens, the eyes' frusta) |
| Low Quality | `low` | as Full, with the draws past the table shaded in 2 x 2 fragments |
| Backdrop | `backdrop` | the table fit live; the rest from a still picture taken now and then (below) |
| Full | `full` | every resident room within the reach, every frame |

A number (`LARECOMP_VR_SCENE=3`) is Full with that many rooms. `LARECOMP_VR_SCENE_ROOMS=<n>` sets the reach of
Low, Full and Backdrop (2 by default, 3 at most). The row is live (`vr_scene_set_mode`).

Low and Full (and Backdrop's capture frames):
- the culling box is (rooms + 0.5) rooms each way about the table centre (37.5 x 30 units for 2), clipped as before by
  both eyes' frusta and their margin: only what can be seen from the table, a little more as the head turns;
- the widest culling tangent is 20 instead of 4: the box reaches under and behind the game camera, which a frustum from
  that camera can't cover with less (with 4, the rows south of the camera stayed black);
- ring draws (`LARECOMP_VR_RING=4`, `STREAM.md`): what lies past the table fit's footprint goes into the eye colour
  passes only (no shadow casting, no depth pre-pass);
- once the box reaches behind the game camera its frustum lets through every resident room in front of it, so ring
  draws whose box misses the culling box are left out everywhere (class 3, "beyond", in `vr_ring_class`);
- the round fade (Table Edge: Fade) ends at the box's nearer side instead of 25 units out.

Low Quality's coarse shading needs the device's pipeline shading rates (the foveation's setup: off at startup means
none); without them Low Quality is Full. The rate combines with the foveation map (MAX) where the driver offers it.
On the Frame's driver the pipelines get the 2 x 2 rate (`[scene] low quality: draws past the table shade 2x2
fragments`), but neither the pictures nor the GPU time change measurably (multiview, foveated passes): for now Low
Quality costs what Full costs.

Explicit `LARECOMP_VR_CULL_EXTENT` / `_TMAX` / `LARECOMP_VR_RING` win in every mode.

## Backdrop (`scene.c`)

Drawing the outer rooms every frame costs the game thread as much as the GPU: the game handles every object its
frustum lets through, even those the executor then leaves out. Backdrop pays that only on a capture:

1. ask vr_cull for the wide culling (as Full) and wait `LARECOMP_VR_BACKDROP_DELAY` new eye outputs (8) for it to reach
   the eyes;
2. copy eye 0's output and scene depth, with that frame's camera; back to the table fit;
3. every frame after, draw the capture into both eyes as a grid mesh (`LARECOMP_VR_BACKDROP_GRID`, 128 x 128 cells): each
   grid point moved to where its depth puts it in the world, drawn with the eye's current camera, into the pixels the
   live scene left empty (scene depth still cleared). The live game always wins. Cells whose corners differ in distance
   from the capture's eye by more than `LARECOMP_VR_BACKDROP_JUMP` (8%) are left out: no sheets between a tree and the
   ground behind it.

A capture is taken when there is none, when the table's centre moved `LARECOMP_VR_BACKDROP_MOVE` units (6), when eye 0
turned `LARECOMP_VR_BACKDROP_TURN` degrees (25) from the captured view, when the level changes, after
`LARECOMP_VR_BACKDROP_REFRESH` seconds (0: never), and at most every `LARECOMP_VR_BACKDROP_GAP` seconds (1).
Not while the world changes: at the start, after a level change, after a warp (the table's centre jumps more than
3 units between two outputs) and during the rig's blink, it waits `LARECOMP_VR_BACKDROP_SETTLE` seconds (2.5; 0.5
after a blink) for the rooms to stream in and the game to fade in, then takes a capture and one more a settle later.
A level change drops the old level's picture at once. `LARECOMP_VR_BACKDROP_LOG=1` logs each one.

## The edge fill (`scene.c`, every mode but Off, overworld only)

Where the drawn world ends, the void past it takes the colour next to it and fades out, instead of a hard black edge
(`LARECOMP_VR_SCENE_EDGE=0`: off). An eighth-size image holds, per 8 x 8 block of the eye, the average colour of the
drawn pixels (16 taps; the live scene's depth or the backdrop's) times their share; its mips spread that out, and each
empty pixel reads the levels from the coarsest down, each over the coarser ones where it has colour (pull-push): the
nearest drawn world's colour, smoothly spread. It fades with the share of drawn pixels around it, over
`LARECOMP_VR_SCENE_EDGE_REACH` of the eye's height (0.18); past that the void stays as it was. Indoors nothing changes
(the dark around a room is the game's look).

Limits: the picture is still (nothing outside animates; the owner's call); it covers what eye 0 saw at the capture;
characters in it stay where they were (only where the live scene is empty); no visit is needed: it is taken from
whatever is resident.

## Measured

xrmock, 120 Hz, multiview, dynamic resolution off, 1728² eyes, Mabe Village (Field_11C); the forest is alike.
"Overview" is `LARECOMP_VR_SCALE=0.025` (the whole 2-room reach in view, the worst case); "default" the 0.067 scale
with the head 45 degrees down. GPU = the pacer's GPU per game frame; CPU = the game thread; render = translation.

| view | config | GPU ms | game CPU ms | render CPU ms | FPS | void % |
|---|---|---|---|---|---|---|
| overview | Off | 14.0 | 7.3 | 8.3 | 58 | 74 |
| overview | box 2, tan 20, no ring | 35.7 | 23.3 | 22.6 | 27 | 45 |
| overview | + ring | 31.9 | 22.8 | 17.3 | 29 | 36 |
| overview | + beyond (= Full) | 27.7 | 22.9 | 12.9 | 29.5 | 38 |
| overview | Full at 864² eyes | 16.3 | 20.8 | 11.2 | 30 | 38 |
| default | Off | 14.7 | 8.3 | 9.1 | 57 | 21 |
| default | Full | 16.4 | 15.7 | 9.3 | 29.4 | 19 |
| overview | Backdrop, no edge fill | 14.4 | 7.5 | 8.5 | 55.5 | 41 |
| overview | Backdrop | 15.8 | 7.5 | 8.5 | 56.5 | 33 |
| default | Backdrop | 16.2 | 8.5 | 9.3 | 56.7 | 15 |

- The game thread is what Full costs at 60 FPS: +7 ms at the default scale, +15 at the overview. At 90 Hz (30 FPS)
  Full fits.
- The outer draws are pixel-heavy (one profiled overview frame: main colour 16 of 32 ms), so 2 x 2 shading helps the
  GPU; it does nothing for the game thread.
- No memory change: the rooms are resident anyway (Backdrop adds one eye's colour and depth, and a D16 image).
- Backdrop keeps 60 FPS: the picture costs about 0.4 ms, the edge fill about 1.4 ms (both eyes); the capture frames
  (a few after each settle, move or turn) cost what Full costs. The edge fill also covers the cells Backdrop leaves
  out at depth jumps.
