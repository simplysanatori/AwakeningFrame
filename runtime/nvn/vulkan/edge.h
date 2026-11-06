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

// The eye post hook (stereo.h stereo_eye_post, defined in edge.c): once per new eye output it notes that eye's colour
// image, scene depth, matrices and depth range. Nothing is drawn there: the eye output is a B10G11R11 image (no alpha,
// 5-6 mantissa bits: a smooth gradient written back into it would band), so the result goes straight into the XR eye
// image instead.
//
// edge_eye: records the passes for eye `eye` from the latest noted inputs into the frame's command buffer outside any
// rendering, then draws the result into the XR eye image `target` (extent w x h, format fmt; COLOR_ATTACHMENT_OPTIMAL
// in and out) in place of the present draw (exec.c vk_stereo_eyes, one line). false: mode black, no depth noted, or a
// failure (logged once): the caller draws the eye as before and nothing was recorded.
bool edge_eye(VkCommandBuffer cb, int eye, VkImageView target, VkFormat fmt, uint32_t w, uint32_t h);
