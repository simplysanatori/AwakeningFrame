# Stereo executor

VR mode (`LARECOMP_XR=1`) draws the real game into both eye swapchains with a camera per eye. The HUD goes on the quad
layer. Code: `runtime/nvn/vulkan/stereo.c/.h` (switches, v1 diorama cameras), `exec.c` (the executor), `textures.c`
(eye twins), `runtime/nvn/cam_override.c` (per-eye substitution), and small hooks in `device.c` and `runtime/xr`.

Stereo is multiview: a list runs once and each pass draws both eyes (see "Multiview" below). `LARECOMP_MV` switches it
(1 on, 0 off). Without multiview the executor runs each list once
per eye, which is what the first section describes; multiview keeps the same camera, twin, HUD and pose machinery and
falls back to it for the lists it can't do in one run.

## Per-eye execution (the first version, and the fallback)

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

`LARECOMP_VR_FOVEATION=off|low|medium|high` (the code's default is medium in VR; the VR tab always sets it, default High; flat mode is unchanged). Foveation is eye-tracked
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
- **VR tab:** `fov_set_level` changes the level live, from Off too. Only `LARECOMP_VR_FOVEATION=off` in the launch
  environment leaves the extension out, and then there is nothing to turn on.
- **Log:** `[fov] eye gaze in N of M game frames ... ; X of Y eye passes foveated` every 10 s.
  `LARECOMP_VR_FOVEATION_LOG=1` logs each pass decision (the first 400).

## Diorama placement

The rig (`runtime/xr/xr_rig.c`, docs/af/RIG.md) places the table: per-area anchoring, the player's turn,
tilt, height, size and recentre, and the blink. What follows is the v1 placement it grew from; the switches below
still apply (`LARECOMP_VR_FOLLOW` set: the comfort follow instead of the rig's per-area anchoring).

`app = A + s * Ry(yaw) * (world - pivot)`:

- **pivot:** the table's anchor. The raw value is the screen centre of the game's camera at Link's height, once per
  game frame. `LARECOMP_VR_FOLLOW` says how the anchor follows it, on new game frames only:
  - `smooth` (default): filtered, time constant `LARECOMP_VR_FOLLOW_TAU` (0.5 s; height twice that, for Link's jumps).
  - `room`: held. When the camera zone changes it glides (as smooth) until the game's camera has settled, then holds.
  - `fixed`: held.
  - In every mode a cut snaps it: the raw pivot more than 24 units away (a level change, a teleport, a camera cut).
- **A:** the point the pivot sits on. It is the player's Distance (default 1.2 m, `LARECOMP_VR_DIST`) from the head's
  start, at `LARECOMP_VR_PITCH` degrees down (default 15, so Link is near the forward view; the table is then tilted 30
  degrees to be seen at the game camera's 45), or `LARECOMP_VR_TABLE=x,y,z`. See RIG.md, Default placement.
- **s:** `LARECOMP_VR_SCALE` (0.067 m per unit). `LARECOMP_VR_YAW` sets the yaw.
- **Depth:** the eye projections keep the game's depth terms. `LARECOMP_VR_NEAR=<units>` overrides them.

## Multiview (on where the device has multiview; `LARECOMP_MV=0`: off)

One run of a list draws both eyes (VK_KHR_multiview, `viewMask` 3) instead of running the list once per eye.

- **Twins are layer 1** (`textures.c`). A twin candidate (16:9 scalable target, guest-sized AO target) is made with
  two array layers (`TexBackend.mv2`). `b->layers` still says 1 because that is the guest's view. It becomes a twin
  (`mv_tw`) like a v1 twin does: when an eye-1 run or a multiview run starts and the texture has not been presented,
  or when eye 0 copies a twin into it (`vk_stereo_twin_add`). That case re-creates a one-layer image with two layers,
  both holding the old contents. A texture presented after it became a twin stays one. Single-layer views of a
  layered image take layer `vk_eye_layer`. `vk_view_mv` gives the 2D_ARRAY view of both layers.
- **Passes** (`exec.c begin_pass`, `mv_rt_kind`):
  - If every attachment is layered, the pass is multiview: 2D_ARRAY views, and the view mask goes into the pipeline
    keys and the clear pipeline.
  - If the only shared attachments are presented images (the HUD clone, the window image over the scene's twinned
    depth), eye 0's view is drawn alone. Eye 1's draws into those images are never shown.
  - Any other mix of layered and shared attachments makes eye 1 run the list again, as in v1. That run skips the
    multiview passes and the copies and clears into layered images. These lists are counted in the `[mv] 5 s` line.
  - A list with no such pass is done in one run, and its fences are signalled after it.
- **Per-eye camera** (`mv_program`, `cbuf_info`, `cam_pair`):
  - A multiview pass's draw uses the base program unless a vertex or fragment constant buffer (k >= 3) is a camera
    block (the 320-byte main camera, or a block `cam_override_patch` rewrites) or it samples a layered texture.
  - Otherwise it uses the variant whose `ShaderKey.mv` is the stage's mask of those buffers (bit i: cbuf i + 3, cbufs
    3-10). Each masked buffer is bound as an [eye 0 | eye 1] copy, with eye 1 `cbuf_mv_rows[k]` rows (16 bytes each)
    after eye 0. The translator declares it with twice the rows and reads `gl_ViewIndex == 0 ? cb[r] : cb[r + rows]`.
    Both indices stay constant, so the driver can still keep the block in constant registers. Other buffers are read as
    in the base program.
  - The first version took the masks from a push constant and offset every buffer's row by
    `gl_ViewIndex * bit * rows`. With every index non-constant, Turnip's GPU time went up by about 2.8 ms a frame at
    Mabe.
  - Sampled layered textures are bound as 2D arrays and read at layer `gl_ViewIndex` (`ShaderKey.tex_amask`).
  - The shader prep records carry `SR_MV` / `SR_MVP` / `v=`, so these variants are prepared before the game starts.
- **Copies and clears** into a layered image are done for both layers by eye 0's run. Feedback snapshots of a layered
  attachment have both layers.
- **Eye outputs:** the composite into the eye output is one multiview pass. `vk_stereo_eyes` presents layer `eye`.
  The eye post hook gets `layer` / `depth_layer` (`StereoEyePost`).
- **Foveation:** Turnip has no layered shading-rate attachments (`layeredShadingRateAttachments` false). A multiview
  pass gets one map (`fov_pass` eye 2): each texel takes the finer rate of the two eyes' foci. The density-map mode
  (`LARECOMP_VR_FOVEATION_MODE=fdm`) gives multiview passes no map.
- **Camera check:** the `[mv] 5 s` line counts the per-eye camera bindings ([eye 0 | eye 1] copies) and the camera
  blocks bound alike to both views of a multiview pass ("with eye 0's in both views", logged). Those would give eye 1
  eye 0's camera, so the count should be 0. The double-execution check (`[stereo] camera check`) runs only for lists
  that still have an eye-1 run.
- **Measured** (xrmock 1728², 90 Hz unless noted, foveation medium, gaze mock 0,0, dynres off; v1 → MV):
  - **GPU, both eyes:** Mabe 19.5-20.0 → 18.1-18.4 ms, forest 18.7 → 17.3, beach 20.5 → 18.2, Tail Cave 04B
    9.0 → 8.1, house 7.8-8.0 → 7.6-7.7.
  - **Render CPU:** Mabe 12.9-13.2 → 8.0-8.6 ms, forest 11.3 → 7.3. Every list is one run.
  - **At 120 Hz:** Mabe 53 → 60.0 fps, forest 30 → 59.9 fps. MV's GPU is 15.2 ms there.
  - **Eye dumps:** within v1's run-to-run noise at Mabe, the forest, the house, Tail Cave 04B and Field_09B (worst
    2.1% of pixels over 24/255; two v1 runs differ by up to 2.8%).
  - **Pose:** 0 pose mismatches, 0 sets with eyes of two frames.
- **Debugging:** `LARECOMP_DEBUG_FRAME` pass dumps are named by eye (`_e0` / `_e1`, `m`: a multiview pass, one PNG
  per layer). `LARECOMP_MV_PASSES=0` keeps the layered storage and runs each list per eye (stage 1: the storage
  checked against v1 alone).
- **Limits:**
  - Twins other than single-sample 2D images with one layer stay shared under MV (logged "[mv] candidate ... has no
    eye-1 layer", or "twin add ... refused"); v1 could twin any image.
  - A camera block in cbuf 11-17 can't be per eye (logged; none seen).
  - Turnip applies the shading-rate map to both views alike, so a multiview pass is foveated by the union of the two
    foci.
  - Turnip's multipos (`TU_DEBUG=nomultipos` changes nothing), so the vertex work is still done per view.

## Frame end

The doubling seen while turning the head in the first headset session (120 Hz, 60 fps, 2026-11-06) is not a pose or
content error. With xrmock's image probe (`MOCK_XR_SHIFT`, docs/af/TOOLING.md), after 5-degree head steps each eye
image matches the pose of its own layer to 0.001 degrees rms, from the first set after the step, in v1 and in multiview.
The cause is the compositor's frame end. SteamVR's `xrEndFrame` waits behind the app's GPU work on the one Turnip
queue. When the game's GPU time per frame nears the 16.7 ms frame, the wait reaches up to 17 ms. The frame misses its
vsync, and SteamVR shows the previous one again (reused) and the late one off its time (mispresented): the doubled
image. In the session, the slow frame ends per second track SteamVR's reused + mispresented count (r = 0.89, against
0.63 for GPU time alone).

The fix is headroom. `LARECOMP_VR_HEADROOM` (default on; `=0` for the old values) sets the dynamic resolution budget
to 10 ms (from 14.5) and its floor to 1152 px (from 1536). On SteamVR at 120 Hz in the Field, reused + mispresented
falls from 85 a second to 5. That only applies with Dynamic Resolution on: with it off, the eyes stay full size and
the Field doubles as before. 90 Hz does not help there: the Field holds 30 fps.

`xr_loop.c` logs the frame end every 5 s: `xrEndFrame avg / max`, the frame ends over 2 ms and those that waited over
1 ms for the queue lock. `LARECOMP_XR_INFLIGHT=<n>` (stereo, off by default) waits after each submission until at
most n older ones are on the GPU. It also stops the doubling, but at 30 fps, so it stays an experiment.

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
| `LARECOMP_VR_HEADROOM=0` | the dynamic resolution budget and floor from before the frame end fix (14.5 ms, 1536 px; above) |
| `LARECOMP_XR_INFLIGHT=<n>` | cap the submissions on the GPU behind the newest (off; above) |
| `LARECOMP_GPU_OVERLAP=0\|1` | the CPU / GPU overlap (VR default on, flat off on Turnip; docs/af/PASSES.md) |
| `LARECOMP_VR_FOVEATION=off\|low\|medium\|high` | eye-tracked foveation of the eye passes (above) |
| `LARECOMP_MV=0\|1` | multiview stereo (default: on where the device has multiview): twins as layer 1, one run per list (above); 0: stereo v1; 1: the layered twins even without multiview |
| `LARECOMP_MV_PASSES=0` | with `LARECOMP_MV=1`: the layered twins, but each list still runs once per eye |

Mock run: `XR_RUNTIME_JSON=build/xrmock/runtime.json LARECOMP_XR=1 MOCK_XR_EYE_SIZE=1728x1728
MOCK_XR_POSE=static:0,1.6,0,0,-45 SDL_VIDEODRIVER=offscreen ENABLE_GAMESCOPE_WSI=0 ...`. The head looks 45 degrees down
at the table.

## Known limits

- Cost: per-eye execution is draw-bound. At Mabe with widened culling, both eyes take about 29-31 ms of GPU at both
  1728² and 1400². The pixel count barely matters. Multiview exists to cut that.
- Fades go to the HUD quad only. Frames with no scene (menus) keep the last eye images.
- A dispatch that writes a shared target from a twin gives eye 1 eye 0's result. Copies are twinned (above).
- Second perspective cameras (reflection, shadow, light) are not substituted.
- The loaded world ends at the zone edge (black void) unless the Table Edge is on (docs/af/EDGE.md).
