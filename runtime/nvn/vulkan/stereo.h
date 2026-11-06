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

// ---- poses (the layer carries what the eyes were rendered with; docs/af/STEREO.md "Pose")
// (exec.c, at each eye's composite) that eye's output now holds an image rendered with the latest latch's cameras.
void stereo_note_composite(int eye);
// (exec.c vk_stereo_eyes, after drawing the eye outputs into the set's eye images) the set's projection layer gets the
// poses of the latches those outputs were rendered with (xr_set_views). Returns its result (-1: no latch known).
int stereo_set_views(void);
// The view and projection (game units, the game's conventions: row-major, clip = P * V * x) eye `eye`'s current output
// was rendered with. false: none (the game's own camera, or no output yet).
bool stereo_eye_matrices(int eye, float V[16], float P[16]);

// ---- the eye post hook (the "edge" lane's fog / backdrop pass)
// exec.c vk_stereo_eyes calls stereo_eye_post once per NEW eye output (a composite that was not shown before), before
// it is drawn into the XR eye image, outside any rendering, on the render thread. The default is an empty weak
// definition in exec.c: define a strong one elsewhere to use it. It may record commands into e->cb that change
// e->color in place (layout GENERAL; it must leave it GENERAL with its writes made visible to fragment reads: a full
// barrier is fine). e->depth is that eye's scene depth (GENERAL; read only), or NULL when the frame had none.
// Images are the executor's TexBackend (textures.c: ->image, ->view; raw_view() for the colour's plain view);
// e->ax / e->ay is the part of the image the frame filled (the scalable targets' top-left part, 0..1).
typedef struct {
    int eye;
    void *cb;                  // VkCommandBuffer
    void *color, *depth;       // TexBackend *
    uint32_t width, height;    // the eye output's image size (pixels)
    float ax, ay;              // the filled part (0..1 of width / height)
    float V[16], P[16];        // stereo_eye_matrices (identity when none)
    bool have_matrices;
    float zn, zf;              // the scene pass's depth range (st.depth_range of its viewport 0)
    uint32_t layer, depth_layer;  // (LARECOMP_MV) the image layer of color / depth that is this eye's (0 otherwise)
} StereoEyePost;
void stereo_eye_post(const StereoEyePost *e);
