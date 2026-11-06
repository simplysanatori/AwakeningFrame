// Eye-tracked foveated rendering of the VR eye passes (foveation.h).
//
// Default: VK_KHR_fragment_shading_rate attachments (VRS). Each eye's map (R8_UINT, 8 px texels) sets a coarser shading
// rate away from the focus; rasterisation, depth and edges stay at full resolution, and it works in Turnip's sysmem and
// GMEM modes alike. The map is optimal-tiled (Turnip has no linear shading-rate images), so a change is copied in from
// transient memory at the start of the pending submission.
//
// LARECOMP_VR_FOVEATION_MODE=fdm: VK_EXT_fragment_density_map instead. On Turnip 26.3 this draws whole bins black in a
// checkerboard whenever one map holds more than one density (a uniform map is fine; hw binning, bin merging and
// concurrent binning are not the cause). Kept for later drivers. Turnip facts (tu_tile_config.cc / tu_image.cc /
// tu_autotune.cc):
//   - the density map is read on the CPU when the pass is recorded (vkCmdEndRendering), from the map image's own
//     mapping (linear, host-visible); the GPU never reads it. So a map is rewritten only once the submission of the
//     last pass that named it is complete (recorded long before), from a small ring per (eye, size).
//   - one density per bin, sampled at the bin's centre; the map texel is 2^ceil(log2(fb / map)) px (32 here: map =
//     ceil(fb / 32)). Areas are rounded down to powers of two (1, 2, 4 px a side); a density of 0 skips the bin.
//   - a pass with a density map is forced into the tiled (GMEM) mode by the autotuner; the sysmem path ignores maps.
//   - pipelines need VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT: their FragCoord is then
//     the framebuffer position (Turnip patches it per bin), so texelFetch(gl_FragCoord) shaders stay right.
#include "foveation.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../platform.h"
#include "../../rt.h"
#include "vk.h"
#include "../renderer.h"
#include "record.h"
#include "stereo.h"
#ifdef LARECOMP_HAVE_XR
#include "../../xr/xr.h"
#endif

enum { LV_OFF, LV_LOW, LV_MEDIUM, LV_HIGH };
#define FOV_RING 6
#define FOV_MAPS 16
#define FOV_TEXEL 32
static const VkFormat k_fmt = VK_FORMAT_R16G16_SFLOAT;

typedef struct {
    VkImage image;
    VmaAllocation mem;
    VkImageView view;
    uint8_t *ptr;
    VkDeviceSize pitch;
    uint64_t used_seq;  // the pending submission of the last pass that named it
    uint32_t key;       // what it holds (map_key)
} FovSlot;
typedef struct {
    int eye;
    uint32_t w, h, mw, mh;
    FovSlot ring[FOV_RING];
    int cur;            // -1: none filled yet
} FovMap;

// a shading-rate map (VRS mode): R8_UINT, 8 px texels, optimal tiling (Turnip has no linear shading-rate images), so
// each change is copied in from transient memory at the start of the pending submission (vk_upload_cmd)
#define VRS_TEXEL 8
typedef struct {
    int eye;
    uint32_t w, h, mw, mh;
    VkImage image;
    VmaAllocation mem;
    VkImageView view;
    bool init;
    uint32_t key;
    uint64_t seq;       // the submission of its last update (one update per submission)
} VrsMap;

static struct {
    bool init, dev_ok, log_passes;
    bool vrs;           // shading-rate attachments (default); false: density maps (LARECOMP_VR_FOVEATION_MODE=fdm)
    float uniform;      // (debug) LARECOMP_VR_FOVEATION_UNIFORM=<density>: the whole map at one density
    int level;
    float min_scale;    // passes smaller than this part of the eye size are left alone
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT feat;
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr_feat;
    VrsMap vmaps[FOV_MAPS];
    int nvmaps;
    VkRenderingFragmentShadingRateAttachmentInfoKHR fsr_info;
    FovMap maps[FOV_MAPS];
    int nmaps;
    VkRenderingFragmentDensityMapAttachmentInfoEXT info;
    // the focus (NDC of each eye, y up), smoothed
    float focus[2][2];
    bool have_focus, tracking;  // tracking: a gaze within the last 200 ms (else no map: full density)
    float ramp;         // 0..1: the rings ramp in over 0.5 s once the gaze is back
    uint64_t gaze_ns;   // last frame with a gaze
    uint64_t t_prev, t0;
    // mock gaze (xrmock has none): LARECOMP_VR_GAZE_MOCK=tx,ty (tangents in each eye's frame) or sweep;
    // LARECOMP_VR_GAZE_MOCK_DROP=on,off (seconds): the mock gaze is there for `on` s, then gone for `off` s, repeating
    int mock;           // 0 none, 1 static, 2 sweep
    float mock_t[2], drop_on, drop_off;
    // stats
    unsigned st_gaze, st_frames, st_fdm, st_eye_passes;
    uint64_t st_t;
} F = {.level = -1};

static void cfg(void) {
    if (F.init) return;
    F.init = true;
    const char *e = getenv("LARECOMP_VR_FOVEATION");
    F.level = LV_MEDIUM;
    if (e && *e) {
        if (!strcmp(e, "off") || !strcmp(e, "0")) F.level = LV_OFF;
        else if (!strcmp(e, "low")) F.level = LV_LOW;
        else if (!strcmp(e, "high")) F.level = LV_HIGH;
    }
    if (!stereo_wanted()) F.level = LV_OFF;
    F.min_scale = (e = getenv("LARECOMP_VR_FOVEATION_MIN")) && atof(e) > 0 ? (float)atof(e) : 0.75f;
    F.log_passes = getenv("LARECOMP_VR_FOVEATION_LOG") != NULL;
    if ((e = getenv("LARECOMP_VR_GAZE_MOCK")) && *e) {
        if (!strcmp(e, "sweep")) F.mock = 2;
        else if (sscanf(e, "%f,%f", &F.mock_t[0], &F.mock_t[1]) == 2) F.mock = 1;
    }
    if ((e = getenv("LARECOMP_VR_GAZE_MOCK_DROP")) && sscanf(e, "%f,%f", &F.drop_on, &F.drop_off) != 2) F.drop_on = F.drop_off = 0;
    F.vrs = !((e = getenv("LARECOMP_VR_FOVEATION_MODE")) && !strcmp(e, "fdm"));
    F.uniform = (e = getenv("LARECOMP_VR_FOVEATION_UNIFORM")) ? (float)atof(e) : 0;
}

static const char *k_names[] = {"off", "low", "medium", "high"};

static bool setup_vrs(VkPhysicalDevice phys, bool have_ext, const char **ext, uint32_t *n_ext, VkPhysicalDeviceFeatures2 *f) {
    if (!have_ext) { LOG("[fov] foveation unavailable: no VK_KHR_fragment_shading_rate\n"); return false; }
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have};
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pp};
    vkGetPhysicalDeviceProperties2(phys, &p2);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R8_UINT, &fp);
    bool fmt = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) != 0;
    bool texel = pp.minFragmentShadingRateAttachmentTexelSize.width <= VRS_TEXEL && pp.maxFragmentShadingRateAttachmentTexelSize.width >= VRS_TEXEL &&
                 pp.minFragmentShadingRateAttachmentTexelSize.height <= VRS_TEXEL && pp.maxFragmentShadingRateAttachmentTexelSize.height >= VRS_TEXEL;
    if (!have.attachmentFragmentShadingRate || !have.pipelineFragmentShadingRate || !fmt || !texel) {
        LOG("[fov] foveation unavailable: shading-rate attachments %d, pipeline rates %d, R8_UINT %d, 8 px texels %d\n",
            have.attachmentFragmentShadingRate, have.pipelineFragmentShadingRate, fmt, texel);
        return false;
    }
    ext[(*n_ext)++] = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
    F.fsr_feat = (VkPhysicalDeviceFragmentShadingRateFeaturesKHR){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
                                                                  .pipelineFragmentShadingRate = VK_TRUE,
                                                                  .attachmentFragmentShadingRate = VK_TRUE, .pNext = f->pNext};
    f->pNext = &F.fsr_feat;
    LOG("[fov] foveation %s, eye-tracked only: full rate until an eye gaze arrives (shading-rate attachments, 8 px texels, "
        "fragments up to %ux%u)\n", k_names[F.level], pp.maxFragmentSize.width, pp.maxFragmentSize.height);
    return true;
}

static bool setup_fdm(VkPhysicalDevice phys, bool have_ext, const char **ext, uint32_t *n_ext, VkPhysicalDeviceFeatures2 *f) {
    if (!have_ext) { LOG("[fov] foveation unavailable: no VK_EXT_fragment_density_map\n"); return false; }
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have};
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(phys, k_fmt, &fp);
    bool fmt = (fp.linearTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT) != 0;
    if (!have.fragmentDensityMap || !have.fragmentDensityMapNonSubsampledImages || !fmt) {
        LOG("[fov] foveation unavailable: density maps %d, non-subsampled images %d, linear R16G16 maps %d\n",
            have.fragmentDensityMap, have.fragmentDensityMapNonSubsampledImages, fmt);
        return false;
    }
    ext[(*n_ext)++] = VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME;
    F.feat = (VkPhysicalDeviceFragmentDensityMapFeaturesEXT){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT,
                                                             .fragmentDensityMap = VK_TRUE,
                                                             .fragmentDensityMapNonSubsampledImages = VK_TRUE, .pNext = f->pNext};
    f->pNext = &F.feat;
    VkPhysicalDeviceFragmentDensityMapPropertiesEXT pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pp};
    vkGetPhysicalDeviceProperties2(phys, &p2);
    LOG("[fov] foveation %s, eye-tracked only: full density until an eye gaze arrives (density maps, texels %ux%u to %ux%u px)\n",
        k_names[F.level], pp.minFragmentDensityTexelSize.width, pp.minFragmentDensityTexelSize.height,
        pp.maxFragmentDensityTexelSize.width, pp.maxFragmentDensityTexelSize.height);
    return true;
}

void fov_device_setup(VkPhysicalDevice phys, bool fdm_ext, bool fsr_ext, const char **ext, uint32_t *n_ext, VkPhysicalDeviceFeatures2 *f) {
    cfg();
    if (F.level == LV_OFF) { LOG("[fov] foveation off (LARECOMP_VR_FOVEATION)\n"); return; }
    F.dev_ok = F.vrs ? setup_vrs(phys, fsr_ext, ext, n_ext, f) : setup_fdm(phys, fdm_ext, ext, n_ext, f);
}

bool fov_on(void) { return F.dev_ok && g_vk.dev; }

VkPipelineCreateFlags fov_pipeline_flags(void) {
    return !F.dev_ok ? 0 : F.vrs ? VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
                                 : VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
}

const void *fov_pipeline_chain(void) {
    // the attachment's rate replaces the pipeline's (1x1); passes without an attachment: 1x1
    static const VkPipelineFragmentShadingRateStateCreateInfoKHR st = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR, .fragmentSize = {1, 1},
        .combinerOps = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR}};
    return F.dev_ok && F.vrs ? &st : NULL;
}

// ---------------------------------------------------------------- the focus
static void quat_rot_inv(const float q[4], const float v[3], float out[3]) {  // conj(q) * v * q
    float x = -q[0], y = -q[1], z = -q[2], w = q[3];
    float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// tangents (eye frame: x right, y up) -> the eye's NDC (y up)
static void tan_ndc(const float fov[4], float tx, float ty, float out[2]) {
    float tl = tanf(fov[0]), tr = tanf(fov[1]), tu = tanf(fov[2]), td = tanf(fov[3]);
    out[0] = (2 * tx - (tr + tl)) / (tr - tl);
    out[1] = (2 * ty - (tu + td)) / (tu - td);
}

void fov_note_views(const float orient[2][4], const float fov[2][4], uint64_t id) {
    cfg();
    if (!F.dev_ok) return;
    uint64_t now = plat_now_ns();
    float dt = F.t_prev ? (float)((now - F.t_prev) * 1e-9) : 0.0f;
    F.t_prev = now;
    float target[2][2];
    bool gaze = false;
    float d[3];
#ifdef LARECOMP_HAVE_XR
    if (!F.mock && xr_latch_gaze(id, d)) {
        gaze = true;
        for (int e = 0; e < 2 && gaze; e++) {
            float v[3];
            quat_rot_inv(orient[e], d, v);
            if (v[2] > -0.2f) { gaze = false; break; }  // (behind / far outside the eye's view)
            tan_ndc(fov[e], v[0] / -v[2], v[1] / -v[2], target[e]);
        }
    }
#else
    (void)id, (void)orient, (void)d;
#endif
    if (!F.t0) F.t0 = now;
    if (F.mock) {
        float tx = F.mock_t[0], ty = F.mock_t[1], t = (float)((now - F.t0) * 1e-9);
        if (F.mock == 2) {  // (a circle of radius 0.5 in tangents, once per 8 s)
            float a = fmodf(t, 8.0f) * (float)(M_PI / 4);
            tx = 0.5f * cosf(a), ty = 0.5f * sinf(a);
        }
        for (int e = 0; e < 2; e++) tan_ndc(fov[e], tx, ty, target[e]);
        gaze = !(F.drop_on > 0 && fmodf(t, F.drop_on + F.drop_off) >= F.drop_on);
    }
    F.st_frames++;
    F.st_gaze += gaze;
    if (gaze) F.gaze_ns = now;
    // Eye-tracked only: no gaze for 200 ms -> no map (full density); back -> the rings ramp in over 0.5 s
    bool tracking = F.gaze_ns && now - F.gaze_ns < 200000000ull;
    if (tracking != F.tracking) {
        LOG("[fov] %s\n", tracking ? "eye gaze back: foveation ramping in" : F.gaze_ns ? "eye gaze lost: foveation off (full density)"
                                                                                : "no eye gaze: foveation off (full density)");
        F.tracking = tracking;
        if (!tracking) F.ramp = 0;
    }
    if (tracking) F.ramp = fminf(1.0f, F.ramp + dt / 0.5f);
    for (int e = 0; e < 2 && gaze; e++) {
        for (int k = 0; k < 2; k++) target[e][k] = fminf(fmaxf(target[e][k], -1.2f), 1.2f);
        float dx = target[e][0] - F.focus[e][0], dy = target[e][1] - F.focus[e][1];
        // smoothed (40 ms); a jump (a saccade) or the first gaze after a loss snaps
        float a = !F.have_focus || F.ramp <= dt / 0.5f + 1e-3f || dx * dx + dy * dy > 0.25f * 0.25f ? 1.0f : 1.0f - expf(-dt / 0.04f);
        F.focus[e][0] += dx * a, F.focus[e][1] += dy * a;
    }
    if (gaze) F.have_focus = true;
    if (!F.st_t) F.st_t = now;
    if (now - F.st_t >= 10000000000ull) {
        LOG("[fov] eye gaze in %u of %u game frames%s (left eye focus at %.2f %.2f); %u of %u eye passes foveated\n", F.st_gaze,
            F.st_frames, F.mock ? " (mock)" : "", F.focus[0][0], F.focus[0][1], F.st_fdm, F.st_eye_passes);
        F.st_t = now, F.st_gaze = F.st_frames = F.st_fdm = F.st_eye_passes = 0;
    }
}
// ---------------------------------------------------------------- the maps
static uint16_t half(float v) {  // v in 1/16..1: a normal half float
    uint32_t b;
    memcpy(&b, &v, 4);
    return (uint16_t)(((b >> 16) & 0x8000) | ((((b >> 23) & 0xff) - 112) << 10) | ((b >> 13) & 0x3ff));
}

// The density (per axis) at NDC distance r from the focus. Rings: full inside r0, half in each axis to r1, then the
// level's edge. While the rings ramp in (the gaze just came back) they start 4 x wider.
static void density(float r, float out[2]) {
    static const float k[4][4] = {  // r0, r1, edge x, edge y
        {0, 0, 1, 1}, {0.60f, 0.95f, 0.5f, 0.5f}, {0.45f, 0.80f, 0.25f, 0.5f}, {0.35f, 0.65f, 0.25f, 0.25f}};
    const float *p = k[F.level];
    float s = 1.0f + 3.0f * (1.0f - F.ramp);
    if (F.uniform > 0) { out[0] = out[1] = fminf(F.uniform, 1.0f); return; }
    if (r < p[0] * s) out[0] = out[1] = 1.0f;
    else if (r < p[1] * s) out[0] = out[1] = 0.5f;
    else out[0] = p[2], out[1] = p[3];
    if (F.level == LV_LOW && r >= p[0] * s && r < p[1] * s) out[1] = 1.0f;  // (low: 2x1 in the ring)
}

static bool slot_make(FovMap *m, FovSlot *s) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = k_fmt,
                            .extent = {m->mw, m->mh, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT,
                            .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo inf;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &s->image, &s->mem, &inf) != VK_SUCCESS) return false;
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout lay;
    vkGetImageSubresourceLayout(g_vk.dev, s->image, &sub, &lay);
    s->ptr = (uint8_t *)inf.pMappedData + lay.offset, s->pitch = lay.rowPitch;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = k_fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &s->view) != VK_SUCCESS) return false;
    // PREINITIALIZED -> GENERAL (kept: the host writes it in GENERAL from then on; only the driver's CPU reads it)
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                               .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT,
                               .dstAccessMask = VK_ACCESS_2_FRAGMENT_DENSITY_MAP_READ_BIT_EXT,
                               .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = s->image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(vk_upload_cmd(), &di);
    s->key = ~0u;
    return true;
}

static void slot_fill(FovMap *m, FovSlot *s, float cx, float cy, float hw, float hh, float fw, float fh) {
    for (uint32_t j = 0; j < m->mh; j++) {
        uint16_t *row = (uint16_t *)(s->ptr + j * s->pitch);
        float py = (j + 0.5f) * FOV_TEXEL;
        for (uint32_t i = 0; i < m->mw; i++) {
            float px = (i + 0.5f) * FOV_TEXEL, d[2];
            float dx = (px - cx) / hw, dy = (py - cy) / hh;
            if (px > fw + FOV_TEXEL || py > fh + FOV_TEXEL) d[0] = d[1] = 0.25f;  // (outside the filled part)
            else density(sqrtf(dx * dx + dy * dy), d);
            row[i * 2] = half(d[0]), row[i * 2 + 1] = half(d[1]);
        }
    }
}

// VRS mode: the shading-rate map of eye `eye`'s w x h passes, brought up to date (one copy per submission at most)
static const void *vrs_pass(int eye, uint32_t w, uint32_t h, float cx, float cy, float fw, float fh, uint32_t key) {
    VrsMap *m = NULL;
    for (int i = 0; i < F.nvmaps && !m; i++)
        if (F.vmaps[i].eye == eye && F.vmaps[i].w == w && F.vmaps[i].h == h) m = &F.vmaps[i];
    if (!m) {
        if (F.nvmaps == FOV_MAPS) return NULL;
        m = &F.vmaps[F.nvmaps];
        *m = (VrsMap){.eye = eye, .w = w, .h = h, .mw = (w + VRS_TEXEL - 1) / VRS_TEXEL, .mh = (h + VRS_TEXEL - 1) / VRS_TEXEL, .key = ~0u};
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8_UINT,
                                .extent = {m->mw, m->mh, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO};
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &m->image, &m->mem, NULL) != VK_SUCCESS) { F.dev_ok = false; return NULL; }
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = VK_FORMAT_R8_UINT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(g_vk.dev, &vi, NULL, &m->view) != VK_SUCCESS) { F.dev_ok = false; return NULL; }
        F.nvmaps++;
        LOG("[fov] shading-rate map for eye %d's %ux%u passes (%ux%u texels)\n", eye, w, h, m->mw, m->mh);
    }
    uint64_t seq = vk_pending_seq();
    if (m->key != key && m->seq != seq) {
        VkSpan sp = vk_transient((uint64_t)m->mw * m->mh, 16);
        uint8_t *px = sp.ptr;
        for (uint32_t j = 0; j < m->mh; j++)
            for (uint32_t i = 0; i < m->mw; i++) {
                float x = (i + 0.5f) * VRS_TEXEL, y = (j + 0.5f) * VRS_TEXEL, d[2];
                float dx = (x - cx) / (fw * 0.5f), dy = (y - cy) / (fh * 0.5f);
                if (x > fw + VRS_TEXEL || y > fh + VRS_TEXEL) d[0] = d[1] = 0.25f;
                else density(sqrtf(dx * dx + dy * dy), d);
                int lw = d[0] >= 1 ? 0 : d[0] >= 0.5f ? 1 : 2, lh = d[1] >= 1 ? 0 : d[1] >= 0.5f ? 1 : 2;
                px[j * m->mw + i] = (uint8_t)(lw << 2 | lh);  // (log2 width << 2 | log2 height)
            }
        VkCommandBuffer cb = vk_upload_cmd();
        VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .srcAccessMask = 0, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                   .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                   .oldLayout = m->init ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = m->image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
        rec_CmdPipelineBarrier2(cb, &di);
        VkBufferImageCopy r = {.bufferOffset = sp.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {m->mw, m->mh, 1}};
        rec_CmdCopyBufferToImage(cb, sp.buf, m->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
        b.dstAccessMask = VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        rec_CmdPipelineBarrier2(cb, &di);
        m->init = true, m->key = key, m->seq = seq;
    }
    if (!m->init) return NULL;
    F.st_fdm++;
    F.fsr_info = (VkRenderingFragmentShadingRateAttachmentInfoKHR){
        .sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR, .imageView = m->view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .shadingRateAttachmentTexelSize = {VRS_TEXEL, VRS_TEXEL}};
    return &F.fsr_info;
}

const void *fov_pass(int eye, bool per_eye, uint32_t w, uint32_t h, float fx, float fy) {
    if (!F.dev_ok || eye < 0 || eye > 1) return NULL;
    F.st_eye_passes++;
    uint32_t ew = 0, eh = 0;
    bool size_ok = stereo_eye_size(&ew, &eh) && w >= ew * F.min_scale && h >= eh * F.min_scale;
    static int nlog = 400;
    if (F.log_passes && nlog > 0) {
        nlog--;
        LOG("[fov] eye %d pass %ux%u (filled %.2f x %.2f): %s\n", eye, w, h, fx, fy,
            !per_eye ? "shared / not scalable" : !size_ok ? "smaller than the eye" : "foveated");
    }
    if (!per_eye || !F.have_focus || !F.tracking || !size_ok) return NULL;
    // the focus in this pass's pixels (rows from the top of the image; the filled part is its top-left fx x fy)
    if (fx <= 0 || fx > 1) fx = 1;
    if (fy <= 0 || fy > 1) fy = 1;
    float fw = fx * w, fh = fy * h, nx = F.focus[eye][0], ny = F.focus[eye][1];
    float cx = (nx * 0.5f + 0.5f) * fw, cy = (renderer_origin_lower_left() ? ny * 0.5f + 0.5f : 0.5f - ny * 0.5f) * fh;
    uint32_t key = ((uint32_t)fminf(fmaxf(cx / 8 + 512, 0), 1023)) | ((uint32_t)fminf(fmaxf(cy / 8 + 512, 0), 1023) << 10) |
                   ((uint32_t)(fx * 31) << 20) | ((uint32_t)(fy * 15) << 25) | ((uint32_t)(F.ramp * 7) << 29);
    if (F.vrs) return vrs_pass(eye, w, h, cx, cy, fw, fh, key);
    FovMap *m = NULL;
    for (int i = 0; i < F.nmaps && !m; i++)
        if (F.maps[i].eye == eye && F.maps[i].w == w && F.maps[i].h == h) m = &F.maps[i];
    if (!m) {
        if (F.nmaps == FOV_MAPS) return NULL;
        m = &F.maps[F.nmaps++];
        *m = (FovMap){.eye = eye, .w = w, .h = h, .mw = (w + FOV_TEXEL - 1) / FOV_TEXEL, .mh = (h + FOV_TEXEL - 1) / FOV_TEXEL, .cur = -1};
        for (int i = 0; i < FOV_RING; i++)
            if (!slot_make(m, &m->ring[i])) { LOG("[fov] cannot make a density map: off\n"); F.dev_ok = false; return NULL; }
        LOG("[fov] density maps for eye %d's %ux%u passes (%ux%u texels)\n", eye, w, h, m->mw, m->mh);
    }
    uint64_t done = vk_completed_seq();
    if (m->cur < 0 || m->ring[m->cur].key != key) {
        int pick = -1;
        for (int i = 0; i < FOV_RING && pick < 0; i++)
            if (i != m->cur && m->ring[i].used_seq <= done) pick = i;
        if (pick >= 0) {
            slot_fill(m, &m->ring[pick], cx, cy, fw * 0.5f, fh * 0.5f, fw, fh);
            m->ring[pick].key = key;
            m->cur = pick;
        }
        if (m->cur < 0) return NULL;
    }
    FovSlot *s = &m->ring[m->cur];
    s->used_seq = vk_pending_seq();
    F.st_fdm++;
    F.info = (VkRenderingFragmentDensityMapAttachmentInfoEXT){.sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT,
                                                              .imageView = s->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    return &F.info;
}
