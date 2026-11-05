// Minimal OpenXR + Vulkan client for the mock runtime (and, in principle, any
// runtime): XR_KHR_vulkan_enable2 with a Vulkan 1.3 instance and device made
// through the runtime, LOCAL / VIEW / STAGE spaces, one colour swapchain per
// eye (sRGB) cleared to a colour per eye, an optional depth swapchain (depth
// info on the projection layer), a quad layer cleared to a third colour, and
// the usual wait / begin / locate / acquire / render / release / end loop.
//
//   test_client [frames=120] [--mutable] [--depth] [--vk12] [--bad-layout]
//     --mutable  eye swapchains get MUTABLE_FORMAT and a UNORM view format
//     --depth    add a depth swapchain and XrCompositionLayerDepthInfoKHR
//     --vk12     ask for a Vulkan 1.2 instance (what SteamVR offers)
//     --bad-layout  one deliberate validation error (a clear in the wrong layout): proves the layers are on
// Exit status 0 if the session ran its frames and exited cleanly.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define CK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { fprintf(stderr, "test_client: %s failed: %d (line %d)\n", #x, (int)r_, __LINE__); exit(2); } } while (0)
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "test_client: %s failed: %d (line %d)\n", #x, (int)r_, __LINE__); exit(3); } } while (0)

typedef struct {
    XrSwapchain sc;
    uint32_t w, h, n;
    XrSwapchainImageVulkan2KHR* im;
    int depth;
} Chain;

static XrInstance inst;
static XrSession sess;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer cb;
static VkFence fence;
static int gBadLayout;

static Chain makeChain(int64_t fmt, uint32_t w, uint32_t h, int depth, int mutableFmt) {
    Chain c = {0};
    c.w = w; c.h = h; c.depth = depth;
    XrVulkanSwapchainFormatListCreateInfoKHR list = {XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR};
    VkFormat views[1] = {VK_FORMAT_R8G8B8A8_UNORM};
    list.viewFormatCount = 1;
    list.viewFormats = views;
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.next = mutableFmt ? &list : NULL;
    ci.usageFlags = (depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | (mutableFmt ? XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT : 0);
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    CK(xrCreateSwapchain(sess, &ci, &c.sc));
    CK(xrEnumerateSwapchainImages(c.sc, 0, &c.n, NULL));
    c.im = calloc(c.n, sizeof(*c.im));
    for (uint32_t i = 0; i < c.n; i++) c.im[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    CK(xrEnumerateSwapchainImages(c.sc, c.n, &c.n, (XrSwapchainImageBaseHeader*)c.im));
    return c;
}

// Clears the next image of the chain and releases it.
static void clearAndRelease(Chain* c, const float rgba[4], float depthValue) {
    uint32_t idx;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    CK(xrAcquireSwapchainImage(c->sc, &ai, &idx));
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    CK(xrWaitSwapchainImage(c->sc, &wi));
    VkImage image = c->im[idx].image;
    VkImageAspectFlags aspect = c->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout attach = c->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VK(vkResetCommandBuffer(cb, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(cb, &bi));
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = c->depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = attach; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    if (c->depth) {
        VkClearDepthStencilValue d = {depthValue, 0};
        vkCmdClearDepthStencilImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &d, 1, &b.subresourceRange);
    } else {
        VkClearColorValue col = {{rgba[0], rgba[1], rgba[2], rgba[3]}};
        vkCmdClearColorImage(cb, image, gBadLayout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &b.subresourceRange);
    }
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = c->depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = attach;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VK(vkResetFences(dev, 1, &fence));
    VK(vkQueueSubmit(queue, 1, &si, fence));
    VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));  // finished before release
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    CK(xrReleaseSwapchainImage(c->sc, &ri));
}

int main(int argc, char** argv) {
    int frames = 120, useMutable = 0, useDepth = 0, vk12 = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mutable")) useMutable = 1;
        else if (!strcmp(argv[i], "--depth")) useDepth = 1;
        else if (!strcmp(argv[i], "--vk12")) vk12 = 1;
        else if (!strcmp(argv[i], "--bad-layout")) gBadLayout = 1;
        else frames = atoi(argv[i]);
    }
    const char* exts[] = {"XR_KHR_vulkan_enable2", "XR_KHR_composition_layer_depth"};
    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "xrmock test_client");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = useDepth ? 2 : 1;
    ici.enabledExtensionNames = exts;
    CK(xrCreateInstance(&ici, &inst));
    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    CK(xrGetSystem(inst, &sgi, &sys));

    PFN_xrGetVulkanGraphicsRequirements2KHR getReq;
    PFN_xrCreateVulkanInstanceKHR createInst;
    PFN_xrGetVulkanGraphicsDevice2KHR getDev;
    PFN_xrCreateVulkanDeviceKHR createDev;
    CK(xrGetInstanceProcAddr(inst, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction*)&getReq));
    CK(xrGetInstanceProcAddr(inst, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction*)&createInst));
    CK(xrGetInstanceProcAddr(inst, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction*)&getDev));
    CK(xrGetInstanceProcAddr(inst, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction*)&createDev));
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    CK(getReq(inst, sys, &req));
    printf("test_client: runtime offers Vulkan %u.%u - %u.%u\n", XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
           XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xrmock test_client";
    app.apiVersion = vk12 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_3;
    VkInstanceCreateInfo vici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vici.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xvi = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xvi.systemId = sys;
    xvi.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xvi.vulkanCreateInfo = &vici;
    VkInstance vki;
    VkResult vr;
    CK(createInst(inst, &xvi, &vki, &vr));
    VK(vr);
    XrVulkanGraphicsDeviceGetInfoKHR gdi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gdi.systemId = sys;
    gdi.vulkanInstance = vki;
    VkPhysicalDevice phys;
    CK(getDev(inst, &gdi, &phys));
    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    uint32_t family = 0;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dq.queueFamilyIndex = family; dq.queueCount = 1; dq.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &dq;
    XrVulkanDeviceCreateInfoKHR xdi = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdi.systemId = sys;
    xdi.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xdi.vulkanPhysicalDevice = phys;
    xdi.vulkanCreateInfo = &dci;
    CK(createDev(inst, &xdi, &dev, &vr));
    VK(vr);
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    VK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(dev, &fci, NULL, &fence));

    XrGraphicsBindingVulkan2KHR bind = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    bind.instance = vki; bind.physicalDevice = phys; bind.device = dev; bind.queueFamilyIndex = family; bind.queueIndex = 0;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &bind;
    sci.systemId = sys;
    CK(xrCreateSession(inst, &sci, &sess));

    XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rci.poseInReferenceSpace.orientation.w = 1;
    XrSpace local, view, stage;
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; CK(xrCreateReferenceSpace(sess, &rci, &local));
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; CK(xrCreateReferenceSpace(sess, &rci, &view));
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE; CK(xrCreateReferenceSpace(sess, &rci, &stage));

    uint32_t nv = 2;
    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    CK(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv));
    uint32_t nf;
    CK(xrEnumerateSwapchainFormats(sess, 0, &nf, NULL));
    int64_t* fmts = calloc(nf, sizeof(int64_t));
    CK(xrEnumerateSwapchainFormats(sess, nf, &nf, fmts));
    int64_t colorFmt = 0, depthFmt = 0;
    printf("test_client: formats:");
    for (uint32_t i = 0; i < nf; i++) {
        printf(" %lld", (long long)fmts[i]);
        if (fmts[i] == VK_FORMAT_R8G8B8A8_SRGB) colorFmt = fmts[i];
        if (fmts[i] == VK_FORMAT_D32_SFLOAT || (!depthFmt && fmts[i] == VK_FORMAT_D16_UNORM)) depthFmt = fmts[i];
    }
    printf("\n");
    if (!colorFmt) { fprintf(stderr, "test_client: no RGBA8 sRGB swapchain format\n"); return 4; }
    Chain eye[2], quad, dsc = {0};
    for (int e = 0; e < 2; e++) eye[e] = makeChain(colorFmt, vcv[e].recommendedImageRectWidth, vcv[e].recommendedImageRectHeight, 0, useMutable);
    quad = makeChain(colorFmt, 512, 256, 0, 0);
    if (useDepth) {
        if (!depthFmt) { fprintf(stderr, "test_client: no depth format\n"); return 4; }
        dsc = makeChain(depthFmt, eye[0].w, eye[0].h, 1, 0);
    }
    printf("test_client: eyes %ux%u, quad %ux%u%s\n", eye[0].w, eye[0].h, quad.w, quad.h, useDepth ? ", depth swapchain" : "");

    const float eyeColor[2][4] = {{0.8f, 0.1f, 0.1f, 1}, {0.1f, 0.1f, 0.8f, 1}};
    const float quadColor[4] = {0.1f, 0.8f, 0.1f, 1};
    int running = 0, exiting = 0, done = 0, rendered = 0, requested = 0;
    while (!done) {
        XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                XrSessionState st = ((XrEventDataSessionStateChanged*)&ev)->state;
                if (st == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    CK(xrBeginSession(sess, &bi));
                    running = 1;
                } else if (st == XR_SESSION_STATE_STOPPING) {
                    CK(xrEndSession(sess));
                    running = 0;
                } else if (st == XR_SESSION_STATE_EXITING) {
                    done = 1; exiting = 1;
                }
            }
            ev = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) { usleep(1000); continue; }
        if (rendered >= frames && !requested) { CK(xrRequestExitSession(sess)); requested = 1; }
        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        CK(xrWaitFrame(sess, NULL, &fs));
        CK(xrBeginFrame(sess, NULL));
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        XrViewState vs = {XR_TYPE_VIEW_STATE};
        XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = local;
        uint32_t cnt;
        CK(xrLocateViews(sess, &li, &vs, 2, &cnt, views));
        if (rendered == 0) {  // a stage-relative head pose, to exercise the spaces
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            CK(xrLocateSpace(view, stage, fs.predictedDisplayTime, &loc));
            printf("test_client: head in STAGE at %.2f %.2f %.2f; first eye in LOCAL at %.3f %.3f %.3f\n", loc.pose.position.x, loc.pose.position.y, loc.pose.position.z,
                   views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z);
        }
        const XrCompositionLayerBaseHeader* layers[2];
        uint32_t nl = 0;
        XrCompositionLayerProjectionView pv[2];
        XrCompositionLayerDepthInfoKHR di[2];
        XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerQuad ql = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (fs.shouldRender) {
            for (int e = 0; e < 2; e++) {
                clearAndRelease(&eye[e], eyeColor[e], 0);
                pv[e] = (XrCompositionLayerProjectionView){XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                pv[e].pose = views[e].pose;
                pv[e].fov = views[e].fov;
                pv[e].subImage.swapchain = eye[e].sc;
                pv[e].subImage.imageRect = (XrRect2Di){{0, 0}, {(int32_t)eye[e].w, (int32_t)eye[e].h}};
                if (useDepth) {
                    clearAndRelease(&dsc, NULL, 0.5f);
                    di[e] = (XrCompositionLayerDepthInfoKHR){XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
                    di[e].subImage.swapchain = dsc.sc;
                    di[e].subImage.imageRect = pv[e].subImage.imageRect;
                    di[e].minDepth = 0; di[e].maxDepth = 1; di[e].nearZ = 0.1f; di[e].farZ = 100.0f;
                    pv[e].next = &di[e];
                }
            }
            proj.space = local;
            proj.viewCount = 2;
            proj.views = pv;
            clearAndRelease(&quad, quadColor, 0);
            ql.space = local;
            ql.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            ql.subImage.swapchain = quad.sc;
            ql.subImage.imageRect = (XrRect2Di){{0, 0}, {(int32_t)quad.w, (int32_t)quad.h}};
            ql.pose.orientation.w = 1;
            ql.pose.position = (XrVector3f){0, -0.2f, -1.5f};
            ql.size = (XrExtent2Df){1.0f, 0.5f};
            layers[nl++] = (const XrCompositionLayerBaseHeader*)&proj;
            layers[nl++] = (const XrCompositionLayerBaseHeader*)&ql;
            rendered++;
        }
        XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = nl;
        ei.layers = layers;
        CK(xrEndFrame(sess, &ei));
    }
    printf("test_client: %d frames rendered, session %s\n", rendered, exiting ? "exited cleanly" : "ended");
    return exiting ? 0 : 5;
}
