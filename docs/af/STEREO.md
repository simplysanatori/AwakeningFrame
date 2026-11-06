# Stereo executor (AwakeningFrame lane 4)

VR mode (`LARECOMP_XR=1`) draws the real game into both eye swapchains with a camera per eye. The HUD goes on the quad
layer. Design: `~/.claude/plans/awakeningframe-vr-design.md` sections 2-6. Code: `runtime/nvn/vulkan/stereo.c/.h`
(switches, v1 diorama cameras), `exec.c` (the executor), `textures.c` (eye twins), `runtime/nvn/cam_override.c`
(per-eye substitution), and small hooks in `device.c` and `runtime/xr`.

## How it works

- **Each list runs once per eye** (`exec.c stereo_execute`). Before eye 1, the bound state is restored. The game's
  memory writes from eye 0 (uniform updates, buffer-to-buffer copies) are undone, and eye 1 then redoes them.
- **Eye 1 runs a draw, clear or copy only when a target is twinned.** Twinned targets are the 16:9 render targets
  (the scalable ones) and the guest-sized AO targets. Shadow maps, cube maps and small offscreen targets are drawn
  once, in eye 0, and shared. Compute, counters and read-backs run in eye 0 only.
- **Twins** (`vk_stereo_swap`): while eye 1 runs, each twinned texture's `backend` is its eye-1 image. Each swap bumps
  `vk_view_gen`, because the texture memo and cached descriptor sets would otherwise hand eye 1 the views of eye 0.
  Copy aliases are off.
- **Camera:** this is the free-camera probe's substitution, done once per eye. Kind `CAM_STEREO` rewrites the 320-byte
  camera: floats 0-47, the rotation block 48-63, back (64-67) and eye position (68-71). It finds them by matching the
  game's own axes at the latch. Every other block that holds the camera's matrices or eye position is rewritten too
  (`cam_override_patch`). Culling is widened (3.2:1.5) unless `LARECOMP_FREECAM_CULL` is set.
- **Composite:** the scene's composite into the window image goes into a per-eye "eye output" instead. That output is
  a clone of the window texture, twinned. The window image is cleared transparent and keeps the HUD. Blended draws into
  it get alpha factors ONE / ONE_MINUS_SRC_ALPHA. Unblended full-screen fills after the composite leave alpha alone.
  The present to the HUD image keeps alpha (no SMAA/FSR on that path), and the quad layer blends with source alpha.
- **Render scale:** scalable targets are allocated at the eye size. The scale is anisotropic (0.9 x 1.6 for 1728²).
  The game draws its UI in the centred 16:9 box, and that box is cropped to fill the HUD image.
- **Pose:** one `xrLocateViews` per game frame, at the frame's first camera draw (`xr_latch_views_early`). The set
  carries those poses into the projection layer.
- **Eye images:** at the present, both eye outputs are drawn into the XR eye images (`vk_stereo_eyes`).
- **Timing:** each eye run of a list is wrapped in timestamps. Results are summed per game frame. `[stereo] GPU per
  game frame` is logged every 2 s, and `xr_timing.log` gets `gpu0` / `gpu1`. Eye 0 includes the shared passes.

## v1 diorama (fixed table; lane 6 replaces it)

`app = A + s * Ry(yaw) * (world - pivot)`:

- **pivot:** the screen centre of the game's camera, at Link's height. It is latched per game frame, so the table
  follows the game camera, but only on new game frames.
- **A:** the point the pivot sits on. It is `LARECOMP_VR_DIST` (1.0 m) from the head's start, at
  `LARECOMP_VR_PITCH` degrees down (by default the game camera's own pitch, clamped to 25-65, about 45 at Mabe), or
  `LARECOMP_VR_TABLE=x,y,z`.
- **s:** `LARECOMP_VR_SCALE` (0.067 m per unit). `LARECOMP_VR_YAW` sets the yaw.
- **Depth:** the eye projections keep the game's depth terms. `LARECOMP_VR_NEAR=<units>` overrides them.

## Switches and tests

| Variable | Meaning |
|---|---|
| `LARECOMP_STEREO=0` | VR mode without the executor (the flat image on the HUD quad, stub eyes) |
| `LARECOMP_STEREO_SIZE=WxH` | eye render size (default: the eye swapchain's) |
| `LARECOMP_STEREO_TEST=identity` | the game's own camera bytes in both eyes (eye 0 == eye 1 == flat, stretched) |
| `LARECOMP_STEREO_TEST=gamecam / gameP / gamerot / gamepos` | bisect aids: the game's V+P / P / rotation / position in the eye cameras |
| `LARECOMP_STEREO_NOAXIS=1` | floats 48-79 left as the game has them |
| `LARECOMP_STEREO_DUMP_AT=<frame>,...`, `LARECOMP_STEREO_DUMP_DIR` | eye outputs and the window image as PNG |

Mock run: `XR_RUNTIME_JSON=build/xrmock/runtime.json LARECOMP_XR=1 MOCK_XR_EYE_SIZE=1728x1728
MOCK_XR_POSE=static:0,1.6,0,0,-45 SDL_VIDEODRIVER=offscreen ENABLE_GAMESCOPE_WSI=0 ...`. The head looks 45 degrees down
at the table.

## Known limits

- Cost: double execution is draw-bound. At Mabe with widened culling, both eyes take about 29-31 ms of GPU at both
  1728² and 1400². The pixel count barely matters.
- Fades go to the HUD quad only. Frames with no scene (menus) keep the last eye images.
- A dispatch or a copy that writes a shared target from a twin gives eye 1 eye 0's result. The copy case is logged.
- Second perspective cameras (reflection, shadow, light) are not substituted.
- The loaded world ends at the zone edge (black void). There is no edge fade yet (lane 6).
