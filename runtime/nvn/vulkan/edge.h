// Edge treatment of the VR diorama (AwakeningFrame lane "edge"; docs/af/EDGE.md).
//
// The loaded world ends at the zone edge and beyond it the eye output is cleared black. This pass runs on each eye
// output on its way into the XR eye image: it reads the eye output's colour and the scene depth once, and writes
//   - void pixels (no geometry: the depth is still cleared) -> a backdrop, a vertical gradient tinted from the scene
//   - geometry -> blended toward that backdrop by a soft fade: a distance fog from the eye, and a fade where the loaded
//     world meets the void (a blurred coverage mask, so no hard straight edge), dithered against banding
//   - LARECOMP_VR_EDGE=passthrough: the same weights as alpha (the void is transparent; premultiplied)
// The backdrop colours are the average colours of the top and bottom bands of the scene's geometry, smoothed over time
// on the GPU (no read-back).
//
// Modes (LARECOMP_VR_EDGE): fog (default), gradient, black (off: edge_eye returns false and the caller's present runs
// as before), passthrough. Everything else is in docs/af/EDGE.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef enum { EDGE_BLACK = 0, EDGE_FOG, EDGE_GRADIENT, EDGE_PASSTHROUGH } EdgeMode;

// The mode (LARECOMP_VR_EDGE, read once).
EdgeMode edge_mode(void);
// Passthrough wanted (mode passthrough): xr_loop.c's xrEndFrame then needs environmentBlendMode ALPHA_BLEND (when the
// runtime offers it) and XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT on the projection layer (edge_layer_flags).
bool edge_passthrough_wanted(void);

typedef struct {
    int eye;                    // 0 left, 1 right (eye 0 also updates the backdrop colours)
    VkImageView color;          // the eye output (sampled; GENERAL layout)
    VkImageView depth;          // the scene depth, depth aspect (sampled; GENERAL). VK_NULL_HANDLE: edge_eye returns false
    float color_crop[2];        // the present's crop of the colour: the visible part of the image, as (fx, fy) of its size
    float depth_crop[2];        // the same for the depth image
    bool origin_lower_left;     // renderer_origin_lower_left(): the present's flip
    float V[16], P[16];         // this eye's view and projection (row-major, column vectors), as the game's camera
    float zn, zf;               // st.depth_range (0, 1 normally)
    bool depth_minus1;          // nvn_depth_mode() == 0: depth is stored as (ndc z + 1) / 2
    float pivot_dist;           // eye to the diorama pivot in game units; <= 0: LARECOMP_VR_DIST / LARECOMP_VR_SCALE
} EdgeEye;

// Records the passes into the frame's command buffer outside any rendering, then draws the result into the XR eye
// image `target` (extent w x h, format fmt; COLOR_ATTACHMENT_OPTIMAL in and out) in place of the present draw.
// false: edge off (mode black), no depth, or a failure (logged once): the caller draws the eye as before and nothing
// was recorded.
bool edge_eye(VkCommandBuffer cb, const EdgeEye *in, VkImageView target, VkFormat fmt, uint32_t w, uint32_t h);

// The eye cameras of the latest game frame (stereo.c eye_fn, once per frame): V[eye], P[eye] row-major as the game's
// camera, pivot_dist the eye's distance to the diorama pivot in game units. Until the rig lane's hook hands them over
// with each eye, this is how edge_eye's caller fills EdgeEye.V / .P / .pivot_dist (edge_cameras).
void edge_note_cameras(const float V[2][16], const float P[2][16], float pivot_dist);
bool edge_cameras(int eye, float V[16], float P[16], float *pivot_dist);
