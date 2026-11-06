// The menu laser's look (GalaxyFrame's kBeamLayer / kReticleLayer, xr_app.cpp / vr_game.cpp uiLayer): a beam from the
// pointing controller and a dot where it meets the HUD panel, each a quad layer of its own over the HUD's (drawn into
// the HUD image, or before it, they were under the panel: GalaxyFrame's first bug). Both images are drawn once, on the
// CPU, when the session starts, and never change: each refresh only moves the two layers (xr_input.c xr_input_pointer
// locates the aim pose for that refresh's display time), so the laser costs the GPU nothing and never lags the hand.
// Shown only while the recomp menu (or its name entry) has the controls; with it closed, no layers and no work.
//
// The look: a soft Link-tunic green beam, a paler core, fading from the hand to the panel; a warm cream-gold dot with
// a thin dark outline, so it reads over the parchment and over the game. It swells for a moment when it reaches a new
// control (with the haptic tick).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../rt.h"
#include "xr_pointer.h"

#define BEAM_W 32
#define BEAM_H 512
#define DOT_S 64

static struct {
    bool ready;
    XrChain beam, dot;
    XrPointerPose pose;  // (XR thread)
} P;

// ---- the images (premultiplied, linear colour encoded to sRGB: the swapchains are sRGB, the compositor decodes them)
static float smooth(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}
static float lin(float s) { return s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f); }
static uint8_t enc(float l) {
    l = l < 0 ? 0 : l > 1 ? 1 : l;
    float s = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1 / 2.4f) - 0.055f;
    return (uint8_t)(s * 255 + 0.5f);
}
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
// one pixel: sRGB colour c (0..1), coverage a, into the swapchain's byte order
static void put(uint8_t *px, bool bgra, const float c[3], float a) {
    uint8_t r = enc(lin(c[0]) * a), g = enc(lin(c[1]) * a), b = enc(lin(c[2]) * a), al = (uint8_t)(a * 255 + 0.5f);
    px[0] = bgra ? b : r, px[1] = g, px[2] = bgra ? r : b, px[3] = al;
}

// The beam: x across, rows from the far end (row 0, the top of the layer: +y, the panel) to the hand. Narrower at the
// hand (45 % of the far end's width, as GalaxyFrame's taper at about 2 m), a pale core in tunic green, 0.85 opaque at the
// hand fading to 0.15 at the panel.
static void draw_beam(uint8_t *img, bool bgra) {
    static const float edge[3] = {0.27f, 0.72f, 0.29f}, core[3] = {0.80f, 0.97f, 0.72f};
    for (int y = 0; y < BEAM_H; y++) {
        float along = 1 - (y + 0.5f) / BEAM_H;  // 0 at the hand, 1 at the panel
        float half = mixf(0.45f, 1.0f, along);
        float fade = mixf(0.85f, 0.15f, along);
        for (int x = 0; x < BEAM_W; x++) {
            float across = fabsf((x + 0.5f) / BEAM_W * 2 - 1) / half;  // 0 centre .. 1 edge
            float prof = across >= 1 ? 0 : (1 - across) * (1 - across) * (3 - 2 * (1 - across)) * 0.6f + (1 - across) * 0.4f;
            float k = powf(across < 1 ? 1 - across : 0, 2.5f);
            float c[3] = {mixf(edge[0], core[0], k), mixf(edge[1], core[1], k), mixf(edge[2], core[2], k)};
            put(img + 4 * (y * BEAM_W + x), bgra, c, prof * fade);
        }
    }
}

// The dot: cream at the centre to gold at the rim, a thin dark outline (r 0.80 .. 0.96 of the image's half size).
static void draw_dot(uint8_t *img, bool bgra) {
    static const float cream[3] = {1.0f, 0.96f, 0.82f}, gold[3] = {0.96f, 0.76f, 0.30f}, dark[3] = {0.16f, 0.11f, 0.05f};
    const float px = 2.0f / DOT_S;  // one pixel, in the -1..1 square
    for (int y = 0; y < DOT_S; y++)
        for (int x = 0; x < DOT_S; x++) {
            float u = (x + 0.5f) / DOT_S * 2 - 1, v = (y + 0.5f) / DOT_S * 2 - 1, r = sqrtf(u * u + v * v);
            float fill = 1 - smooth(0.80f - px, 0.80f + px, r);
            float outline = (1 - smooth(0.96f - px, 0.96f + px, r)) * 0.85f;
            float t = smooth(0.15f, 0.80f, r);
            float c[3] = {mixf(cream[0], gold[0], t), mixf(cream[1], gold[1], t), mixf(cream[2], gold[2], t)};
            // the fill over the outline (premultiplied "over", in sRGB-coded colour: a 2-pixel blend, close enough)
            float a = fill + outline * (1 - fill);
            float o[3];
            for (int i = 0; i < 3; i++) o[i] = a > 0 ? (c[i] * fill + dark[i] * outline * (1 - fill)) / a : 0;
            put(img + 4 * (y * DOT_S + x), bgra, o, a);
        }
}

// One image of chain c from the CPU: acquired, copied in, left in COLOR_ATTACHMENT_OPTIMAL and released (the layer then
// shows that image for good: nothing acquires the chain again).
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_xr.vphys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static bool upload(XrChain *c, const uint8_t *rgba) {
    VkDevice d = g_xr.vdev;
    VkDeviceSize size = (VkDeviceSize)c->w * c->h * 4;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool ok = false, acquired = false;
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateBuffer(d, &bc, NULL, &buf) != VK_SUCCESS) goto out;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d, buf, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d, &ai, NULL, &mem) != VK_SUCCESS) goto out;
    vkBindBufferMemory(d, buf, mem, 0);
    void *map;
    if (vkMapMemory(d, mem, 0, size, 0, &map) != VK_SUCCESS) goto out;
    memcpy(map, rgba, (size_t)size);
    vkUnmapMemory(d, mem);
    VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                  .queueFamilyIndex = g_xr.qfam};
    if (vkCreateCommandPool(d, &pc, NULL, &pool) != VK_SUCCESS) goto out;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(d, &cai, &cb) != VK_SUCCESS) goto out;
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(d, &fc, NULL, &fence) != VK_SUCCESS) goto out;
    uint32_t idx;
    if (!xr_chain_acquire(c, &idx)) goto out;
    acquired = true;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = c->img[idx], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {c->w, c->h, 1}};
    vkCmdCopyBufferToImage(cb, buf, c->img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    vk_queue_lock();
    VkResult r = vkQueueSubmit(g_xr.queue, 1, &si, fence);
    vk_queue_unlock();
    ok = r == VK_SUCCESS && vkWaitForFences(d, 1, &fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
out:
    if (acquired) xr_chain_release(c);  // (released even after a failure: no chain is left with an image acquired)
    if (fence) vkDestroyFence(d, fence, NULL);
    if (pool) vkDestroyCommandPool(d, pool, NULL);
    if (buf) vkDestroyBuffer(d, buf, NULL);
    if (mem) vkFreeMemory(d, mem, NULL);
    return ok;
}

bool xr_pointer_create(void) {
    if (getenv("LARECOMP_XR_LASER") && !atoi(getenv("LARECOMP_XR_LASER"))) {
        LOG("[xr] menu laser: off (LARECOMP_XR_LASER=0; the controller still clicks)\n");
        return false;
    }
    uint64_t t0 = plat_now_ns();
    if (!xr_chain_create(&P.beam, "laser beam", BEAM_W, BEAM_H) || !xr_chain_create(&P.dot, "laser dot", DOT_S, DOT_S)) goto fail;
    bool bgra = P.beam.fmt == VK_FORMAT_B8G8R8A8_SRGB;
    uint8_t *beam = malloc(BEAM_W * BEAM_H * 4), *dot = malloc(DOT_S * DOT_S * 4);
    draw_beam(beam, bgra), draw_dot(dot, bgra);
    bool ok = upload(&P.beam, beam) && upload(&P.dot, dot);
    free(beam), free(dot);
    if (!ok) goto fail;
    P.ready = true;
    LOG("[xr] menu laser: beam %dx%d and dot %dx%d drawn once (%.1f ms)\n", BEAM_W, BEAM_H, DOT_S, DOT_S, (plat_now_ns() - t0) / 1e6);
    return true;
fail:
    LOG("[xr] menu laser: its images could not be made: no beam or dot (the controller still clicks)\n");
    return false;
}

void xr_pointer_destroy(void) {
    XrChain *cs[] = {&P.beam, &P.dot};
    for (int k = 0; k < 2; k++) {
        XrChain *c = cs[k];
        for (uint32_t i = 0; i < c->count; i++) if (c->view[i]) vkDestroyImageView(g_xr.vdev, c->view[i], NULL);
        if (c->sc) { vk_queue_lock(); XR_RT(p_xrDestroySwapchain(c->sc)); vk_queue_unlock(); }
        memset(c, 0, sizeof *c);
    }
    P.ready = false;
}

void xr_pointer_set(const XrPointerPose *p) { P.pose = *p; }

// ---- the layers (xr_loop.c end_frame, after the HUD's)
typedef struct { float x, y, z; } V3;
static V3 v3(const float *a) { return (V3){a[0], a[1], a[2]}; }
static V3 sub(V3 a, V3 b) { return (V3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 add(V3 a, V3 b) { return (V3){a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 mul(V3 a, float k) { return (V3){a.x * k, a.y * k, a.z * k}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return (V3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static V3 norm(V3 a) {
    float l = sqrtf(dot(a, a));
    return l > 1e-6f ? mul(a, 1 / l) : (V3){0, 0, 1};
}
// the rotation whose axes are x, y, z (orthonormal, right-handed)
static XrQuaternionf basis_quat(V3 x, V3 y, V3 z) {
    float m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22;
    XrQuaternionf q;
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q = (XrQuaternionf){(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1 + m00 - m11 - m22) * 2;
        q = (XrQuaternionf){0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (m11 > m22) {
        float s = sqrtf(1 + m11 - m00 - m22) * 2;
        q = (XrQuaternionf){(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        float s = sqrtf(1 + m22 - m00 - m11) * 2;
        q = (XrQuaternionf){(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q.x /= l, q.y /= l, q.z /= l, q.w /= l;
    return q;
}

uint32_t xr_pointer_layers(XrCompositionLayerQuad out[2]) {
    const XrPointerPose *p = &P.pose;
    if (!P.ready || !p->shown || !(p->length > 0.01f)) return 0;
    V3 o = v3(p->origin), d = norm(v3(p->dir)), head = v3(p->head);
    V3 end = add(o, mul(d, p->length));
    XrCompositionLayerQuad base = {.type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = g_xr.app,
                                   .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, .eyeVisibility = XR_EYE_VISIBILITY_BOTH};
    // the beam: y along the ray, its face turned to the head (GalaxyFrame: 2.4 mm wide at the hand, +1.6 mm a metre)
    float far_w = 2 * (0.0012f + 0.0008f * p->length);
    V3 mid = mul(add(o, end), 0.5f);
    V3 x = norm(cross(d, norm(sub(head, mid))));
    out[0] = base;
    out[0].subImage = (XrSwapchainSubImage){P.beam.sc, {{0, 0}, {BEAM_W, BEAM_H}}, 0};
    out[0].pose = (XrPosef){basis_quat(x, d, cross(x, d)), {mid.x, mid.y, mid.z}};
    out[0].size = (XrExtent2Df){far_w, p->length};
    // the dot, facing the head, sized to stay readable with distance; it swells with p->swell (0..1)
    float r = (0.007f + 0.004f * p->length) * (1 + 0.5f * p->swell);
    V3 n = norm(sub(head, end));
    V3 u = norm(cross(fabsf(n.y) < 0.9f ? (V3){0, 1, 0} : (V3){1, 0, 0}, n));
    out[1] = base;
    out[1].subImage = (XrSwapchainSubImage){P.dot.sc, {{0, 0}, {DOT_S, DOT_S}}, 0};
    out[1].pose = (XrPosef){basis_quat(u, cross(n, u), n), {end.x, end.y, end.z}};
    out[1].size = (XrExtent2Df){2 * r, 2 * r};
    return p->dot ? 2 : 1;
}
