// XR_KHR_vulkan_enable2 (the instance and device made by the runtime from LAS's own create infos) and the swapchains
// (from GalaxyFrame's xr_gfx_vk.cpp attach / createChain).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../rt.h"
#include "xr_internal.h"
#include "xr_input.h"
#include "xr_pointer.h"

// The Vulkan entry points the runtime gets (pfnGetInstanceProcAddr): its queue calls take the queue lock. SteamVR
// submits to the app's queue from inside xrWaitFrame too (validation: THREADING ERROR with the recorder's
// vkQueueSubmit2), which the OpenXR spec does not list among the calls that need the queue synchronised, and holding
// the lock over xrWaitFrame's wait would stall the recorder for most of each refresh. So the runtime's own queue calls
// are locked one by one, whatever OpenXR call they come from. (One device: the real entry points dispatch by handle.)
static PFN_vkQueueSubmit r_QueueSubmit;
static PFN_vkQueueSubmit2 r_QueueSubmit2;
static PFN_vkQueueWaitIdle r_QueueWaitIdle;
static PFN_vkQueueBindSparse r_QueueBindSparse;
static PFN_vkQueuePresentKHR r_QueuePresentKHR;
static PFN_vkDeviceWaitIdle r_DeviceWaitIdle;
static PFN_vkGetDeviceProcAddr r_GetDeviceProcAddr;
static VKAPI_ATTR VkResult VKAPI_CALL l_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    vk_queue_lock(); VkResult r = r_QueueSubmit(q, n, s, f); vk_queue_unlock(); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL l_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f) {
    vk_queue_lock(); VkResult r = r_QueueSubmit2(q, n, s, f); vk_queue_unlock(); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL l_QueueWaitIdle(VkQueue q) {
    vk_queue_lock(); VkResult r = r_QueueWaitIdle(q); vk_queue_unlock(); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL l_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo *b, VkFence f) {
    vk_queue_lock(); VkResult r = r_QueueBindSparse(q, n, b, f); vk_queue_unlock(); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL l_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *p) {
    vk_queue_lock(); VkResult r = r_QueuePresentKHR(q, p); vk_queue_unlock(); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL l_DeviceWaitIdle(VkDevice d) {
    vk_queue_lock(); VkResult r = r_DeviceWaitIdle(d); vk_queue_unlock(); return r;
}
// name's locked stand-in (real: its real entry point, stored), or real itself
static PFN_vkVoidFunction locked(const char *name, PFN_vkVoidFunction real) {
    if (!real) return NULL;
#define L(fn, ...) if (!strcmp(name, "vk" #fn) __VA_ARGS__) { r_##fn = (PFN_vk##fn)real; return (PFN_vkVoidFunction)l_##fn; }
    L(QueueSubmit) L(QueueSubmit2) L(QueueSubmit2, || !strcmp(name, "vkQueueSubmit2KHR")) L(QueueWaitIdle) L(QueueBindSparse)
    L(QueuePresentKHR) L(DeviceWaitIdle)
#undef L
    return real;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL l_GetDeviceProcAddr(VkDevice d, const char *name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)l_GetDeviceProcAddr;
    return locked(name, r_GetDeviceProcAddr(d, name));
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL l_GetInstanceProcAddr(VkInstance i, const char *name) {
    PFN_vkVoidFunction f = vkGetInstanceProcAddr(i, name);
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)l_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        if (f) r_GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)f;
        return f ? (PFN_vkVoidFunction)l_GetDeviceProcAddr : NULL;
    }
    return locked(name, f);
}

VkResult xr_vk_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out) {
    if (!xr_instance_create()) {
        LOG("[xr] is SteamVR (or another OpenXR runtime) running, with the headset connected? --flat starts the game in its window\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    XrGraphicsRequirementsVulkan2KHR req = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!XR_OK(p_xrGetVulkanGraphicsRequirements2KHR(g_xr.inst, g_xr.sys, &req))) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t api = ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    // (SteamVR says 1.0 to 1.2 but makes a 1.3 instance and device with LAS's features: tools/xrvkprobe)
    LOG("[xr] the runtime takes Vulkan %u.%u to %u.%u; asking for %u.%u\n", (unsigned)XR_VERSION_MAJOR(req.minApiVersionSupported),
        (unsigned)XR_VERSION_MINOR(req.minApiVersionSupported), (unsigned)XR_VERSION_MAJOR(req.maxApiVersionSupported),
        (unsigned)XR_VERSION_MINOR(req.maxApiVersionSupported), VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));
    XrVulkanInstanceCreateInfoKHR xi = {.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = g_xr.sys,
                                        .pfnGetInstanceProcAddr = l_GetInstanceProcAddr, .vulkanCreateInfo = ci};
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if (!XR_OK(p_xrCreateVulkanInstanceKHR(g_xr.inst, &xi, out, &vr))) return VK_ERROR_INITIALIZATION_FAILED;
    if (vr == VK_SUCCESS) g_xr.vinst = *out;
    return vr;
}

VkPhysicalDevice xr_vk_physical_device(VkInstance inst) {
    XrVulkanGraphicsDeviceGetInfoKHR gi = {.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = g_xr.sys, .vulkanInstance = inst};
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    if (!XR_OK(p_xrGetVulkanGraphicsDevice2KHR(g_xr.inst, &gi, &phys))) return VK_NULL_HANDLE;
    g_xr.vphys = phys;
    return phys;
}

VkResult xr_vk_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, VkDevice *out) {
    XrVulkanDeviceCreateInfoKHR xi = {.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = g_xr.sys,
                                      .pfnGetInstanceProcAddr = l_GetInstanceProcAddr, .vulkanPhysicalDevice = phys,
                                      .vulkanCreateInfo = ci};
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if (!XR_OK(p_xrCreateVulkanDeviceKHR(g_xr.inst, &xi, out, &vr))) return VK_ERROR_INITIALIZATION_FAILED;
    if (vr == VK_SUCCESS) g_xr.vdev = *out;
    return vr;
}

// Everything on the one queue finished (before xrEndSession / xrDestroySession: GalaxyFrame's exit GPU faults). The
// queue lock keeps the recorder out for the wait.
void xr_drain_gpu(const char *before) {
    if (!g_xr.queue) return;
    uint64_t t0 = plat_now_ns();
    vk_queue_lock();
    VkResult r = vkQueueWaitIdle(g_xr.queue);
    vk_queue_unlock();
    LOG("[xr] GPU idle before %s (%d, %.1f ms)\n", before, r, (plat_now_ns() - t0) / 1e6);
}

// ---- swapchains: sRGB, MUTABLE_FORMAT, viewed as UNORM (see xr.h); one mip level (SteamVR allocates level 0 only)
static int64_t chain_fmt;
static VkFormat chain_view_fmt;

static bool pick_format(void) {
    uint32_t n = 0;
    if (!XR_OK(p_xrEnumerateSwapchainFormats(g_xr.session, 0, &n, NULL)) || !n) return false;
    int64_t *f = calloc(n, sizeof *f);
    p_xrEnumerateSwapchainFormats(g_xr.session, n, &n, f);
    char list[256] = "";
    bool bgra = false, rgba = false;
    for (uint32_t i = 0; i < n; i++) {
        snprintf(list + strlen(list), sizeof list - strlen(list), " %lld", (long long)f[i]);
        bgra |= f[i] == VK_FORMAT_B8G8R8A8_SRGB;
        rgba |= f[i] == VK_FORMAT_R8G8B8A8_SRGB;
    }
    free(f);
    // B8G8R8A8 first: the flat window's own (B8G8R8A8_UNORM), so every pipeline made for the window's format fits
    if (bgra) chain_fmt = VK_FORMAT_B8G8R8A8_SRGB, chain_view_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    else if (rgba) chain_fmt = VK_FORMAT_R8G8B8A8_SRGB, chain_view_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    LOG("[xr] swapchain formats (VkFormat):%s; using %lld viewed as %d\n", list, (long long)chain_fmt, (int)chain_view_fmt);
    return bgra || rgba;
}

bool xr_chain_create(XrChain *c, const char *what, uint32_t w, uint32_t h) {
    VkFormat vf[2] = {(VkFormat)chain_fmt, chain_view_fmt};
    XrVulkanSwapchainFormatListCreateInfoKHR fl = {.type = XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR,
                                                   .viewFormatCount = 2, .viewFormats = vf};
    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .next = g_xr.has_format_list ? &fl : NULL,
                                .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                                              XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                                              XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT,
                                .format = chain_fmt, .sampleCount = 1, .width = w, .height = h, .faceCount = 1,
                                .arraySize = 1, .mipCount = 1};
    vk_queue_lock();  // (a runtime may use the queue to set the images up: the mock does)
    XrResult cr = XR_RT(p_xrCreateSwapchain(g_xr.session, &ci, &c->sc));
    vk_queue_unlock();
    if (!xr_ok_(cr, "xrCreateSwapchain")) return false;
    c->fmt = chain_fmt, c->view_fmt = chain_view_fmt, c->w = w, c->h = h;
    XrSwapchainImageVulkan2KHR im[8];
    for (int i = 0; i < 8; i++) im[i] = (XrSwapchainImageVulkan2KHR){.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
    uint32_t n = 0;
    if (!XR_OK(p_xrEnumerateSwapchainImages(c->sc, 0, &n, NULL))) return false;
    if (n > 8) { LOG("[xr] %s swapchain has %u images (8 at most)\n", what, n); return false; }
    if (!XR_OK(p_xrEnumerateSwapchainImages(c->sc, n, &n, (XrSwapchainImageBaseHeader *)im))) return false;
    c->count = n;
    for (uint32_t i = 0; i < n; i++) {
        c->img[i] = im[i].image;
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = c->img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = c->view_fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(g_xr.vdev, &vi, NULL, &c->view[i]) != VK_SUCCESS) { LOG("[xr] %s image view failed\n", what); return false; }
    }
    LOG("[xr] %s swapchain %ux%u, %u images\n", what, w, h, n);
    return true;
}

bool xr_chains_create(uint32_t hud_w, uint32_t hud_h) {
    if (!pick_format()) { LOG("[xr] the runtime offers no 8-bit sRGB swapchain format\n"); return false; }
    uint32_t nv = 0;
    for (int i = 0; i < 2; i++) g_xr.vcv[i] = (XrViewConfigurationView){.type = XR_TYPE_VIEW_CONFIGURATION_VIEW};
    if (!XR_OK(p_xrEnumerateViewConfigurationViews(g_xr.inst, g_xr.sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, g_xr.vcv)) || nv != 2)
        return false;
    const char *e = getenv("LARECOMP_XR_EYE_SCALE");
    float scale = e ? (float)atof(e) : 1.0f;
    if (!(scale > 0.1f && scale <= 2.0f)) scale = 1.0f;
    for (int i = 0; i < 2; i++) {
        uint32_t w = (uint32_t)(g_xr.vcv[i].recommendedImageRectWidth * scale + 0.5f);
        uint32_t h = (uint32_t)(g_xr.vcv[i].recommendedImageRectHeight * scale + 0.5f);
        if (w > g_xr.vcv[i].maxImageRectWidth) w = g_xr.vcv[i].maxImageRectWidth;
        if (h > g_xr.vcv[i].maxImageRectHeight) h = g_xr.vcv[i].maxImageRectHeight;
        if (i == 0) LOG("[xr] eyes: recommended %ux%u (max %ux%u), x%.2f\n", g_xr.vcv[i].recommendedImageRectWidth,
                        g_xr.vcv[i].recommendedImageRectHeight, g_xr.vcv[i].maxImageRectWidth, g_xr.vcv[i].maxImageRectHeight, scale);
        for (int p = 0; p < 2; p++)
            if (!xr_chain_create(&g_xr.eyes[p][i], i ? "right eye" : "left eye", w, h)) return false;
    }
    for (int p = 0; p < 2; p++) if (!xr_chain_create(&g_xr.hud[p], "HUD", hud_w, hud_h)) return false;
    if (g_xr.hud[0].count + g_xr.hud[1].count > 8) { LOG("[xr] HUD swapchains with more than 4 images each\n"); return false; }
    xr_pointer_create();  // (the menu laser's two images; without them the controller still clicks)
    return true;
}

void xr_chains_destroy(void) {
    xr_pointer_destroy();
    XrChain *cs[6 + 4 * XR_RESHOW_MAX] = {&g_xr.hud[0], &g_xr.hud[1], &g_xr.eyes[0][0], &g_xr.eyes[0][1], &g_xr.eyes[1][0], &g_xr.eyes[1][1]};
    for (int i = 0; i < 4 * XR_RESHOW_MAX; i++) cs[6 + i] = &g_xr.rs[i / (2 * XR_RESHOW_MAX)][(i / 2) % XR_RESHOW_MAX][i % 2];
    for (int k = 0; k < 6 + 4 * XR_RESHOW_MAX; k++) {
        XrChain *c = cs[k];
        for (uint32_t i = 0; i < c->count; i++) if (c->view[i]) vkDestroyImageView(g_xr.vdev, c->view[i], NULL);
        if (c->sc) { vk_queue_lock(); XR_RT(p_xrDestroySwapchain(c->sc)); vk_queue_unlock(); }
        memset(c, 0, sizeof *c);
    }
}

// Acquire (under the queue lock: the runtime may use the queue in it) and wait for the next image.
bool xr_chain_acquire(XrChain *c, uint32_t *index) {
    XrSwapchainImageAcquireInfo ai = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    vk_queue_lock();
    XrResult r = XR_RT(p_xrAcquireSwapchainImage(c->sc, &ai, index));
    vk_queue_unlock();
    if (!xr_ok_(r, "xrAcquireSwapchainImage")) return false;
    XrSwapchainImageWaitInfo wi = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 100000000};
    for (int tries = 0;; tries++) {
        r = XR_RT(p_xrWaitSwapchainImage(c->sc, &wi));
        if (r != XR_TIMEOUT_EXPIRED) break;
        if (tries == 10) LOG("[xr] still waiting for a swapchain image after 1 s\n");
    }
    return xr_ok_(r, "xrWaitSwapchainImage");
}

void xr_chain_release(XrChain *c) {
    XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    vk_queue_lock();
    XrResult r = XR_RT(p_xrReleaseSwapchainImage(c->sc, &ri));
    vk_queue_unlock();
    xr_ok_(r, "xrReleaseSwapchainImage");
}

// ---- eyes (xr.h)
bool xr_eye_target(int eye, XrEyeTarget *out) {
    if (!g_xr.in_set || eye < 0 || eye > 1) return false;
    XrSet *s = &g_xr.building;
    if (!s->has_eye[eye]) {
        if (!xr_chain_acquire(&g_xr.eyes[s->pair][eye], &s->eye[eye])) return false;
        s->has_eye[eye] = true;
    }
    const XrChain *c = &g_xr.eyes[s->pair][eye];
    *out = (XrEyeTarget){c->img[s->eye[eye]], c->view[s->eye[eye]], c->view_fmt, {c->w, c->h}};
    return true;
}

bool xr_eye_used(int eye) { return g_xr.in_set && eye >= 0 && eye < 2 && g_xr.building.has_eye[eye]; }

// The display time to locate a frame's views for. Measured (default): now plus the measured time from a latch to the
// first display of its set (the eyes are shown then, so the poses they were rendered with are the head's then, and the
// compositor's reprojection has least to correct); until it is measured, or LARECOMP_XR_PREDICT=vsync: the latest
// vsync's display time (two refreshes after it). *ref: that vsync's time (the grid line the re-shown refreshes' lead
// counts from).
static XrTime predict_time(XrTime *ref) {
    int64_t t = atomic_load(&g_xr.target_time), lat = atomic_load(&g_xr.lat_ns);
    *ref = t;
    if (!t || !g_xr.predict || !lat) return t;
    int64_t now = (int64_t)plat_now_ns() + atomic_load(&g_xr.xr_minus_plat);
    int64_t period = g_xr.display_hz > 0 ? (int64_t)(1e9f / g_xr.display_hz) : 11111111;
    int64_t p = now + lat;
    if (p < now + period) p = now + period;
    if (p > now + 150000000ll) p = now + 150000000ll;
    // (LARECOMP_XR_REPROJ rot / pos / follow) on the display's grid: the set is shown at a refresh, not between two, and
    // its re-shown refreshes follow it a whole period apart (xr_reshow_times). Off the grid the set's first refresh was
    // up to half a period from the pose it was rendered for: 0.24 degrees at a 30 degrees a second turn, 120 Hz. The
    // refresh is the vsync's target plus the lead: the latest first refresh the last sets could have had, counted from
    // theirs (xr_release_completed). Not the measured latency rounded: a set is shown at one of two refreshes, by
    // whether its GPU work finishes just before one of them, and the latency's mean lies between the two. A set finished
    // before its refresh waits for it there.
    int64_t g = atomic_load(&g_xr.grid_t), gp = atomic_load(&g_xr.grid_period);
    if (g && gp > 0 && xr_reproj_mode() >= XR_REPROJ_ROT) {
        int lead = atomic_load(&g_xr.lead);
        if (lead != XR_LEAD_NONE) {
            p = t + lead * gp;
            while (p < now + gp / 2) p += gp;
            return p;
        }
        int64_t d = p - g;
        p = g + (d >= 0 ? (d + gp / 2) / gp : -((-d + gp / 2) / gp)) * gp;
        if (p < now) p += gp;
    }
    return p;
}

// One xrLocateViews for both eyes at display time t in `space` (0: the app space now, under the lock: the XR thread
// changes it), *space_out the one used.
static bool locate_views_in(XrTime t, XrSpace space, XrView views[2], XrSpace *space_out) {
    if (!t || !g_xr.session) return false;
    pthread_mutex_lock(&g_xr.mu);
    if (!space) space = g_xr.app;
    XrViewLocateInfo li = {.type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                           .displayTime = t, .space = space};
    XrViewState vs = {.type = XR_TYPE_VIEW_STATE};
    views[0] = views[1] = (XrView){.type = XR_TYPE_VIEW};
    uint32_t n = 0;
    XrResult r = p_xrLocateViews(g_xr.session, &li, &vs, 2, &n, views);
    pthread_mutex_unlock(&g_xr.mu);
    const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (XR_FAILED(r) || n != 2 || (vs.viewStateFlags & need) != need) return false;
    if (space_out) *space_out = space;
    return true;
}
bool xr_locate_in(XrTime t, XrSpace space, XrView views[2]) { return space && locate_views_in(t, space, views, NULL); }

// One xrLocateViews for both eyes at display time t, in the app space.
static bool locate_views(XrTime t, XrView views[2], XrSpace *space_out) {
    XrSpace space;
    if (!locate_views_in(t, 0, views, &space)) return false;
    static bool fov_logged;  // (once: each eye's fov and how far the eyes' views turn apart, canted displays)
    if (!fov_logged) {
        fov_logged = true;
        const XrQuaternionf *a = &views[0].pose.orientation, *b = &views[1].pose.orientation;
        float d = fabsf(a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w);
        LOG("[xr] eye fov (degrees left right up down): left %.1f %.1f %.1f %.1f, right %.1f %.1f %.1f %.1f; views %.1f degrees apart\n",
            views[0].fov.angleLeft * 57.29578f, views[0].fov.angleRight * 57.29578f, views[0].fov.angleUp * 57.29578f,
            views[0].fov.angleDown * 57.29578f, views[1].fov.angleLeft * 57.29578f, views[1].fov.angleRight * 57.29578f,
            views[1].fov.angleUp * 57.29578f, views[1].fov.angleDown * 57.29578f, 2.0f * acosf(fminf(d, 1.0f)) * 57.29578f);
    }
    *space_out = space;
    return true;
}

static void views_out(const XrView v2[2], XrEyeView out[2]) {
    for (int i = 0; out && i < 2; i++) {
        const XrView *v = &v2[i];
        out[i] = (XrEyeView){{v->pose.orientation.x, v->pose.orientation.y, v->pose.orientation.z, v->pose.orientation.w},
                             {v->pose.position.x, v->pose.position.y, v->pose.position.z},
                             {v->fov.angleLeft, v->fov.angleRight, v->fov.angleUp, v->fov.angleDown}};
    }
}

bool xr_latch_views(XrEyeView out[2]) {
    if (!g_xr.in_set) return false;
    XrSet *s = &g_xr.building;
    if (!s->views_ok) {
        XrTime ref, t = predict_time(&ref);
        if (!locate_views(t, s->views, &s->space)) return false;
        s->views_ok = true, s->vtime = t, s->vref = ref, s->latch_ns = plat_now_ns();
    }
    views_out(s->views, out);
    return true;
}

bool xr_latch_views_id(XrEyeView out[2], uint64_t *id) {
    if (!xr_active()) return false;
    XrView v[2];
    XrSpace space;
    XrTime ref, t = predict_time(&ref);
    if (!locate_views(t, v, &space)) return false;
    uint64_t n = ++g_xr.latch_next;
    XrLatch *l = &g_xr.latch[n % XR_LATCHES];
    pthread_mutex_lock(&g_xr.mu);  // (the XR thread reads a set's latch time; the ring itself is this thread's)
    *l = (XrLatch){n, {v[0], v[1]}, space, t, plat_now_ns(), ref};
    pthread_mutex_unlock(&g_xr.mu);
    views_out(v, out);
    if (id) *id = n;
    return true;
}

bool xr_latch_gaze(uint64_t id, float dir[3]) {
    const XrLatch *l = &g_xr.latch[id % XR_LATCHES];
    return id && l->id == id && xr_input_gaze(l->space, l->time, dir);
}

static int pose_differs(const XrView *v, const XrEyeView *e) {
    XrEyeView c[2];
    XrView two[2] = {*v, *v};
    views_out(two, c);
    return memcmp(c[0].orientation, e->orientation, sizeof e->orientation) || memcmp(c[0].position, e->position, sizeof e->position) ||
           memcmp(c[0].fov, e->fov, sizeof e->fov);
}

int xr_set_views(const uint64_t id[2], const XrEyeView rendered[2]) {
    if (!g_xr.in_set) return -1;
    XrSet *s = &g_xr.building;
    s->exec_eyes = true;
    const XrLatch *l[2];
    XrLatch fb;  // (gone from the ring - frames with camera draws but no composite, a load - : the poses as rendered, in
                 // the app space now; right unless the view was recentred since)
    for (int i = 0; i < 2; i++) {
        l[i] = &g_xr.latch[id[i] % XR_LATCHES];
        if (!id[i]) return -1;
        if (l[i]->id != id[i]) {
            fb = (XrLatch){id[i], {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}}, g_xr.app, 0, 0};
            for (int e = 0; e < 2; e++) {
                const XrEyeView *r = &rendered[e];
                fb.views[e].pose = (XrPosef){{r->orientation[0], r->orientation[1], r->orientation[2], r->orientation[3]},
                                             {r->position[0], r->position[1], r->position[2]}};
                fb.views[e].fov = (XrFovf){r->fov[0], r->fov[1], r->fov[2], r->fov[3]};
            }
            l[i] = &fb;
        }
    }
    s->views[0] = l[0]->views[0], s->views[1] = l[1]->views[1];
    s->space = l[0]->space;
    s->vtime = l[0]->time, s->vref = l[0]->ref, s->latch_ns = l[0]->made_ns;
    s->views_ok = true;
    s->rendered = true;
    memcpy(s->rview, rendered, sizeof s->rview);
    s->rid[0] = id[0], s->rid[1] = id[1];
    int bad = 0;
    for (int i = 0; i < 2; i++) bad |= pose_differs(&s->views[i], &rendered[i]) << i;
    return (id[0] != id[1] || l[0]->space != l[1]->space ? 4 : 0) | bad;
}

bool xr_layer_matches_render(const XrSet *s) {
    return s->rendered && !pose_differs(&s->views[0], &s->rview[0]) && !pose_differs(&s->views[1], &s->rview[1]) && s->rid[0] == s->rid[1];
}

int64_t xr_latch_time(uint64_t id) {
    pthread_mutex_lock(&g_xr.mu);
    const XrLatch *l = &g_xr.latch[id % XR_LATCHES];
    int64_t t = id && l->id == id ? l->time : 0;
    pthread_mutex_unlock(&g_xr.mu);
    return t;
}

// ---- the re-shown refreshes' images (xr.h; LARECOMP_XR_REPROJ)
// The set's first refresh is the display's refresh nearest the time its views were located for (the grid is the XR
// thread's latest predicted display time plus whole periods), the others follow one a period. The XR thread picks the
// image for each refresh by its display time (xr_loop.c end_frame), so a set first shown late, or shown longer because
// the next one is late, still gets the image nearest its time.
int xr_reshow_times(int64_t times[4]) {
    if (!g_xr.in_set || xr_reproj_mode() < XR_REPROJ_ROT) return 0;
    XrSet *s = &g_xr.building;
    int64_t g = atomic_load(&g_xr.grid_t), per = atomic_load(&g_xr.grid_period);
    if (!s->views_ok || !s->vtime || !g || per <= 0) return 0;
    int64_t d = s->vtime - g;
    int64_t n = d >= 0 ? (d + per / 2) / per : -((-d + per / 2) / per);
    s->grid0 = g + n * per, s->period = per;
    int k = xr_refreshes_per_frame();
    if (k > XR_RESHOW_MAX + 1) k = XR_RESHOW_MAX + 1;
    if (k < 1) k = 1;
    for (int i = 0; i < k; i++) times[i] = s->grid0 + i * per;
    return k;
}

bool xr_reshow_views(int k, XrEyeView out[2]) {
    if (!g_xr.in_set || k < 1 || k > XR_RESHOW_MAX) return false;
    XrSet *s = &g_xr.building;
    XrView *v = s->rs_views[k - 1];
    if (!s->grid0 || !xr_locate_in(s->grid0 + k * s->period, s->space, v)) return false;
    views_out(v, out);
    return true;
}

bool xr_reshow_target(int k, int eye, XrEyeTarget *out) {
    if (!g_xr.in_set || k < 1 || k > XR_RESHOW_MAX || eye < 0 || eye > 1) return false;
    XrSet *s = &g_xr.building;
    int i = k - 1;
    if (!g_xr.rs[0][i][0].sc) {  // (both pairs and eyes at once: the next set uses the other pair)
        static bool failed[XR_RESHOW_MAX];
        if (failed[i]) return false;
        for (int p = 0; p < 2; p++)
            for (int e = 0; e < 2; e++)
                if (!xr_chain_create(&g_xr.rs[p][i][e], e ? "re-shown right eye" : "re-shown left eye", g_xr.eyes[p][e].w, g_xr.eyes[p][e].h)) {
                    LOG("[xr] no swapchains for re-shown refresh %d: those refreshes show the set's own images\n", k);
                    failed[i] = true;
                    return false;
                }
        LOG("[xr] swapchains for re-shown refresh %d: 2 pairs x 2 eyes of %ux%u, %u images each (%.0f MB)\n", k, g_xr.eyes[0][0].w,
            g_xr.eyes[0][0].h, g_xr.rs[0][i][0].count, 4.0 * g_xr.rs[0][i][0].count * g_xr.eyes[0][0].w * g_xr.eyes[0][0].h * 4 / 1048576.0);
    }
    XrChain *c = &g_xr.rs[s->pair][i][eye];
    if (!s->rs_has[i][eye]) {
        if (!xr_chain_acquire(c, &s->rs_img[i][eye])) return false;
        s->rs_has[i][eye] = true;
    }
    *out = (XrEyeTarget){c->img[s->rs_img[i][eye]], c->view[s->rs_img[i][eye]], c->view_fmt, {c->w, c->h}};
    return true;
}

void xr_reshow_ready(int k) {
    if (!g_xr.in_set || k < 1 || k > XR_RESHOW_MAX) return;
    XrSet *s = &g_xr.building;
    if (s->rs_has[k - 1][0] && s->rs_has[k - 1][1]) s->rs_ok[k - 1] = true, s->rs_n = s->rs_n > k ? s->rs_n : k;
}

bool xr_eye_size(uint32_t *w, uint32_t *h) {
    if (!g_xr.eyes[0][0].sc) return false;
    *w = g_xr.eyes[0][0].w, *h = g_xr.eyes[0][0].h;
    return true;
}

void xr_stub_eye_colour(int eye, float rgba[4]) {
    static int test = -1;
    if (test < 0) test = getenv("LARECOMP_XR_EYE_TEST") && atoi(getenv("LARECOMP_XR_EYE_TEST"));
    // (the start screen's own background, 0.04 0.05 0.06, in both eyes; the test: left blue, right red)
    float c[4] = {0.04f, 0.05f, 0.06f, 1};
    if (test) c[0] = eye ? 0.5f : 0.02f, c[1] = 0.02f, c[2] = eye ? 0.02f : 0.5f;
    memcpy(rgba, c, sizeof c);
}
