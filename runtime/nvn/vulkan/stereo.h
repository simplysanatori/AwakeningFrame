// Stereo executor (AwakeningFrame lane 4; design: ~/.claude/plans/awakeningframe-vr-design.md sections 2-4, 6).
// VR mode (LARECOMP_XR=1): each game frame's command lists run once per eye (exec.c backend_execute), with the
// camera substituted per eye in the host-side copies of the constant blocks (runtime/nvn/cam_override.c), the 16:9
// render targets twinned per eye (textures.c vk_stereo_swap), passes into other targets (shadow maps, cube maps,
// small offscreen targets) run once and shared, the scene's composite drawn into a per-eye output, and the window
// image (now transparent) keeping only the HUD / layout / menus for the quad layer. Docs: docs/af/STEREO.md.
//
// This file: the switches, and the eye cameras of the v1 diorama (a fixed world-to-table transform; lane 6 replaces
// it with the rig). The executor itself is in exec.c (it needs the translation's state).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// LARECOMP_XR=1 and LARECOMP_STEREO not 0 (read once).
bool stereo_wanted(void);
// stereo_wanted() and the XR session runs (eye swapchains exist): the executor runs each list per eye.
bool stereo_active(void);
// The render size of each eye (the eye swapchain's; LARECOMP_STEREO_SIZE=WxH overrides). false: not active.
bool stereo_eye_size(uint32_t *w, uint32_t *h);
// Turns the per-eye cameras on in cam_override (once, from the executor when it first runs).
void stereo_cameras_init(void);
// Stats: per-eye GPU ms of a finished game frame (exec.c's timestamps), for the log and xr_timing.log.
void stereo_note_gpu(uint64_t frame, double ms0, double ms1, int lists);
