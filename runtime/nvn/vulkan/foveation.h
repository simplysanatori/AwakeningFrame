// Eye-tracked foveated rendering of the VR eye passes (AwakeningFrame "fov" lane). Docs: docs/af/STEREO.md "Foveation".
//
// VR mode only. Each eye-sized pass into twinned (per-eye) scalable targets gets a shading-rate attachment
// (VK_KHR_fragment_shading_rate; LARECOMP_VR_FOVEATION_MODE=fdm: a fragment density map instead): pixels away from the
// eye's focus are shaded at a coarser rate (2x1 .. 4x4), edges and depth stay at full resolution. The
// focus is the eye gaze (XR_EXT_eye_gaze_interaction, through the latch the eye was rendered with), smoothed.
// Eye-tracked only: with no gaze (no extension, no permission, or none tracked for 200 ms) there is no map (full
// density); when the gaze comes back the rings ramp in over 0.5 s. State changes are logged.
//
// LARECOMP_VR_FOVEATION=off|low|medium|high (default medium in VR). Flat mode: nothing changes (no extension, no
// pipeline flag, no map).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

// (device.c create_device, VR mode, before vkCreateDevice) enables the extension and its features when the level is
// not off and the device has them: appends to ext[] and chains a feature struct into f (kept alive here).
void fov_device_setup(VkPhysicalDevice phys, bool fdm_ext, bool fsr_ext, const char **ext, uint32_t *n_ext, VkPhysicalDeviceFeatures2 *f);
// Foveation is on (device ready, extension enabled, level not off).
bool fov_on(void);
// The VR tab's live hook (runtime/vr_settings.c): 0..3 = off, low, medium, high.
void fov_set_level(int level);
// Extra flags for every graphics pipeline that may draw in a foveated pass (0 when off).
VkPipelineCreateFlags fov_pipeline_flags(void);
// The pNext for those pipelines' VkPipelineRenderingCreateInfo (shading-rate mode: the attachment's rate replaces the
// pipeline's), or NULL. Constant, safe from any thread.
const void *fov_pipeline_chain(void);
// (stereo.c, at each latch of the eye views) the eyes' orientations (x y z w, app space) and fov angles (left, right,
// up, down; radians) of latch `id`: the focus of each eye for the passes that follow.
void fov_note_views(const float orient[2][4], const float fov[2][4], uint64_t id);
// (exec.c, each pass while eye `eye` executes; per_eye: its attachments are all twinned and scalable; render area
// w x h; fx / fy: the filled part of the image, 0..1 of w / h, from the top-left) the density map to chain into the
// VkRenderingInfo (a shading-rate or density-map attachment info, nothing chained), or NULL.
// LARECOMP_VR_FOVEATION_LOG=1 logs the first 400 decisions.
// eye 2: a multiview pass (both eyes; one map, the finer rate of the two foci)
const void *fov_pass(int eye, bool per_eye, uint32_t w, uint32_t h, float fx, float fy);
// (edge pass, render thread) how the latest eye-sized pass was shaded, so the edge pass can smooth the coarse blocks
// away; false: it had no shading-rate map (full rate). Per 8 px texel (centre x, y in that pass's pixels, rows from the
// top) the rate is the finest over the foci of: r = |((x, y) - c) / (fw / 2, fh / 2)|, 1x1 for r < r0, `mid` for r < r1,
// else `out`; past the filled part (x > fw + 8 or y > fh + 8) 4x4. Rates are log2 width, log2 height.
typedef struct { uint32_t w, h; int nfoci; float c[2][2], fw, fh, r0, r1; int mid[2], out[2]; } FovShade;
bool fov_shade(FovShade *out);
