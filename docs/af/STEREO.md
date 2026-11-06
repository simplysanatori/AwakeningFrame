# Stereo executor (AwakeningFrame lane 4)

VR mode (`LARECOMP_XR=1`) draws the real game into both eye swapchains with a camera per eye. The HUD goes on the quad
layer. Design: `~/.claude/plans/awakeningframe-vr-design.md` sections 2-6. Code: `runtime/nvn/vulkan/stereo.c/.h`
(switches, v1 diorama cameras), `exec.c` (the executor), `textures.c` (eye twins), `runtime/nvn/cam_override.c`
(per-eye substitution), and small hooks in `device.c` and `runtime/xr`.

## How it works

- **Each list runs once per eye** (`exec.c stereo_execute`). Before eye 1, the bound state is restored. The game's
  memory writes from eye 0 (uniform updates, buffer-to-buffer copies) are undone, and eye 1 then redoes them.
- **The frame's camera is latched before eye 0 runs a list** (`stereo_prelatch`): the list's records are walked for the
  vertex slot-5 binding up to its first draw with a 320-byte camera there. The latch used to happen inside eye 0, at that
  draw: eye 0's draws before it (the level's geometry, which reads the view-projection from the pass's own 1 KB scene
  blocks) were patched against the previous frame's latch, which their blocks no longer matched once the camera moved.
  Eye 0 then drew them with the game's own camera while eye 1 (after the latch) had its eye camera: one eye flat and
  squashed whenever Link or the camera moved. `LARECOMP_STEREO_PRELATCH=0` turns it off (the old behaviour).
- **Camera check** (`st_cc_*`, exec.c): per draw into a twinned target, the uniform slots that got the eye's camera.
  Eye 1 compares each of its draws with eye 0's run of the same draw record (both eyes run the same list memory): a
  slot only one eye substituted, or a main camera that changed since the latch, means that eye drew with the game's own
  camera. Every 10 s: `[stereo] camera check (10 s): F frames, D draws compared ...; game's own camera: eye 0 N draws,
  eye 1 M; K draws before the frame's camera was latched` and, when there were any, `[stereo] eye N drew M draws with the
  game's own camera in K of F frames (10 s)` (the tour's BROKEN). `LARECOMP_STEREO_CAMCHECK=0` off, `=2` also the
  first mismatches in detail and where each frame's latch happened.
- **Fences after both eyes:** in the executor without the render thread (`LARECOMP_RENDER_THREAD=0`) a list's fence
  syncs are signalled once both eyes ran. With the render thread (the default) the game's SyncWait already waits for
  the whole list's translation, both eyes (`rq_wait_item`; `nvn_sync_set_seq` is a no-op there), so the deferred GPU
  wait of the overlap cannot let the game overwrite memory translation still reads.
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
  a clone of the window texture, twinned.
- **HUD:** the window image is B10G11R11 (NVN 0x3F), which has no alpha. After eye 0's composite, the game's draws
  into the window go to a clone in RGBA16F (`g_hud`, never twinned) that is cleared transparent. Blended draws into
  it get alpha factors ONE / ONE_MINUS_SRC_ALPHA. Unblended full-screen fills after the composite leave alpha alone.
  The present draws the clone into the HUD image and keeps alpha (no SMAA/FSR on that path). The quad layer blends
  with source alpha. Frames with no composite (menus) present the window image as it is, opaque.
- **Copies from a twin:** when eye 0 copies a twinned texture into a shared one, the destination is twinned from then
  on (`vk_stereo_twin_add`). Its twin starts as a copy of eye 0's image, and eye 1 runs the copy into it.
- **Render scale:** scalable targets are allocated at the eye size. The scale is anisotropic (0.9 x 1.6 for 1728²).
  The game draws its UI in the centred 16:9 box, and that box is cropped to fill the HUD image.
- **Pose:** one `xrLocateViews` per game frame, at the frame's first camera draw (`xr_latch_views_id`). Each eye
  output keeps the latch it was rendered with (`stereo_note_composite`). When the outputs are drawn into a set's eye
  images, the set's projection layer gets those latches' poses (`stereo_set_views` -> `xr_set_views`), so a set that
  shows an older output (a present without a new composite) carries the older poses. The poses are located for the
  time the frame is expected on the display: now plus the measured time from a latch to its set's first refresh
  (`LARECOMP_XR_PREDICT=vsync`: the latest vsync's display time, as before).
- **Pose check:** the `[xr]` 5 s line counts the refreshes whose layer pose is not the one the eye images were rendered
  with ("pose mismatches ... of ... refreshes with the executor's eyes"), sets whose eyes come from two latches, the
  latch-to-display time and how far the poses' time was from the display time.
- **Eye images:** at the present, both eye outputs are drawn into the XR eye images (`vk_stereo_eyes`).
- **Eye post hook (for the fog / backdrop pass):** `stereo_eye_post(const StereoEyePost *)` (stereo.h) is called once
  per new eye output, in `vk_stereo_eyes`, before that output is drawn into the XR eye image. It runs on the render
  thread, outside any rendering, with the command buffer, the eye output (GENERAL; change it in place), that eye's
  scene depth (the latest twinned depth target drawn before its composite; read only), the filled part of the images
  (`ax`, `ay`), the eye's V and P in game units (`stereo_eye_matrices`) and the scene pass's depth range. exec.c has an
  empty weak definition; a strong one elsewhere replaces it.
- **Timing:** each eye run of a list is wrapped in timestamps. Results are summed per game frame. `[stereo] GPU per
  game frame` is logged every 2 s, and `xr_timing.log` gets `gpu0` / `gpu1`. Eye 0 includes the shared passes.

## Foveation (eye-tracked; `runtime/nvn/vulkan/foveation.c`)

`LARECOMP_VR_FOVEATION=off|low|medium|high` (default medium in VR; flat mode is unchanged). Foveation is eye-tracked
only. Each eye pass whose attachments are all twinned and scalable, and at least `LARECOMP_VR_FOVEATION_MIN` (0.75) of
the eye size, gets a per-eye shading-rate attachment (VK_KHR_fragment_shading_rate, 8 px texels). Away from the focus,
pixels are shaded at a coarser rate (2x1 up to 4x4). Rasterisation, depth and geometry edges stay at full resolution,
and it works in both of Turnip's modes (sysmem and GMEM).

- **Focus:** the eye gaze (XR_EXT_eye_gaze_interaction: a pose action in its own set, `xr_input.c`), located in the
  latch's app space at the latch's display time (`xr_latch_gaze`). It is projected through each eye's fov and smoothed
  (40 ms; a jump over 0.25 NDC snaps).
- **No gaze, no foveation:** with no gaze (no extension, no permission, not tracked) for 200 ms, no map is attached and
  everything renders at full density. There is no fixed or centred fallback. When the gaze comes back, the rings ramp in
  over 0.5 s, starting 4 x wider. `[fov] eye gaze lost: ...` / `[fov] eye gaze back: ...` log each change.
- **Mock (xrmock has no eye tracker):** `LARECOMP_VR_GAZE_MOCK=tx,ty` (tangents in the eye's frame) or `sweep` (a
  circle) stands in for the gaze. `LARECOMP_VR_GAZE_MOCK_DROP=on,off` (seconds) drops it in a cycle, to test the lost
  and ramp-in path.
- **Rings** (NDC radius from the focus; density per axis): low: 1 inside 0.6, 2x1 to 0.95, then 2x2. medium: 1 inside
  0.45, 2x2 to 0.8, then 4x2. high: 1 inside 0.35, 2x2 to 0.65, then 4x4.
- **Maps:** R8_UINT, optimal tiling (Turnip has no linear shading-rate images), one per eye and pass size. A change (a
  focus move of 8 px or more, the ramp) is copied in from transient memory at the start of the pending submission, at
  most once per submission. Every game pipeline gets `VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR`
  and combiner ops KEEP / REPLACE. Passes without an attachment shade at 1x1.
- **`LARECOMP_VR_FOVEATION_MODE=fdm`:** VK_EXT_fragment_density_map instead (non-subsampled). On Turnip 26.3 it draws
  whole bins black in a checkerboard whenever one map holds more than one density. A uniform map is fine, and hw
  binning, bin merging and concurrent binning are not the cause. It is kept for later drivers. Turnip reads density maps
  on the CPU when the pass is recorded, one density per bin, and forces those passes into GMEM.
- **VR tab:** `fov_set_level` changes the level live. If foveation was off when the game started, the extension is
  not enabled, so turning it on needs a restart.
- **Log:** `[fov] eye gaze in N of M game frames ... ; X of Y eye passes foveated` every 10 s.
  `LARECOMP_VR_FOVEATION_LOG=1` logs each pass decision (the first 400).

## Diorama placement

The rig (`runtime/xr/xr_rig.c`, docs/af/RIG.md) now places the table: per-area anchoring, the player's snap turn,
tilt, height, size and recentre, and the blink. What follows is the v1 placement it grew from; the switches below
still apply (`LARECOMP_VR_FOLLOW` set: the comfort follow instead of the rig's per-area anchoring).

`app = A + s * Ry(yaw) * (world - pivot)`:

- **pivot:** the table's anchor. The raw value is the screen centre of the game's camera at Link's height, once per
  game frame. `LARECOMP_VR_FOLLOW` says how the anchor follows it, on new game frames only:
  - `smooth` (default): filtered, time constant `LARECOMP_VR_FOLLOW_TAU` (0.5 s; height twice that, for Link's jumps).
  - `room`: held. When the camera zone changes it glides (as smooth) until the game's camera has settled, then holds.
  - `fixed`: held.
  - In every mode a cut snaps it: the raw pivot more than 24 units away (a level change, a teleport, a camera cut).
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
| `LARECOMP_STEREO_DUMP_AT=<frame>,...`, `LARECOMP_STEREO_DUMP_DIR` | eye outputs, the HUD clone and the window image as PNG |
| `LARECOMP_STEREO_PRELATCH=0` | latch the frame's camera at its first draw inside eye 0 (as before the fix; one eye flat while moving) |
| `LARECOMP_STEREO_CAMCHECK=0\|1\|2` | the per-draw camera check between the eyes (default 1; 2: details) |
| `LARECOMP_VR_FOLLOW=smooth\|room\|fixed`, `LARECOMP_VR_FOLLOW_TAU=<s>` | how the table follows the game camera (above) |
| `LARECOMP_XR_PREDICT=measured\|vsync` | the display time the eye poses are located for |
| `LARECOMP_VR_DIET=0\|dof,merge,smaa` | the VR pass diet (default all; docs/af/PASSES.md) |
| `LARECOMP_VR_DYNRES=1`, `_MIN` / `_MAX` / `_MS` | dynamic eye resolution (off by default; docs/af/PASSES.md) |
| `LARECOMP_GPU_OVERLAP=0\|1` | the CPU / GPU overlap (VR default on, flat off on Turnip; docs/af/PASSES.md) |
| `LARECOMP_VR_FOVEATION=off\|low\|medium\|high` | eye-tracked foveation of the eye passes (above) |

Mock run: `XR_RUNTIME_JSON=build/xrmock/runtime.json LARECOMP_XR=1 MOCK_XR_EYE_SIZE=1728x1728
MOCK_XR_POSE=static:0,1.6,0,0,-45 SDL_VIDEODRIVER=offscreen ENABLE_GAMESCOPE_WSI=0 ...`. The head looks 45 degrees down
at the table.

## Known limits

- Cost: double execution is draw-bound. At Mabe with widened culling, both eyes take about 29-31 ms of GPU at both
  1728² and 1400². The pixel count barely matters.
- Fades go to the HUD quad only. Frames with no scene (menus) keep the last eye images.
- A dispatch that writes a shared target from a twin gives eye 1 eye 0's result. Copies are twinned (above).
- Second perspective cameras (reflection, shadow, light) are not substituted.
- The loaded world ends at the zone edge (black void). There is no edge fade yet (lane 6).
