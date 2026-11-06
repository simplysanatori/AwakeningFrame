# Edge treatment (AwakeningFrame "edge" lane)

In the headset the loaded world is a tabletop and everything past the zone edge is black: the eye output is cleared to
black and the scene depth is cleared to far. This pass replaces that black with a soft backdrop and fades the world into
it. Code: `runtime/nvn/vulkan/edge.c/.h`. One call in `exec.c vk_stereo_eyes`, inputs from the eye post hook
(`stereo_eye_post`, `docs/af/STEREO.md`), which edge.c defines.

## What it does

Per eye, once per present (all in the frame's command buffer, device local, no read-back):

1. **coverage** (1/8 size): the share of each 8x8 block that has geometry (scene depth below far).
2. **blur** (two passes, 13 taps): the coverage, widely. A pixel at the world's edge sits at 0.5, deep inside at 1.
3. **backdrop colours** (eye 0, a 2x1 image): the average colour of the geometry in the top 14% and the bottom 14% of
   the eye, desaturated 30% and darkened 8%, followed with a time constant of 0.8 s (last frame's colours are the other
   of two images). Too little geometry up there (under 6 of 96 samples): the colours stay.
4. **main** (into the XR eye image, in place of the present draw): colour, depth and coverage are read once each.
   - the pixel's distance from the eye (game units) comes from the depth and the eye's projection (the game's depth terms
     `P[2][2]`, `P[2][3]`; depth stored as `(z + 1) / 2` when `nvn_depth_mode() == 0`, as the snow pass does);
   - the backdrop is a vertical gradient by the pixel's elevation (the angle above the world's horizontal, from the eye
     matrix, so it stays put when the head tilts): bottom colour below, top colour above, smoothstep over -0.45..0.50;
   - void pixels (depth still cleared) get the backdrop;
   - geometry fades to the backdrop by `max(fog, edge)`: `fog` = smoothstep of the distance from 1.9 to 3.6 times the
     diorama's pivot distance, `edge` = 1 - smoothstep(0.5, 0.97, blurred coverage), so the world thins out over about
     7% of the image height before it meets the void instead of ending on a straight line, and the colour there is the
     backdrop's, so there is no seam;
   - triangular dither of one 8-bit step, against banding.

The eye output is B10G11R11 (no alpha, 5-6 mantissa bits), so the result is not written back into it: a smooth gradient
would band. The pass draws straight into the XR eye image instead (UNORM view, sRGB bytes, as the present did).

## Modes (`LARECOMP_VR_EDGE`)

| Value | Result |
|---|---|
| `fog` (default) | void -> backdrop gradient, geometry fogged by distance and thinned at the world edge |
| `gradient` | void -> backdrop gradient only; geometry untouched (the straight edge stays) |
| `black` (`off`, `0`, `none`) | off: the present draws the eye as before |
| `passthrough` | void and fade as alpha: eye image alpha = 1 - fade, colour premultiplied; the projection layer gets `BLEND_TEXTURE_SOURCE_ALPHA_BIT` and `xrEndFrame` `ALPHA_BLEND` (xr_loop.c) |

Other variables:

| Variable | Meaning |
|---|---|
| `LARECOMP_VR_EDGE_FOG=<start>,<end>` | fog distances, multiples of the pivot distance (default `1.9,3.6`). The pivot distance is `LARECOMP_VR_DIST / LARECOMP_VR_SCALE` (14.9 units) |
| `LARECOMP_VR_EDGE_SOFT=<f>` | edge fade radius, share of the image height (0.07; 0 = off) |
| `LARECOMP_VR_EDGE_STRENGTH=<0..1>` | how much of the edge fade (1) |
| `LARECOMP_VR_EDGE_TOP=<rrggbb>`, `_BOTTOM` | fixed backdrop colours (no tint from the scene) |
| `LARECOMP_VR_EDGE_HAZE=<desat>,<bright>` | how the scene's average becomes haze (0.30, 0.92) |
| `LARECOMP_VR_EDGE_TAU=<s>` | how fast the colours follow the scene (0.8) |
| `LARECOMP_VR_EDGE_DEBUG=<n>` | 1 depth, 2 coverage, 3 distance / fog / edge, 4 elevation, 5 backdrop, 8 before / after split, 9 2x2 mosaic (colour, depth, coverage and fog, result) |

## Cost

`[edge] GPU per eye` in the log every 2 s: the whole set of passes for one eye (timestamps around them).

RESULTS_COST

## Passthrough probe (`tools/xrblendprobe`)

No session, no frame loop: `xrEnumerateEnvironmentBlendModes` for each view configuration and the instance extensions,
on the real SteamVR runtime (OpenXR 2.19.1 on the Frame). Result: PROBE_RESULT

`LARECOMP_VR_EDGE=passthrough` is untested on the headset: the runtime offers ALPHA_BLEND, but whether SteamVR shows the
room behind alpha-0 pixels of a projection layer (and not just black) can only be seen in the headset.

## Known limits and open items

- The pivot distance (fog scale) is the diorama's default (`LARECOMP_VR_DIST / LARECOMP_VR_SCALE`); the rig lane's size
  changes at runtime are not followed yet.
- The edge fade is image-space: a room that floats in black (interiors) also thins at its silhouette.
- Second-camera passes (reflection) and the HUD are untouched; the HUD quad is a separate layer.
