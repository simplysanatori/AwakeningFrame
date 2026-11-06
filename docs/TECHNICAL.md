# How the port works

Technical notes for people working on the port. For playing, see the
[README](../README.md) and [CONTROLS.md](CONTROLS.md). Building is in
[af/BUILD-frame.md](af/BUILD-frame.md).

## Presentation in the headset

- Gameplay is shown as a **diorama**. The loaded part of Koholint sits on a
  table about 1 m in front of you, at the game camera's own pitch below the
  horizon (about 45 degrees at Mabe Village), at 0.067 m per game unit: one
  tile is about 10 cm. You look around it and lean in with your own head.
  The game camera's movement never moves your view; it only says where the
  table's centre is.
- In the overworld the table's centre follows the game camera's focus at
  Link's height, filtered (half a second; the height twice that, so Link's
  jumps don't bob the world). In a house or a dungeon each room is held
  still at its centre, at the height it was entered at, and a room bigger
  than a screen keeps the screen inside it. A room change, a door, a warp or
  a camera cut snaps the table behind a blink. A new room has to hold for
  three game frames first, so a zone read that flickers during a load never
  blinks.
- **Side views** (a camera zone thinner than 6 units deep, such as Bottle
  Grotto's side-scrolling room) stand upright like a wall diorama: the table
  turns so the side plane faces you and drops to about eye height. The
  player's turn and tilt are not used there and come back when you leave.
- The left stick is turned by the table's yaw, so pushing it up always walks
  Link away from you.
- **The blink** is on screen only, with no sound. The present multiplies the
  eye images and the HUD by `1 - fade`. Turns and resets fade out, change at
  the first black game frame, and fade in. Automatic cuts go black at once on
  the frame that would have jumped and fade in from there. A blink with no
  game frame coming (a menu) fades back in by itself.
- The **HUD panel**: hearts, text, item cards and menus go to a flat quad
  layer straight ahead at eye height, 2 m away and 2.2 m wide by default.
  The game's 16:9 UI box is cropped to fill it. The game's own fades to black
  are on the panel too.
- **Table edge** (off by default): past the loaded zone the world ends in a
  void. *Fade* ends it in a round fade to black around the table's centre, in
  every room: each pixel's distance from the table's anchor is taken from the
  scene depth, or from where its ray meets the floor. *Passthrough* (overworld
  only) writes the void and the fade as alpha and asks SteamVR to blend the eye
  images over your room. The mode can change while the game runs: the next eye
  is drawn in the new one.

## How it works

| Piece | Where | Notes |
|---|---|---|
| Game | `generated/` (made by `make gen` from your `exefs`) | LAS Recomp's static recompilation, built into `lasgame.so` on your machine. None of it is in this repository |
| Renderer | `runtime/nvn/vulkan/` | LAS Recomp's NVN-on-Vulkan renderer: the game's command lists translated to Vulkan, its Maxwell shaders translated to GLSL and compiled to SPIR-V |
| OpenXR | `runtime/xr/` | Session, swapchains and frame loop (`xr_session.c`, `xr_loop.c`, `xr_vk.c`), controllers (`xr_input.c`), the diorama rig (`xr_rig.c`) |
| Stereo | `runtime/nvn/vulkan/stereo.c`, `exec.c`, `textures.c`, `runtime/nvn/cam_override.c` | Draws both eyes in one multiview pass, each with its own camera |
| Foveation | `runtime/nvn/vulkan/foveation.c` | Eye-tracked shading rate per eye pass |
| Menu laser | `runtime/xr/xr_pointer.c`, `xr_input.c` | The beam and dot layers, and the pointing hand's click and scroll |
| Edge | `runtime/nvn/vulkan/edge.c` | The table edge |
| VR passes, dynamic resolution | `runtime/nvn/vulkan/vr_diet.c`, `runtime/nvn/output_scale.c` | What the VR mode leaves out, and the eye resolution |
| VR tab | `runtime/vr_settings.c`, `runtime/ui/vr_tab.mm` | The settings, saved in `larecomp.ini` |
| Frame scripts | `install.sh`, `uninstall.sh`, `steam-frame/` | Install, Steam entry and SteamVR settings, launch wrapper, game extraction |

The OpenXR loader is SteamVR's own (`/opt/steamvr/bin/linuxarm64`), opened at
run time. The game creates its Vulkan instance and device through
`XR_KHR_vulkan_enable2`, so the eye images are drawn on SteamVR's device.

### Stereo

VR mode draws the real game into both eye swapchains in one pass, with a camera per eye.

- Each of the game's command lists runs once. A render pass whose attachments are all **twinned** draws both eyes with
  `VK_KHR_multiview` (view mask 3). Twinned targets are the 16:9 render targets that scale with the resolution, and the
  ambient occlusion targets; each is one image with two array layers, eye 1 in layer 1. Shadow maps, cube maps and
  small offscreen targets are drawn once and shared. Compute, counters and read-backs run once. A copy from a twinned
  texture into a shared one twins the destination from then on.
- A pass that mixes layered and shared attachments falls back to running the list a second time for eye 1, with the
  bound state restored and the game's memory writes from the first run undone and redone. Such lists are counted in
  the log. Presented images, such as the HUD, are drawn for eye 0 alone.
- **Camera.** The game's main camera is the first perspective 320-byte uniform buffer of a frame. It is found before
  the list runs, so every draw of the frame sees the same camera. Per eye, the view, projection and view-projection,
  the rotation block and the eye position are replaced in a host copy of it, and every other bound block that holds the
  same matrices or eye position is rewritten too. Guest memory is never written. Culling is widened so the table's
  sides are drawn.
- **Per-eye blocks.** A draw whose vertex or fragment constant buffers hold the camera, or that samples a layered
  texture, uses a shader variant that reads eye 0's or eye 1's copy by `gl_ViewIndex`. Both indices stay constant, so
  the driver keeps the block in constant registers. Other draws use the plain program. The variants are prepared with
  the rest of the shaders before the game starts.
- **Fences** of a list are signalled only after it has run for both eyes, so the game never sees a frame as done while
  the second eye still reads its data.
- **HUD.** The window image has no alpha, so after the scene's composite the game's UI draws go into a separate
  RGBA16F image cleared to transparent, with blended draws accumulating coverage. That image goes to the quad layer
  with source alpha.
- **Pose.** The head is located once per game frame, at the frame's first camera draw, for the time that frame is
  expected on the display (the measured time from that point to its first refresh). Each eye output keeps the pose it
  was rendered with, and the projection layer is submitted with those poses, so a refresh that repeats an older frame
  carries the older poses.
- The table moves only on new game frames, so it is never shown at two places within one game frame. Smooth turning is
  integrated per game frame for the same reason.
- `LARECOMP_MV=0` switches multiview off: every list then runs once per eye, as in earlier builds. A driver without
  multiview falls back the same way, which the game notes in its log at start.

### Pacing

- At 120 Hz the game runs at 60 frames a second and each frame is shown for
  two refreshes. At 90 Hz it is held at 30 frames a second, three refreshes
  each.
- In VR mode the game thread no longer waits for its own frame's GPU work:
  the game's frame fence is waited on one frame late, so the CPU and the GPU
  run side by side. By default the game's memory reaches shaders as copies,
  made at translation, so the GPU never sees the next frame's data.
- The game's memory can instead be mapped as the driver's own (`LARECOMP_VK_HEAP_DMABUF=1`,
  as on RADV and PanVK), which saves those copies. On the Frame it commits the
  whole heap up front, so it is optional for now and being reworked.
- When the GPU falls behind at 120 Hz, the pacer holds the game at 30 frames
  a second and returns to 60 once the GPU fits 90% of a two-refresh frame.
- The launch wrapper sets SteamVR's per-app keys at every start
  (`preferredRefreshRate`, `framesToThrottle=0`, `additionalFramesToPredict=0`,
  no motion smoothing), because SteamVR does not offer 120 Hz through the
  OpenXR refresh-rate extension and drops the keys at times.

### Foveation

- Eye-tracked only. Each eye pass whose attachments are twinned and at
  least three quarters of the eye size gets a shading-rate attachment
  (`VK_KHR_fragment_shading_rate`, 8-pixel texels). Away from the focus,
  pixels are shaded at a coarser rate, up to 4x4 at High. Rasterisation,
  depth and geometry edges stay at full resolution. Turnip has no layered
  shading-rate attachments, so a multiview pass gets one map, each texel at
  the finer rate of the two eyes' foci.
- The level can change while the game runs, including from Off. Only
  `LARECOMP_VR_FOVEATION=off` in the launch environment leaves the extension out.
- The focus is the eye gaze (`XR_EXT_eye_gaze_interaction`), located for the
  frame's display time, projected through each eye and smoothed.
- With no gaze for 200 ms, no map is attached and everything renders at full
  density: there is no fixed fallback. When the gaze comes back, the rings
  ramp in over half a second.

### Dynamic resolution and the VR passes

- **Dynamic Resolution** draws the eyes at a fraction of the eye size inside
  the same targets and scales that part up to the whole eye image. It
  decides every half second from the game's GPU time per frame: down quickly
  when frames go over, up one step at a time when there has been room for a
  while. The HUD's box follows the same factor.
- **The VR passes.** VR mode leaves out the tilt-shift depth-of-field chain
  when it would not change the image, merges consecutive draws on the same
  attachments into one render pass (the HUD's draws become one pass), and
  skips SMAA on the menu image.

### Shader preparation

The game builds each shader the first time a draw needs it. LAS Recomp ships
a list of shader variants, recorded from a tour of every warp spot, and its
start screen prepares them. The Frame build starts with `--play`, straight
into the game, so the preparation now runs at every start before the first
frame, within a time budget (20 seconds by default). What it does not finish
compiles on demand, lands in the pipeline cache and is ready at the next
start.

- Variants the game builds that the shipped list lacks, including the ones
  only the VR mode makes, are recorded in `cache/shader_seen.txt` and
  prepared at the next start.
- The pipeline cache's tag holds only the GPU, driver and cache format, so a
  new build keeps it. It is saved a few seconds after a burst of compiles
  ends, not only at exit, so a crash or SteamVR closing the game loses little.
- `tools/warm_shaders.sh` (or `install.sh --warm`) plays the game headless
  through all 410 warp spots, in VR under the mock runtime and flat, and
  fills the caches ahead of play. It is optional.

## Settings and the environment

The VR tab's rows map onto environment variables the VR code reads, and are
saved in `larecomp.ini`. A variable set in the launch environment
(`~/Games/AwakeningFrame/session.env`, `NAME=value` lines) wins and greys
its row out. A change in the menu goes straight to the running code through a
hook for each row, so every row applies at once: Turning, Controller Layout,
Buttons Match Labels, Pointer Hand, Follow Mode, Height, Distance, Size, Tilt, Table
Edge, Foveation, Dynamic Resolution and the panel's place. The exception is
the refresh rate, which SteamVR's runtime won't let the game change. The launch
wrapper applies it before the game starts, so it takes a relaunch; only the
switch between the two 90 Hz choices is live. Height, distance, size and tilt belong to
the rig and are saved in `vr_rig.ini`. The full list is in
[af/VR_SETTINGS.md](af/VR_SETTINGS.md).

## Development tools

- `tools/xrmock/`: a mock OpenXR runtime for running the VR mode without a
  headset, with scripted head poses and controllers and eye-image captures.
- `tools/warm_shaders.sh`: the shader tour above.
- `tools/vr_tour.sh` and `tools/mv_check.sh`: a tour of the game's spots under the
  mock runtime with a report of timing and eye images, and a check that the eye
  images of multiview and of per-eye runs agree.
- The free-camera probe (`runtime/nvn/cam_override.c`, the
  `LARECOMP_FREECAM_*` variables): replaces the game's camera on the flat
  window, to see how the world holds up from other angles.
- `steam-frame/test-install.sh`: install, update, launch and uninstall
  against scratch folders and stand-ins for Steam and SteamVR.
- `steam-frame/privacy-check.sh`: checks what this branch added for personal
  data.

The working notes behind each part are in [docs/af/](af/).
