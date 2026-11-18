# The world around the table

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
- the fitted widening is held: it grows at once and shrinks only after `LARECOMP_VR_CULL_HOLD` frames (60) without
  growing, 3% a frame. Without it a box corner dipping behind the game camera as the head moves took the tangent from
  about 2 to 20 for single frames, and objects at the edge of the game's frustum (the cuccos below the table) came and
  went: flicker;
- only room geometry is left out past the box (class 3); characters, props and sprites still draw. Draw boxes are
  memoised by buffer address and checked against three of their vertices each time: buffers rewritten in place
  (sprites, effects) used to get another draw's box, about two draws a frame, and so vanished for a frame.
- a draw's world box comes from vertex block 2 (its model matrix) and the bone palette alone, which every pass binds
  for the draw. It used to pick between the two by the size of block 4, which only the colour passes bind per draw: in
  the depth-only passes a character's draw after a room's was taken for room geometry at the world's origin, past
  every box. The frame's class memo mostly hid that; when another draw had taken the memo's slot, the draw was left
  out of the pre-pass, failed its EQUAL test in the colour pass and was gone for that frame. The slots follow the
  blocks' addresses, which alternate between two sets, so Link's head at Marin and Tarin's door was missing on every
  other frame in Low Quality and Full, and props past the box were left out or drawn by the same chance. Which draws
  are room geometry is now asked only where a colour pass leaves them out (`LARECOMP_VR_RING_DUMP=<frame>,<frames>`
  shows each draw's class per pass).

Low Quality's coarse shading needs the device's pipeline shading rates (the foveation's setup: off at startup means
none); without them Low Quality is Full. The rate combines with the foveation map (MAX) where the driver offers it.
On the Frame's driver the pipelines get the 2 x 2 rate (`[scene] low quality: draws past the table shade 2x2
fragments`), but neither the pictures nor the GPU time change measurably (multiview, foveated passes): for now Low
Quality costs what Full costs.

Explicit `LARECOMP_VR_CULL_EXTENT` / `_TMAX` / `LARECOMP_VR_RING` win in every mode.

## Backdrop (`scene.c`)

Drawing the outer rooms every frame costs the game thread as much as the GPU: the game handles every object its
frustum lets through, even those the executor then leaves out. Backdrop pays that only on a capture:

1. the next frame, still culled to the table, notes what it draws past the table's footprint (by vertex count and
   place), the eyes' passes and the shared ones (the shadow maps) apart, and a map of everything it draws: a floor
   plan about the table, 2 units a texel, holding the height of the highest drawn point (from both eyes' scene depth).
   The table fit's shadow pass already holds outer houses and people the eyes do not have (the light's frustum reaches
   past the table): noted together, they counted as the eyes' own and came into the eyes on every wide frame;
2. ask vr_cull for the wide culling (as Full) and wait until the number of draws it adds stops growing: about 3 new eye
   outputs (`LARECOMP_VR_BACKDROP_DELAY`, 8, at most). An added draw is one past the footprint that step 1 did not
   note. The executor leaves the added draws out of every pass on these frames (`exec.c` `ring_phase`), so the eyes
   show what they showed before;
3. one frame, the capture frame, draws them in the eye colour passes. Eye 0's output and scene depth are copied, with
   that frame's camera. Then both eyes' outputs are cut back to what step 1's frame had (the mask: a pixel stays when
   it lies inside the footprint or no higher than the map has it there, and is made black otherwise), and the fill
   below covers the rest from the capture just taken. So the capture frame too shows the table live and the rest from
   the picture;
4. back to the table fit. The game lets go of the added draws over about 10 more outputs, and they stay out of the eyes
   until their number stops falling (`LARECOMP_VR_BACKDROP_TAIL`, 60, at most);
5. every frame after, draw the capture into both eyes as a grid mesh (`LARECOMP_VR_BACKDROP_GRID`, 128 x 128 cells): each
   grid point moved to where its depth puts it in the world, drawn with the eye's current camera, into the pixels the
   live scene left empty (scene depth still cleared). The live game always wins. Cells whose corners differ in distance
   from the capture's eye by more than `LARECOMP_VR_BACKDROP_JUMP` (8%) are left out: no sheets between a tree and the
   ground behind it.

Two things the fill has to get right about its images:

- **Each image has its own filled part.** With dynamic resolution the eye output fills only a part of its image (1536
  of 1728: 0.89). The scene depth the fill gets is the game's quarter-size copy (480 x 270), which is always whole. The
  capture keeps both parts, and every sample goes through the part of the image it reads. Read with the colour's part,
  as it was, every depth sample was off by up to 11%: grid points got the depth of another place and the 8% test threw
  most cells out (fragments at 1728 after a capture at 1536, nothing at 1536), and the edge fill painted over drawn
  pixels (Full and Low lost a quarter of their edge at 1536).
- **The rows run as the game drew them.** The game's window origin is the lower left, so row 0 of the eye output is the
  bottom of the view. The fill took it for the top. From the pose a capture was taken at, the two mistakes cancel; from
  any other pose the picture came out mirrored about the view's middle row, slid against the table with every head
  movement and showed the table's own picture a second time past its edge. This was the moving-head flicker.

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

Limits: the picture is still (nothing outside animates, by design); it covers what eye 0 saw at the capture;
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

## What the headset flicker was

The owner's headset session (2026-11-06 19:36, Fade, dynamic resolution on with the 14.5 ms budget, eyes 1536 of 1728
from the first seconds) showed what xrmock had not: Backdrop's fill came out as scattered fragments only, Backdrop,
Low Quality and Full flickered, and the reach changed with head pose (buildings missing). `tools/flicker_run.sh`
([TOOLING](TOOLING.md) "Flicker") runs those conditions under xrmock: the eye size stepping between 1536 and 1728, a
moving head, the Frame-like profile. Four faults came out of it, each described where it was fixed above:

| What was seen | Fault | Where |
|---|---|---|
| the fill in fragments, or none, when the eye was below full size; Full and Low losing a quarter of their edge | the scene depth read with the colour's filled part | "Each image has its own filled part" |
| the fill sliding and doubling the table with every head movement | the eye output's rows taken top down | "The rows run as the game drew them" |
| Link's head on every other frame in Low Quality and Full; props past the box coming and going | a draw's box picked by a block the depth-only passes do not bind | the last point of the Low and Full list |
| outer objects popping in and out at each capture | the capture's wide frames were shown; then, with them kept out, the draws the table fit's shadow pass had were taken for the eyes' own | Backdrop, steps 1 to 4 |

Checked and not at fault: the capture's matrices. They are the ones the output was composited with (the same latch,
`stereo_note_composite`).

## Open

- **The capture frame still differs a little from its neighbours.** Measured (still head, a capture every 2 s, eye
  pinned at 1536): the capture frame's top band differs from the image before it by about 5 (mean luma), and the
  image after it from the capture frame by about 4.7, against at most 1.4 between ordinary images (11 with the mask
  cutting at the footprint grown by the ring margin, without the map). Why is not established. A likely reading,
  not measured: where an added draw stood in front of table-fit ground, the mask cannot give the ground back, so for
  that one image the fill shows the added draw there and the far band's outline shifts. A ring dump of the capture
  frame's eye passes against the frame before would settle it. Masking
  every frame of the wide culling instead was tried: over its frames the head moves on from the map's view, and the
  patches without a picture show.
- **A capture still changes the picture.** What the old capture did not hold appears when the new one lands: after a
  turn the eye shows the void (with the edge fill) past the old picture's view until the next capture (25 degrees,
  at most one a second). A capture wider than the eye's view would need a pass of its own.
- **The live picture's outline is coarse.** The scene depth the fill gets is the game's quarter-size copy: at 1728 px
  one of its texels is 3.6 x 6.4 pixels, so the line between the live table and the fill steps in blocks of that
  size. The full-size scene depth is still there at the hook and would make it exact.
- **Table Edge reads the same depth with the colour's part** (`edge.c`, `depth_crop`): the fault the fill had, in
  the Fade and Passthrough passes when the eye is below full size. Not changed here.
- **The cost of a capture** is under "Measured".
- **Real fov.** The headset log does not print the eyes' fov. A one-time `[xr] eye fov` line now prints them, and
  xrmock gets `MOCK_XR_FOV_RIGHT` (`mirror`: an asymmetric fov mirrored for the right eye) and `MOCK_XR_CANT`
  (canted views), to test with Frame-like values (assumed: -52 / 44 / 50 / -54 degrees for the left eye) until
  the real ones are logged.
- **Low / Full pose-dependent.** The fit clips the box by both eyes' frusta, so the culling tangent, and with it
  what the game emits, follows the head; class 3 drops room geometry whose box misses the reach box, which follows
  the table, so a building at the box edge comes and goes. Fix direction: fit the box without the eye clipping in
  Low / Full (the tangent then stays at 20), don't drop level draws inside the resident 7 x 7, and give the box
  hysteresis.
- **Off / Backdrop reach.** One game-camera frustum fitted to the table and widened by the eyes' views, so the reach
  depends on pose. `LARECOMP_VR_CULL_HOLD_ALL=1` holds the widening in every mode; its 60 FPS cost is not measured.
- **Draws the executor cannot place** (instanced ones, draws whose matrix is scaled unevenly: some buildings) are
  core in every mode: they cast shadows and take the depth pre-pass however far out they are. Around a Backdrop
  capture they, and blended draws, reach the eyes on the wide frames too: in one ring dump 18 of the 111 draws the
  wide culling brought into the eyes were of these (11 blended, 4 instanced, 3 without a box). The other 93 had a box;
  of the 85 of them in one of those frames, 69 were the shadow-pass mix-up of step 1, and 16 were not traced. What
  these leave shows as one image that changes about as much as ordinary images change from one to the next (top
  band 1.2 in mean luma, against at most 1.4).
