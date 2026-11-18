# Edge treatment

In the headset the loaded world is a tabletop and everything past the zone edge is black: the eye output is cleared to
black and the scene depth is cleared to far. This pass replaces that black with a soft backdrop and fades the world into
it. Code: `runtime/nvn/vulkan/edge.c/.h`. One call in `exec.c vk_stereo_eyes`, inputs from the eye post hook
(`stereo_eye_post`, `docs/af/STEREO.md`), which edge.c defines.

## What it does

**Overworld only, off unless asked.** The default is `LARECOMP_VR_EDGE=black` (off: the present draws the eye as before).
With a mode on, the pass still runs in the overworld only (`game_level_overworld()`, widescreen.c: the level "Field"). An
interior, a dungeon or the title keeps its dark void and the game's own glow, drawn by the present exactly as without
this pass (`LARECOMP_VR_EDGE_INDOORS=1` lifts that, for testing).

Per eye, once per present (all in the frame's command buffer, device local, no read-back):

1. **void share** (1/8 size): for each 8x8 block of the eye, the share of pixels with no geometry (scene depth still
   cleared), all 64 read, turned into a void weight with `smoothstep(0.35, 0.65, share)` (lone pixels and slivers count for
   nothing).
2. **nearness to the void** (two passes, 19 taps): a separable gaussian-weighted MAX of the void weight. The two passes'
   weights multiply into the radial gaussian, so the result is a soft distance to the nearest void: 1 at the void, 0.6
   one sigma away, 0.14 at two sigma, whatever the size of the void (an average would not reach 1 at the edge of a thin
   hole). Sigma is 1/2.5 of the fade radius (`LARECOMP_VR_EDGE_SOFT`, 2.5% of the image height).
3. **backdrop colours** (eye 0, a 2x1 image): the average colour of the geometry in the top 14% and the bottom 14% of
   the eye, followed with a time constant of 0.8 s (last frame's colours are the other of two images). They only
   tint the haze a little (below). Too little geometry up there (under 6 of 96 samples): the colours stay.
4. **main** (into the XR eye image, in place of the present draw): colour and depth are read per pixel at the full eye
   size, the nearness bilinearly from 1/8 size (a smooth field, so no blocks show).
   - void pixels (depth still cleared) get the haze: a light pale-sky tone (`LARECOMP_VR_EDGE_SKY`, default `cce3f5`),
     15% of it the scene's own top-band colour, brighter toward the horizon and a touch greener below it. It is a
     vertical gradient by the pixel's elevation (from the eye matrix, so it stays put when the head tilts);
   - the loaded geometry is tinted only inside the fade band at a void: `smoothstep(0.06, 0.92, nearness)` blends it
     toward the haze, full at the void's edge and gone about two sigma (2.5% of the height) in. Beyond that band every pixel
     is exactly the untreated eye (dither is applied only where something was blended);
   - the void is found per pixel from the depth, so the silhouette against the haze is as sharp as the eye's own
     pixels, and the fade follows the loaded extent: when the game renders further, the void shrinks and so does
     the haze, with no setting to change;
   - triangular dither of one 8-bit step where blended, against banding.

Two more fades exist and are off by default: a distance fog (`LARECOMP_VR_EDGE_FOG`) and a fade to the culling's floor
quadrilateral (`LARECOMP_VR_EDGE_RECT`, `vr_cull_rect` / `vr_cull_rect_shrink` in `vr_cull.h`: a band from the stock
screen's edge to the culling rectangle's, so `LARECOMP_VR_CULL_RECT` bigger than the stock screen is what gives it room).

The eye output is B10G11R11 (no alpha, 5-6 mantissa bits), so the result is not written back into it: a smooth gradient
would band. The pass draws straight into the XR eye image instead (UNORM view, sRGB bytes, as the present did).

**Which depth.** The eye post hook hands over two depths: the latest twinned depth target drawn into before the
composite, which is the game's quarter-size copy made after the scene for its ambient occlusion (480 x 270, always
whole), and the latest twinned depth target of the window image's size drawn into with depth writes on, the scene's
own (`depth_full`: as large as the eye output and filled like it). Writes on, because the frame's last pass tests
against another depth of that size without writing it, and in the dungeons that one is empty when the eyes are
composited (Tail Cave 04B: every pixel read as void). The passes read the scene's own, each image through its own filled part; the copy only
when a frame had none. Read through the copy, as before 2026-11-18, every depth texel covered 3.6 x 6.4 eye pixels: the
fade fell off in steps a few pixels tall on any surface seen at a shallow angle (the sea at the table's edge) and the
outlines followed the copy's texels, with or without foveation. Read through the colour's filled part, as it also was,
every sample of the copy came from 11% nearer the image's corner at 1536 px. `LARECOMP_VR_EDGE_DEPTH=quarter` reads the
copy again (through its own part), for comparison. `[edge] eye N output ... reads the full-size scene depth ...` in the
log says which.

## Fade (`LARECOMP_VR_EDGE=fade`)

A round fade to black around the table's centre, like a spotlight on a tabletop: the world ends in a soft round edge
instead of the loaded zone's straight one. One pass (the main pass alone: no backdrop colours, no void mask), in play
everywhere: the overworld, interiors, dungeons, side views.

- Each pixel's point on the table: the geometry's own (its depth, through the eye's V / P), else where its ray meets
  the floor plane at the anchor's height (the game's glows and other draws that leave the depth cleared). Rays that
  never reach the floor are black. A pixel that is already black stays black and its depth is not read (most of the eye).
- Its horizontal distance from the fade's centre gives the factor: 1 within `R`, 0 past `R + W`, smootherstep between
  (no kink at either end). The colour is multiplied by it (the image is gamma-encoded there, so the falloff looks even).
  Being horizontal, the fade is a cylinder: a tree is as bright at its top as at its foot, whatever its height.
- **Overworld: fitted to what the culling draws.** `vr_cull_drawn` gives the floor the culling takes in, and the circle
  is fitted inside it each game frame: its centre moves (up to 8 units, preferring the table's centre) to where it can
  be largest, black by the edges (`R + W` at most the settings', at least 10 units), `W` 40% of that (3 units at least).
  Wherever the circle reaches past that floor, the factor is also multiplied by a fade to its edges (2 units wide), so
  no cut shows hard. The fit is filtered over 0.4 s; a level change or a table snap starts it afresh.
  - With the table's culling camera (the default, `STREAM.md` "What the table draws") that floor is the table's
    footprint, a rectangle about the table's centre (+-22.3 x +-23.6 units outdoors) that does not change with the head:
    the circle stays on the table's centre, clear to about 13 units and black past 21.8.
  - With the game camera's frustum (`LARECOMP_VR_CULL=camera`) it is a trapezoid widening away from the camera, whose
    near edge is often only 7-9 units from the table's centre and its slanted sides 10-14 units out. With the old fixed
    circle (black past 25 units) those sides cut the table first: a straight-sided wedge of trees with black past it,
    which from the diorama's angle you could see over. The near edge may cut 3 units into the fitted circle. Measured
    then: Mabe clear to about 8, black past 14; Mysterious Woods clear to about 6.5, black past 11.
- In a room or a side view (the rig's area, with its camera zone) `R` is at least the zone's farthest corner from the
  anchor plus 1.5 units: the whole room stays clear and the fade only takes what lies past it (the next rooms, the
  backdrop glow). No fit there, nor with the VR tab's Surroundings on: the culling is opened wide then and the drawn box
  limits the scene, so the fade ends at the box's nearer side.
- **Foveation's blocks are smoothed.** With eye-tracked foveation (VRS) the eye passes shade up to 4 x 4 pixels at once
  away from the gaze; the composite into the eye output is one of them. Those blocks, and the stairs they leave along a
  silhouette against the black, were the "chunky, pixely" fade: the fade is in the periphery by nature, and a dark fade
  against black makes the blocks' steps stand out. The edge pass itself runs at full rate (its own rendering, no
  shading-rate attachment). It gets the rings the foveated passes were drawn with (`fov_shade`, foveation.c: foci,
  ring radii, rates, noted with each new eye output) and works out each 8 px texel's rate exactly as the map did. Where
  a block of b pixels was shaded once, the treated colour (colour and fade factor, each from its own depth) is taken at
  the four nearest block centres and interpolated: block edges and silhouette stairs become smooth gradients, 1 x 1
  areas are read as before. `LARECOMP_VR_EDGE_SMOOTH=0` leaves the blocks.
- Dither: triangular, one 8-bit step, interleaved gradient noise fixed to the pixel (no per-frame seed: it cannot crawl
  or shimmer under head motion), only where the factor is below 1 and the pixel is not black (the void stays exactly
  black).
- The present's screen filter and the rig's blink are applied in the edge pass too (every mode).

| Variable | Meaning |
|---|---|
| `LARECOMP_VR_EDGE_RADIUS=<units>` | clear radius around the anchor, game units (13; a screen is about 15 x 12, a tile 1.5); overworld: the fit's cap with `_WIDTH` |
| `LARECOMP_VR_EDGE_WIDTH=<units>` | width of the fade past it (12) |
| `LARECOMP_VR_EDGE_FIT=0` | overworld: the fixed circle about the anchor (as before), no fit, no fade to the culled floor |
| `LARECOMP_VR_EDGE_SMOOTH=0` | foveation's blocks left as they are |
| `LARECOMP_VR_EDGE_SMOOTH=all` | (a test) with Table Edge off too: the fade's main pass with every pixel kept whole, only to smooth foveation's blocks (one full-size pass per eye, plus the block-centre images) |
| `LARECOMP_VR_EDGE_DEBUG=6` | the factor in red, geometry in green, over the dimmed eye (unsmoothed, no fit's floor fade) |
| `LARECOMP_VR_EDGE_ROUND=1` | (off) each pixel's ray from its output row: the fade round as described above ("The fade's rows" below); the draws past it are then left out |
| `LARECOMP_VR_EDGE_BLACK=<units>` | (overworld fit) black past this at the most, `W` 40% of it: a smaller round table; with `_ROUND=1` the culling follows it |

### The fade's rows (2026-11-18)

Each pixel's ray is taken from the colour image's row (`q`, `qmap(p)`): `1 - 2 q.y`. With the game's lower-left origin
(`renderer_origin_lower_left()`, 1 in the overworld) that row is the output's mirrored, so the fade reads the top of the
eye as its bottom: a far pixel at the top gets the ray of a near one, steep down, and its point on the table (the ray
times its depth) lands near the eye. `scene.c` (`(g.y * 2 - 1) * ce.w`) and `foveation.c` turn the row by the origin;
the fade does not. Seen at the beach (Field_16E), the owner's rig, seated: the factor (`LARECOMP_VR_EDGE_DEBUG=6`) is 1
along the top rows, posts 24 units from the centre (black past 21.8) are lit, and void pixels whose rays are nearly
level read 1. So today's fade is not round about the table's centre: far rows stay lit well past its black radius (the dark
band is mostly the corners: rays far to the side).
The other modes (Black, fog, gradient, passthrough) use the same ray expression in the main pass; untouched.

`LARECOMP_VR_EDGE_ROUND=1` takes the row from the output position (`p`, as the colour is shown), in the Fade mode only
(`fw.w`): the top rows go black past the radius, the corners darken. It changes the picture (at Mabe, seated: the
houses past the weathercock go black), so it is off by default; the owner chooses. Only with it does `edge_fade_disc` answer, and so only then do the
eyes' passes leave out the draws past the circle (PASSES.md "Draws the eyes cannot see") and the table's culling camera
take no more than the square the fade can light (`vr_cull_note_fade`, `LARECOMP_VR_CULL_FADE=0`: not). Without it a
smaller black radius (`LARECOMP_VR_EDGE_BLACK`) would cut the culling to a square the fade does not end inside: a
straight cut at the far side.

## The Mysterious Woods' fog: the dome and the edge curtain

The woods draw a fog dome round the game camera (fragment shader `ffad3b2d795231a7`, 1200 vertices, about 6.6 units
across before its scale, no depth test, blended): from the stock camera, inside it, it is an even blue haze over the
screen. From the diorama, outside it, it is a big translucent disc with a hard elliptical rim over the middle of the
table ("a big circle of fog"). There is no way to make a camera-centred dome right for a viewer outside it.

From the fields beside the woods (Mabe Village's north edge, Field_09D, and the like) the game draws a blue cloudy mist
curtain where the woods begin (`39ae199ddc1701dc`, one 18-index draw after the opaque scene, blended, fading against a
copy of the scene's depth). In the stock view it is a haze along the top of the screen; from the diorama's side angles
it is a tall hard-edged blue slab standing over the trees ("a fog rectangle"). It does not draw inside the woods.

The VR eyes leave both out (`exec.c`, `FS_FOG_DOME` and `FS_WOODS_CURTAIN` in edge.h; any edge mode). The woods keep
their atmosphere from what else they draw: three instanced mist sheets that fade against the scene's depth
(`8605ce71f404bc10`), the light shafts (`d66b33b13b2eac29`, `1372d4fe34dd4bb4`) and the particles.
`LARECOMP_VR_WOODS_FOG=1` keeps the dome and the curtain.

## Modes (`LARECOMP_VR_EDGE`)

| Value | Result |
|---|---|
| `fog` | void -> haze gradient, thin fade where the loaded world meets the void |
| `gradient` | void -> haze gradient only; geometry untouched (the straight edge stays) |
| `black` (`off`, `0`, `none`; the default) | off: the present draws the eye as before |
| `fade` | round fade to black around the table's centre (above); everywhere in play |
| `passthrough` | void and fade as alpha: eye image alpha = 1 - fade, colour premultiplied; the projection layer gets `BLEND_TEXTURE_SOURCE_ALPHA_BIT` and `xrEndFrame` `ALPHA_BLEND` (xr_loop.c) |

The VR tab's Table Edge offers Off (`black`), Fade and Passthrough, and switches between them while the game runs
(`edge_set_mode`): the next eye is drawn in the new mode, and each set of eye images carries its mode, so the projection
layer's alpha flag and the blend mode follow the images. `fog` and `gradient` can only be set from the environment.

Other variables:

| Variable | Meaning |
|---|---|
| `LARECOMP_VR_EDGE_SOFT=<f>` | edge fade radius, share of the image height (0.025; 0 = off) |
| `LARECOMP_VR_EDGE_STRENGTH=<0..1>` | how much of the edge fade (1) |
| `LARECOMP_VR_EDGE_SKY=<rrggbb>`, `_SKYMIX=<0..1>` | the haze tone and its share against the scene's top colour (`cce3f5`, 0.85) |
| `LARECOMP_VR_EDGE_TOP=<rrggbb>`, `_BOTTOM` | fixed backdrop colours (no tint from the scene) |
| `LARECOMP_VR_EDGE_HAZE=<desat>,<bright>` | how the scene's average becomes the tint (0.30, 0.92) |
| `LARECOMP_VR_EDGE_TAU=<s>` | how fast the tint follows the scene (0.8) |
| `LARECOMP_VR_EDGE_FOG=<start>,<end>` | distance fog from the eye, multiples of the eye-to-table distance (off) |
| `LARECOMP_VR_EDGE_RECT=<f>` | fade to the culling's floor quadrilateral, share of its band (off; 1 = the whole band) |
| `LARECOMP_VR_EDGE_INDOORS=1` | run in interiors and dungeons too (default: the overworld only) |
| `LARECOMP_VR_EDGE_DEBUG=<n>` | 1 depth, 2 nearness to the void, 3 rect / fog / edge, 4 elevation, 5 backdrop, 8 before / after split (left half untreated), 9 2x2 mosaic, 10 untreated eye inside a box (x .55-.95, y .5-.9), treated outside, one frame, 11 the pass's change x8 (white where it changed a pixel) |
| `LARECOMP_VR_EDGE_DEPTH=quarter` | read the game's quarter-size depth copy instead of the scene's own (comparison; above) |
| `LARECOMP_VR_EDGE_BENCH=<mask>` | timing only: 1 no tint pass, 2 no mask passes, 4 no main pass |

## Cost

`[edge] GPU per eye` in the log every 2 s: the whole set of passes for one eye (timestamps around them).

Measured on the Frame (Adreno 750, Turnip, 1728 x 1728 per eye, xrmock, Mabe and the forest): 0.68 ms for eye 0 and 0.58 ms
for eye 1 (eye 0 also runs the tint pass), steady over 60 frames. That is over the 0.5 ms target, with the executor itself
taking 30+ ms of GPU in the same frames (so the timestamps are not free of contention). Five small render passes (tint, void
share, two nearness passes, main) with a full barrier after each: the void share pass reads all 64 depth pixels of each
8x8 block, the main pass reads colour and depth once at full size, the rest work at 1/8 size or 2 x 1. Levers if it has
to come down: skip the tint pass on most frames (it is smoothed over 0.8 s anyway), cut the void share to 4 x 4 taps per
block at 1/4 size, `LARECOMP_VR_EDGE_BENCH` to see which pass costs what.

## Passthrough probe (`tools/xrblendprobe`)

No session, no frame loop: `xrEnumerateEnvironmentBlendModes` for each view configuration and the instance extensions,
on the real SteamVR runtime (OpenXR 2.19.1 on the Frame). Result: 

```
runtime: SteamVR/OpenXR 2.19.1
view configuration PRIMARY_STEREO (2): xrEnumerateEnvironmentBlendModes -> 0, 2 mode(s): OPAQUE(1) ALPHA_BLEND(3)
XR_FB_passthrough: not offered
XR_HTC_passthrough: not offered
XR_EXT_composition_layer_inverted_alpha: not offered
XR_FB_composition_layer_alpha_blend: not offered
XR_KHR_composition_layer_color_scale_bias: not offered
XR_META_* offered: XR_META_performance_metrics, _recommended_layer_resolution, _foveation_eye_tracked, _vulkan_swapchain_create_info
```

52 instance extensions in all (list in `tools/xrblendprobe` output). No passthrough extension, but OPAQUE and ALPHA_BLEND.

`LARECOMP_VR_EDGE=passthrough` is untested on the headset: the runtime offers ALPHA_BLEND, but whether SteamVR shows the
room behind alpha-0 pixels of a projection layer (and not just black) can only be seen in the headset.

## Open: headset flicker

The owner saw Table Edge = Fade flicker in the headset (session 20261106-193606), with Surroundings Off and also with
Backdrop and Full; Black was fine. Not reproduced at the time of these notes (no xrmock run was made). What reading the
code gives (unmeasured):

- **Seen with Surroundings on too, so the fit is not the only cause.** With Surroundings on the fit is off (`reach > 0`)
  and the cut band is unused. What every fade frame has in common, and Black does not, is the edge pass itself: the
  foveated block smoothing and the dither. The dither is fixed to the pixel. The smoothing follows the live gaze: each
  game frame the rings (`fov_shade`) move with the eye tracker, the texels whose rate changes switch between the
  full-rate read and the block-centre interpolation, and at a silhouette against black that interpolation spreads a
  4-8 px ramp of black into the geometry. As the gaze jitters, these ramps jump about along the fade band. In xrmock the
  gaze was fixed (the gaze action space was a hand pose), so this never showed there. First suspect.
- **Surroundings Off: the fit has two frame-to-frame jumps.** (1) When `vr_cull_fit` sees a corner of the table prism
  behind the game camera it clears `g_drawn.have`; for that frame `edge_eye` skips `fade_fit` and draws the old fixed
  circle (clear to 13, black past 25) in place of the fitted one (owner log: about 9 / 15), so the whole fade radius
  jumps for one frame and back. Fast head turns make this more likely. (2) The cut band (`FADE_CUT_BAND`) is drawn
  against the raw trapezoid of each frame, not a filtered one, and `cut` switches on and off by a threshold, so that band
  pops in and out. Only the centre and radius are filtered (0.4 s).
- Dynres (1536-1728) only changes the filled part. The rings, the block-centre images and the dither use the same
  coordinates in the map and in the edge pass, so no mismatch was found there. The depth did not: it was read through
  the colour's filled part ("Which depth" above, fixed 2026-11-18), so the fade's position moved by up to 11% of the
  image whenever the eye size stepped.

Next steps: xrmock with `MOCK_XR_GAZE` (a moving, jittering gaze), the pose file
`build/flick/sway25.pose` (yaw +-25 degrees at 1.5 Hz) and `LARECOMP_VR_EDGE_LOG=1` (one line per eye-0 fade pass:
radius, width, fit, cut, trapezoid, foci, rings). Dump consecutive frames without stalling (`MOCK_XR_SHIFT_DUMP`). Then
bisect `LARECOMP_VR_EDGE_SMOOTH=0`, `LARECOMP_VR_EDGE_FIT=0`, foveation off and dynres off. Likely fixes: smoothing
against rings that hold still (quantised or hysteretic foci), keeping the last fit while `vr_cull_drawn` has nothing,
and filtering the cut-band trapezoid or dropping the band.

## Known limits and open items

- The edge fade is image-space: a room that floats in black (interiors) also thins at its silhouette.
- Second-camera passes (reflection) and the HUD are untouched; the HUD quad is a separate layer.
