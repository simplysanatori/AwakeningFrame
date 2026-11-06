// Mock OpenXR runtime, Vulkan side: XR_KHR_vulkan_enable2.  The instance and
// device are made with the app's own vkGetInstanceProcAddr (no libvulkan
// link, so validation layers and the app's loader config apply); swapchain
// images are VkImages on the app's device, handed out in
// COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL.  The mock uses
// the app's queue (the one in the session's graphics binding) for its own
// setup and PNG copies, as a runtime may: the app must not use that queue
// concurrently with an OpenXR call (its queue lock covers that).
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "xrmock_png.h"
#include "xrmock_vk.h"

extern void xrmock_error(const char* fmt, ...);
extern void xrmock_trace(const char* fmt, ...);

static PFN_vkGetInstanceProcAddr gGipa;
static VkInstance gInstance;
static VkPhysicalDevice gPhys;
static VkDevice gDevice;
static VkQueue gQueue;
static uint32_t gQueueFamily;
static PFN_vkGetDeviceProcAddr gGdpa;
static uint32_t gMaxMajor = 1, gMaxMinor = 3;
static int gReqCalled;

#define DEV(name) ((PFN_##name)gGdpa(gDevice, #name))
#define INST(name) ((PFN_##name)gGipa(gInstance, #name))

void mockvk_set_max_api(uint32_t major, uint32_t minor) {
    gMaxMajor = major;
    gMaxMinor = minor;
}

int mockvk_owns_binding(XrStructureType t) { return t == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR; }

int mockvk_describe_binding(const XrBaseInStructure* n, char* out, size_t cap) {
    const XrGraphicsBindingVulkan2KHR* b = (const XrGraphicsBindingVulkan2KHR*)n;
    int ok = b->instance == gInstance && b->physicalDevice == gPhys && b->device == gDevice;
    if (!ok) xrmock_error("session binding uses Vulkan handles not made through the runtime (xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR)");
    if (gDevice && b->device == gDevice) {
        gGdpa = (PFN_vkGetDeviceProcAddr)gGipa(gInstance, "vkGetDeviceProcAddr");
        DEV(vkGetDeviceQueue)(gDevice, b->queueFamilyIndex, b->queueIndex, &gQueue);
        gQueueFamily = b->queueFamilyIndex;
    }
    return snprintf(out, cap, "binding=VULKAN2 handles=%s queueFamily=%u queueIndex=%u", ok ? "made-by-runtime" : "FOREIGN", b->queueFamilyIndex, b->queueIndex);
}

static XrResult XRAPI_CALL vkRequirements(XrInstance i, XrSystemId s, XrGraphicsRequirementsVulkanKHR* r) {
    (void)i;
    (void)s;
    r->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    r->maxApiVersionSupported = XR_MAKE_VERSION(gMaxMajor, gMaxMinor, 0);
    gReqCalled = 1;
    return XR_SUCCESS;
}

static void listExts(char* out, size_t cap, uint32_t n, const char* const* names) {
    out[0] = 0;
    for (uint32_t i = 0; i < n && strlen(out) + 64 < cap; i++) {
        strcat(out, " ");
        strcat(out, names[i]);
    }
}

static XrResult XRAPI_CALL vkCreateInstanceXr(XrInstance i, const XrVulkanInstanceCreateInfoKHR* ci, VkInstance* out, VkResult* vr) {
    (void)i;
    if (!gReqCalled) xrmock_error("xrCreateVulkanInstanceKHR before xrGetVulkanGraphicsRequirements2KHR");
    gGipa = ci->pfnGetInstanceProcAddr;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gGipa(VK_NULL_HANDLE, "vkCreateInstance");
    const VkApplicationInfo* app = ci->vulkanCreateInfo->pApplicationInfo;
    char ex[1024];
    listExts(ex, sizeof(ex), ci->vulkanCreateInfo->enabledExtensionCount, ci->vulkanCreateInfo->ppEnabledExtensionNames);
    xrmock_trace("createVulkanInstance apiVersion=%u.%u layers=%u extensions:%s", app ? VK_API_VERSION_MAJOR(app->apiVersion) : 1,
                 app ? VK_API_VERSION_MINOR(app->apiVersion) : 0, ci->vulkanCreateInfo->enabledLayerCount, ex);
    if (app && VK_API_VERSION_MINOR(app->apiVersion) + 100 * VK_API_VERSION_MAJOR(app->apiVersion) > gMaxMinor + 100 * gMaxMajor) {
        xrmock_error("instance apiVersion %u.%u above the %u.%u the runtime offers", VK_API_VERSION_MAJOR(app->apiVersion),
                     VK_API_VERSION_MINOR(app->apiVersion), gMaxMajor, gMaxMinor);
        *vr = VK_ERROR_INCOMPATIBLE_DRIVER;
        *out = VK_NULL_HANDLE;
        return XR_SUCCESS;
    }
    *vr = create(ci->vulkanCreateInfo, ci->vulkanAllocator, out);
    gInstance = *out;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL vkGetDevice(XrInstance i, const XrVulkanGraphicsDeviceGetInfoKHR* gi, VkPhysicalDevice* out) {
    (void)i;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)gGipa(gi->vulkanInstance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gGipa(gi->vulkanInstance, "vkGetPhysicalDeviceProperties");
    VkPhysicalDevice devs[8];
    uint32_t n = 8;
    VkResult r = en(gi->vulkanInstance, &n, devs);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) return XR_ERROR_RUNTIME_FAILURE;
    VkPhysicalDevice pick = devs[0];
    for (uint32_t k = 0; k < n; k++) {  // a real GPU before a software one
        VkPhysicalDeviceProperties p;
        props(devs[k], &p);
        if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) { pick = devs[k]; break; }
    }
    gPhys = *out = pick;
    VkPhysicalDeviceProperties p;
    props(pick, &p);
    xrmock_trace("getVulkanDevice");
    fprintf(stderr, "xrmock: Vulkan device %s (api %u.%u.%u)\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
            VK_API_VERSION_PATCH(p.apiVersion));
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL vkCreateDeviceXr(XrInstance i, const XrVulkanDeviceCreateInfoKHR* ci, VkDevice* out, VkResult* vr) {
    (void)i;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)ci->pfnGetInstanceProcAddr(gInstance, "vkCreateDevice");
    char ex[2048];
    listExts(ex, sizeof(ex), ci->vulkanCreateInfo->enabledExtensionCount, ci->vulkanCreateInfo->ppEnabledExtensionNames);
    xrmock_trace("createVulkanDevice queues=%u extensions:%s", ci->vulkanCreateInfo->queueCreateInfoCount, ex);
    *vr = create(ci->vulkanPhysicalDevice, ci->vulkanCreateInfo, ci->vulkanAllocator, out);
    gDevice = *out;
    return XR_SUCCESS;
}

PFN_xrVoidFunction mockvk_get_proc(const char* name) {
    if (!strcmp(name, "xrGetVulkanGraphicsRequirements2KHR")) return (PFN_xrVoidFunction)vkRequirements;
    if (!strcmp(name, "xrCreateVulkanInstanceKHR")) return (PFN_xrVoidFunction)vkCreateInstanceXr;
    if (!strcmp(name, "xrGetVulkanGraphicsDevice2KHR")) return (PFN_xrVoidFunction)vkGetDevice;
    if (!strcmp(name, "xrCreateVulkanDeviceKHR")) return (PFN_xrVoidFunction)vkCreateDeviceXr;
    return NULL;
}

// As SteamVR offers them on the Frame, narrowed to what the device supports.
static const int64_t kWanted[] = {VK_FORMAT_R8G8B8A8_SRGB,       VK_FORMAT_B8G8R8A8_SRGB,       VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                  VK_FORMAT_R8G8B8A8_UNORM,      VK_FORMAT_B8G8R8A8_UNORM,      VK_FORMAT_D32_SFLOAT,          VK_FORMAT_D16_UNORM,
                                  VK_FORMAT_D24_UNORM_S8_UINT,   VK_FORMAT_D32_SFLOAT_S8_UINT};
static int64_t gOffered[16];
static int gOfferedN = -1;

static int isDepth(int64_t f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

int mockvk_formats(const int64_t** list) {
    if (gOfferedN < 0) {
        gOfferedN = 0;
        for (unsigned k = 0; k < sizeof(kWanted) / sizeof(kWanted[0]); k++) {
            VkFormatProperties fp;
            INST(vkGetPhysicalDeviceFormatProperties)(gPhys, (VkFormat)kWanted[k], &fp);
            VkFormatFeatureFlags need = isDepth(kWanted[k]) ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                           : (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
            if ((fp.optimalTilingFeatures & need) == need) gOffered[gOfferedN++] = kWanted[k];
        }
    }
    *list = gOffered;
    return gOfferedN;
}

typedef struct {
    int count, depth, layers, mips;
    int w, h;
    VkFormat format;
    VkImage* im;
    VkDeviceMemory* mem;
} VkImages;

static uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    INST(vkGetPhysicalDeviceMemoryProperties)(gPhys, &mp);
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) return t;
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if (bits & (1u << t)) return t;
    return 0;
}

// Runs `n` image barriers on the app's queue and waits for them.
typedef struct {
    VkImage image;
    int depth, layers, mips;
    VkImageLayout from, to;
    VkAccessFlags srcAccess, dstAccess;
} Barrier;

static void submitBarriers(const Barrier* b, int n, VkBuffer copyDst, const VkBufferImageCopy* copy, VkImage copySrc) {
    if (!gQueue) return;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = gQueueFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool pool;
    DEV(vkCreateCommandPool)(gDevice, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    DEV(vkAllocateCommandBuffers)(gDevice, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    DEV(vkBeginCommandBuffer)(cb, &bi);
    for (int pass = 0; pass < (copy ? 3 : 1); pass++) {
        // pass 0: the barriers; with a copy: 1 = the copy, 2 = back to the first layout
        if (pass == 1) {
            DEV(vkCmdCopyImageToBuffer)(cb, copySrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copyDst, 1, copy);
            continue;
        }
        for (int i = 0; i < n; i++) {
            VkImageMemoryBarrier m = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            m.srcAccessMask = pass == 0 ? b[i].srcAccess : b[i].dstAccess;
            m.dstAccessMask = pass == 0 ? b[i].dstAccess : b[i].srcAccess;
            m.oldLayout = pass == 0 ? b[i].from : b[i].to;
            m.newLayout = pass == 0 ? b[i].to : b[i].from;
            m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            m.image = b[i].image;
            m.subresourceRange.aspectMask = b[i].depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            if (b[i].depth && b[i].depth == 2) m.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            m.subresourceRange.levelCount = (uint32_t)b[i].mips;
            m.subresourceRange.layerCount = (uint32_t)b[i].layers;
            DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &m);
        }
    }
    DEV(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    DEV(vkQueueSubmit)(gQueue, 1, &si, VK_NULL_HANDLE);
    DEV(vkQueueWaitIdle)(gQueue);
    DEV(vkDestroyCommandPool)(gDevice, pool, NULL);
}

static int hasStencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

void* mockvk_create_images(const XrSwapchainCreateInfo* ci, int count, XrResult* err) {
    *err = XR_SUCCESS;
    if (!gDevice) { *err = XR_ERROR_RUNTIME_FAILURE; return NULL; }
    if (!gGdpa) gGdpa = (PFN_vkGetDeviceProcAddr)gGipa(gInstance, "vkGetDeviceProcAddr");
    VkFormat format = (VkFormat)ci->format;
    int depth = isDepth(format);
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // the compositor samples; the mock copies
    XrSwapchainUsageFlags uf = ci->usageFlags;
    if (uf & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (uf & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (uf & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (uf & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) u |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageCreateFlags flags = 0;
    VkFormat views[8];
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    int haveList = 0;
    for (const XrBaseInStructure* n = (const XrBaseInStructure*)ci->next; n; n = n->next) {
        if (n->type == XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR) {
            const XrVulkanSwapchainFormatListCreateInfoKHR* l = (const XrVulkanSwapchainFormatListCreateInfoKHR*)n;
            uint32_t k = 0;
            views[k++] = format;
            for (uint32_t v = 0; v < l->viewFormatCount && k < 8; v++)
                if (l->viewFormats[v] != format) views[k++] = l->viewFormats[v];
            fl.viewFormatCount = k;
            fl.pViewFormats = views;
            haveList = 1;
            flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        }
    }
    if (uf & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkImages* r = (VkImages*)calloc(1, sizeof(*r));
    r->count = count;
    r->depth = depth ? (hasStencil(format) ? 2 : 1) : 0;
    r->layers = ci->arraySize ? (int)ci->arraySize : 1;
    r->mips = (int)ci->mipCount;
    r->w = (int)ci->width;
    r->h = (int)ci->height;
    r->format = format;
    r->im = (VkImage*)calloc((size_t)count, sizeof(VkImage));
    r->mem = (VkDeviceMemory*)calloc((size_t)count, sizeof(VkDeviceMemory));
    Barrier bars[8];
    int nb = 0;
    for (int i = 0; i < count; i++) {
        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.pNext = haveList ? &fl : NULL;
        ic.flags = flags;
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = format;
        ic.extent.width = ci->width;
        ic.extent.height = ci->height;
        ic.extent.depth = 1;
        ic.mipLevels = ci->mipCount ? ci->mipCount : 1;
        ic.arrayLayers = (uint32_t)r->layers;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = u;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (i == 0) {  // is the combination one the device can make at all
            VkImageFormatProperties ip;
            VkResult q = INST(vkGetPhysicalDeviceImageFormatProperties)(gPhys, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, u, flags, &ip);
            if (q != VK_SUCCESS || ci->width > ip.maxExtent.width || ci->height > ip.maxExtent.height || (uint32_t)r->layers > ip.maxArrayLayers) {
                xrmock_trace("createSwapchain: device cannot make format 0x%x usage 0x%x flags 0x%x %ux%u x%d", (unsigned)format, (unsigned)u, (unsigned)flags,
                             ci->width, ci->height, r->layers);
                *err = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
                free(r->im); free(r->mem); free(r);
                return NULL;
            }
        }
        if (DEV(vkCreateImage)(gDevice, &ic, NULL, &r->im[i]) != VK_SUCCESS) {
            xrmock_error("vkCreateImage failed for a swapchain image");
            *err = XR_ERROR_RUNTIME_FAILURE;
            r->im[i] = VK_NULL_HANDLE;
            continue;
        }
        VkMemoryRequirements req;
        DEV(vkGetImageMemoryRequirements)(gDevice, r->im[i], &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (DEV(vkAllocateMemory)(gDevice, &ai, NULL, &r->mem[i]) == VK_SUCCESS) DEV(vkBindImageMemory)(gDevice, r->im[i], r->mem[i], 0);
        if (nb < 8) {
            Barrier b = {r->im[i], r->depth, r->layers, r->mips ? r->mips : 1, VK_IMAGE_LAYOUT_UNDEFINED,
                         depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                         depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
            bars[nb++] = b;
        }
    }
    // As a runtime hands them out (XR_KHR_vulkan_enable2): attachment layouts.
    // submitBarriers with no copy does one pass; its "from" is UNDEFINED here.
    submitBarriers(bars, nb, VK_NULL_HANDLE, NULL, VK_NULL_HANDLE);
    return r;
}

void mockvk_destroy_images(void* images) {
    VkImages* r = (VkImages*)images;
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        if (r->im[i]) DEV(vkDestroyImage)(gDevice, r->im[i], NULL);
        if (r->mem[i]) DEV(vkFreeMemory)(gDevice, r->mem[i], NULL);
    }
    free(r->im); free(r->mem); free(r);
}

XrResult mockvk_fill_images(void* images, int count, XrSwapchainImageBaseHeader* out) {
    const VkImages* r = (const VkImages*)images;
    XrSwapchainImageVulkan2KHR* v = (XrSwapchainImageVulkan2KHR*)out;
    for (int i = 0; i < count; i++) {
        if (v[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR) return XR_ERROR_VALIDATION_FAILURE;
        v[i].image = r->im[i];
    }
    return XR_SUCCESS;
}

double mockvk_release_wait_ms(void) {
    if (!gQueue) return 0.0;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    DEV(vkQueueWaitIdle)(gQueue);
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

static float halfToFloat(uint16_t h) {
    int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m + 1024), e - 25);
    return s ? -v : v;
}
static uint8_t linearToSrgb8(float l) {
    if (!(l > 0)) return 0;
    if (l >= 1) return 255;
    float s = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)(s * 255.0f + 0.5f);
}

static int dumpImpl(void* images, int index, uint32_t layer, int x, int y, int w, int h, const char* path, char* stats, size_t statcap, uint8_t** keep);
int mockvk_dump_png(void* images, int index, uint32_t layer, int x, int y, int w, int h, const char* path, char* stats, size_t statcap) {
    return dumpImpl(images, index, layer, x, y, w, h, path, stats, statcap, NULL);
}
uint8_t* mockvk_read_rgba(void* images, int index, uint32_t layer, int w, int h) {
    char stats[160];
    uint8_t* out = NULL;
    if (dumpImpl(images, index, layer, 0, 0, w, h, NULL, stats, sizeof stats, &out)) { free(out); return NULL; }
    return out;
}
static int dumpImpl(void* images, int index, uint32_t layer, int x, int y, int w, int h, const char* path, char* stats, size_t statcap, uint8_t** keep) {
    VkImages* r = (VkImages*)images;
    if (!r || index < 0 || index >= r->count || !r->im[index] || !gQueue) return -1;
    if (layer >= (uint32_t)r->layers) layer = 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > r->w) w = r->w - x;
    if (y + h > r->h) h = r->h - y;
    if (w <= 0 || h <= 0) return -1;
    int bpp = 0;
    switch (r->format) {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: bpp = 4; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8; break;
    case VK_FORMAT_D16_UNORM: bpp = 2; break;
    default: snprintf(stats, statcap, "format 0x%x not dumped", (unsigned)r->format); return -1;
    }
    VkDeviceSize size = (VkDeviceSize)w * (VkDeviceSize)h * (VkDeviceSize)bpp;
    VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = size;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf;
    if (DEV(vkCreateBuffer)(gDevice, &bc, NULL, &buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements req;
    DEV(vkGetBufferMemoryRequirements)(gDevice, buf, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem;
    if (DEV(vkAllocateMemory)(gDevice, &ai, NULL, &mem) != VK_SUCCESS) { DEV(vkDestroyBuffer)(gDevice, buf, NULL); return -1; }
    DEV(vkBindBufferMemory)(gDevice, buf, mem, 0);
    VkImageLayout attach = r->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    Barrier b = {r->im[index], r->depth, r->layers, r->mips ? r->mips : 1, attach, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 r->depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = r->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.baseArrayLayer = layer;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.x = x;
    c.imageOffset.y = y;
    c.imageExtent.width = (uint32_t)w;
    c.imageExtent.height = (uint32_t)h;
    c.imageExtent.depth = 1;
    submitBarriers(&b, 1, buf, &c, r->im[index]);
    void* p = NULL;
    int rc = -1;
    if (DEV(vkMapMemory)(gDevice, mem, 0, size, 0, &p) == VK_SUCCESS) {
        uint8_t* rgba = (uint8_t*)malloc((size_t)w * (size_t)h * 4);
        double sum[3] = {0, 0, 0};
        size_t npx = (size_t)w * (size_t)h;
        for (size_t k = 0; k < npx; k++) {
            uint8_t px[4] = {0, 0, 0, 255};
            const uint8_t* s = (const uint8_t*)p + k * (size_t)bpp;
            switch (r->format) {
            case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_UNORM: memcpy(px, s, 4); break;
            case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: px[0] = s[2]; px[1] = s[1]; px[2] = s[0]; px[3] = s[3]; break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
                uint32_t v;
                memcpy(&v, s, 4);
                px[0] = (uint8_t)((v & 1023) >> 2); px[1] = (uint8_t)(((v >> 10) & 1023) >> 2); px[2] = (uint8_t)(((v >> 20) & 1023) >> 2);
                px[3] = (uint8_t)(((v >> 30) & 3) * 85);
                break;
            }
            case VK_FORMAT_R16G16B16A16_SFLOAT: {
                uint16_t hv[4];
                memcpy(hv, s, 8);
                for (int ch = 0; ch < 3; ch++) px[ch] = linearToSrgb8(halfToFloat(hv[ch]));
                float a = halfToFloat(hv[3]);
                px[3] = (uint8_t)(a <= 0 ? 0 : a >= 1 ? 255 : a * 255 + 0.5f);
                break;
            }
            case VK_FORMAT_D32_SFLOAT: {
                float d;
                memcpy(&d, s, 4);
                px[0] = px[1] = px[2] = (uint8_t)(d <= 0 ? 0 : d >= 1 ? 255 : d * 255 + 0.5f);
                break;
            }
            case VK_FORMAT_D16_UNORM: {
                uint16_t d;
                memcpy(&d, s, 2);
                px[0] = px[1] = px[2] = (uint8_t)(d >> 8);
                break;
            }
            default: break;
            }
            memcpy(rgba + k * 4, px, 4);
            sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2];
        }
        rc = path ? png_write_rgba(path, rgba, w, h) : 0;
        snprintf(stats, statcap, "%dx%d mean=(%.0f,%.0f,%.0f)", w, h, sum[0] / (double)npx, sum[1] / (double)npx, sum[2] / (double)npx);
        if (keep) *keep = rgba;
        else free(rgba);
        DEV(vkUnmapMemory)(gDevice, mem);
    }
    DEV(vkDestroyBuffer)(gDevice, buf, NULL);
    DEV(vkFreeMemory)(gDevice, mem, NULL);
    return rc;
}

// ---- asynchronous luma probes (MOCK_XR_SHIFT) --------------------------------
// A copy of the centre of a released image into a host buffer, queued on the app's queue (behind the app's own work,
// before any later write to that image) with a fence, collected later without waiting: the frame loop is not held up.
#define PROBES 8
typedef struct {
    int busy, tag, w, h;
    VkFormat fmt;
    VkBuffer buf;
    VkDeviceMemory mem;
    void* map;
    VkDeviceSize cap;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
} Probe;
static Probe gProbe[PROBES];

int mockvk_probe_submit(void* images, int index, uint32_t layer, int x, int y, int w, int h, int tag) {
    VkImages* r = (VkImages*)images;
    if (!r || index < 0 || index >= r->count || !r->im[index] || !gQueue || r->depth) return -1;
    if (r->format != VK_FORMAT_R8G8B8A8_SRGB && r->format != VK_FORMAT_R8G8B8A8_UNORM && r->format != VK_FORMAT_B8G8R8A8_SRGB &&
        r->format != VK_FORMAT_B8G8R8A8_UNORM)
        return -1;
    if (layer >= (uint32_t)r->layers) layer = 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > r->w) w = r->w - x;
    if (y + h > r->h) h = r->h - y;
    if (w <= 0 || h <= 0) return -1;
    int k = 0;
    while (k < PROBES && gProbe[k].busy) k++;
    if (k == PROBES) return -1;
    Probe* p = &gProbe[k];
    VkDeviceSize size = (VkDeviceSize)w * (VkDeviceSize)h * 4;
    if (p->cap < size) {
        if (p->buf) { DEV(vkUnmapMemory)(gDevice, p->mem); DEV(vkDestroyBuffer)(gDevice, p->buf, NULL); DEV(vkFreeMemory)(gDevice, p->mem, NULL); }
        VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bc.size = size;
        bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (DEV(vkCreateBuffer)(gDevice, &bc, NULL, &p->buf) != VK_SUCCESS) { p->buf = VK_NULL_HANDLE; p->cap = 0; return -1; }
        VkMemoryRequirements req;
        DEV(vkGetBufferMemoryRequirements)(gDevice, p->buf, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (DEV(vkAllocateMemory)(gDevice, &ai, NULL, &p->mem) != VK_SUCCESS) { DEV(vkDestroyBuffer)(gDevice, p->buf, NULL); p->buf = VK_NULL_HANDLE; p->cap = 0; return -1; }
        DEV(vkBindBufferMemory)(gDevice, p->buf, p->mem, 0);
        DEV(vkMapMemory)(gDevice, p->mem, 0, size, 0, &p->map);
        p->cap = size;
    }
    if (!p->pool) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = gQueueFamily;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        DEV(vkCreateCommandPool)(gDevice, &pci, NULL, &p->pool);
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = p->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        DEV(vkAllocateCommandBuffers)(gDevice, &ai, &p->cb);
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        DEV(vkCreateFence)(gDevice, &fi, NULL, &p->fence);
    }
    DEV(vkResetCommandBuffer)(p->cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    DEV(vkBeginCommandBuffer)(p->cb, &bi);
    for (int pass = 0; pass < 2; pass++) {
        VkImageMemoryBarrier m = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        m.srcAccessMask = pass ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        m.dstAccessMask = pass ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
        m.oldLayout = pass ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        m.newLayout = pass ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        m.image = r->im[index];
        m.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        m.subresourceRange.levelCount = (uint32_t)(r->mips ? r->mips : 1);
        m.subresourceRange.layerCount = (uint32_t)r->layers;
        DEV(vkCmdPipelineBarrier)(p->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &m);
        if (pass == 0) {
            VkBufferImageCopy c;
            memset(&c, 0, sizeof(c));
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.baseArrayLayer = layer;
            c.imageSubresource.layerCount = 1;
            c.imageOffset.x = x;
            c.imageOffset.y = y;
            c.imageExtent.width = (uint32_t)w;
            c.imageExtent.height = (uint32_t)h;
            c.imageExtent.depth = 1;
            DEV(vkCmdCopyImageToBuffer)(p->cb, r->im[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->buf, 1, &c);
        }
    }
    DEV(vkEndCommandBuffer)(p->cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &p->cb;
    DEV(vkResetFences)(gDevice, 1, &p->fence);
    if (DEV(vkQueueSubmit)(gQueue, 1, &si, p->fence) != VK_SUCCESS) return -1;
    p->busy = 1, p->tag = tag, p->w = w, p->h = h, p->fmt = r->format;
    return k;
}

// A finished probe: its tag, size and luma (0-255, Rec. 601 weights on the stored bytes) box-averaged by `sub`
// into out (w/sub x h/sub, row-major; out must hold that many). 1 when one was collected, 0 when none is done.
int mockvk_probe_collect(int* tag, int* w, int* h, float* out, int sub) {
    for (int k = 0; k < PROBES; k++) {
        Probe* p = &gProbe[k];
        if (!p->busy || DEV(vkGetFenceStatus)(gDevice, p->fence) != VK_SUCCESS) continue;
        int ow = p->w / sub, oh = p->h / sub;
        int bgr = p->fmt == VK_FORMAT_B8G8R8A8_SRGB || p->fmt == VK_FORMAT_B8G8R8A8_UNORM;
        const uint8_t* s = (const uint8_t*)p->map;
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) {
                float acc = 0;
                for (int j = 0; j < sub; j++)
                    for (int i = 0; i < sub; i++) {
                        const uint8_t* q = s + (((size_t)(y * sub + j) * (size_t)p->w) + (size_t)(x * sub + i)) * 4;
                        float rr = bgr ? q[2] : q[0], gg = q[1], bb = bgr ? q[0] : q[2];
                        acc += 0.299f * rr + 0.587f * gg + 0.114f * bb;
                    }
                out[(size_t)y * (size_t)ow + (size_t)x] = acc / (float)(sub * sub);
            }
        *tag = p->tag, *w = ow, *h = oh;
        p->busy = 0;
        return 1;
    }
    return 0;
}

// ---- a compositor's share of the GPU (MOCK_XR_COMPOSITOR) ---------------------------------------------------------
// SteamVR's compositor shares the GPU with the app: each refresh it needs about a millisecond of GPU (its timewarp of
// the layers) finished before the vsync, from its own high-priority context. This models it: a second VkDevice (its
// own kernel context, global priority HIGH where allowed), and a thread that submits a job of two eye-sized image
// copies `lead` ms before each vsync of the mock's grid and waits for it. The job's latency (submit to done) shows how
// long the app's GPU work keeps a compositor from the GPU; a job done after its vsync is a refresh the compositor
// would have missed (the display would show its previous output again, not reprojected to the new head pose).
static struct {
    VkDevice dev;
    VkQueue q;
    PFN_vkGetDeviceProcAddr gdpa;
    VkImage im[4];
    VkDeviceMemory mem[4];
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    pthread_t thread;
    volatile int stop, running;
    int64_t t0, period, lead, from;
    int prio;
    FILE* log;
    // stats from `from` on
    int n, late;
    double sum, worst;
    float* lat;
} gComp;
#define CDEV(name) ((PFN_##name)gComp.gdpa(gComp.dev, #name))
static int64_t compNow(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
static void* compThread(void* arg) {
    (void)arg;
    int64_t n = (compNow() - gComp.t0) / gComp.period + 1;
    while (!gComp.stop) {
        int64_t vs = gComp.t0 + n * gComp.period, at = vs - gComp.lead, now = compNow();
        if (at < now) { n++; continue; }
        struct timespec ts = {(time_t)((at - now) / 1000000000ll), (long)((at - now) % 1000000000ll)};
        nanosleep(&ts, NULL);
        CDEV(vkResetFences)(gComp.dev, 1, &gComp.fence);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &gComp.cb;
        int64_t s = compNow();
        if (CDEV(vkQueueSubmit)(gComp.q, 1, &si, gComp.fence) != VK_SUCCESS) break;
        CDEV(vkWaitForFences)(gComp.dev, 1, &gComp.fence, VK_TRUE, 1000000000ull);
        int64_t d = compNow();
        double ms = (double)(d - s) / 1e6;
        int late = d > vs;
        if (gComp.log) fprintf(gComp.log, "%.3f %.3f %d\n", (double)(vs - gComp.t0) / 1e9, ms, late);
        if (vs - gComp.t0 >= gComp.from && gComp.n < 400000) {
            gComp.lat[gComp.n++] = (float)ms;
            gComp.sum += ms, gComp.late += late;
            if (ms > gComp.worst) gComp.worst = ms;
        }
        n = (compNow() - gComp.t0) / gComp.period + 1;
    }
    return NULL;
}
int mockvk_compositor_start(int64_t t0, int64_t period, double lead_ms, double from_s, int eye_w, int eye_h, const char* logpath) {
    if (gComp.running || !gInstance || !gPhys) return -1;
    float prio = 1.0f;
    VkDeviceQueueGlobalPriorityCreateInfoKHR gp = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = 0;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char* ext[] = {"VK_KHR_global_priority"};
    VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qi;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    static const VkQueueGlobalPriorityKHR tries[] = {VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR, VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR, VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR};
    int first = 0;  // MOCK_XR_COMPOSITOR_PRIO=realtime|high|medium|none: where to start (default realtime, then lower)
    const char* pe = getenv("MOCK_XR_COMPOSITOR_PRIO");
    if (pe) first = !strcmp(pe, "high") ? 1 : !strcmp(pe, "medium") ? 2 : !strcmp(pe, "none") ? 3 : 0;
    for (int i = first; i < 4 && r != VK_SUCCESS; i++) {
        if (i < 3) {
            gp.globalPriority = tries[i];
            qi.pNext = &gp;
            dc.enabledExtensionCount = 1;
            dc.ppEnabledExtensionNames = ext;
            gComp.prio = (int)tries[i];
        } else {
            qi.pNext = NULL;
            dc.enabledExtensionCount = 0;
            gComp.prio = 0;
        }
        r = INST(vkCreateDevice)(gPhys, &dc, NULL, &gComp.dev);
    }
    if (r != VK_SUCCESS) return -1;
    gComp.gdpa = (PFN_vkGetDeviceProcAddr)gGipa(gInstance, "vkGetDeviceProcAddr");
    CDEV(vkGetDeviceQueue)(gComp.dev, 0, 0, &gComp.q);
    VkPhysicalDeviceMemoryProperties mp;
    INST(vkGetPhysicalDeviceMemoryProperties)(gPhys, &mp);
    for (int i = 0; i < 4; i++) {
        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = VK_FORMAT_R8G8B8A8_UNORM;
        ic.extent = (VkExtent3D){(uint32_t)eye_w, (uint32_t)eye_h, 1};
        ic.mipLevels = ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        CDEV(vkCreateImage)(gComp.dev, &ic, NULL, &gComp.im[i]);
        VkMemoryRequirements req;
        CDEV(vkGetImageMemoryRequirements)(gComp.dev, gComp.im[i], &req);
        uint32_t t = 0;
        for (t = 0; t < mp.memoryTypeCount; t++)
            if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = t;
        CDEV(vkAllocateMemory)(gComp.dev, &ai, NULL, &gComp.mem[i]);
        CDEV(vkBindImageMemory)(gComp.dev, gComp.im[i], gComp.mem[i], 0);
    }
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    CDEV(vkCreateCommandPool)(gComp.dev, &pci, NULL, &gComp.pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = gComp.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    CDEV(vkAllocateCommandBuffers)(gComp.dev, &cai, &gComp.cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CDEV(vkBeginCommandBuffer)(gComp.cb, &bi);
    VkImageMemoryBarrier m[4];
    for (int i = 0; i < 4; i++) {
        m[i] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        m[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        m[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        m[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        m[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        m[i].srcQueueFamilyIndex = m[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        m[i].image = gComp.im[i];
        m[i].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    CDEV(vkCmdPipelineBarrier)(gComp.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 4, m);
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {eye_w, eye_h, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {eye_w, eye_h, 1}}};
    for (int e = 0; e < 2; e++)  // (a filtered blit per eye: the compositor's warp reads and writes about this much)
        CDEV(vkCmdBlitImage)(gComp.cb, gComp.im[e], VK_IMAGE_LAYOUT_GENERAL, gComp.im[2 + e], VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
    CDEV(vkEndCommandBuffer)(gComp.cb);
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CDEV(vkCreateFence)(gComp.dev, &fi, NULL, &gComp.fence);
    gComp.t0 = t0, gComp.period = period, gComp.lead = (int64_t)(lead_ms * 1e6), gComp.from = (int64_t)(from_s * 1e9);
    gComp.lat = (float*)calloc(400000, sizeof(float));
    gComp.log = logpath ? fopen(logpath, "w") : NULL;
    gComp.stop = 0;
    if (pthread_create(&gComp.thread, NULL, compThread, NULL) != 0) return -1;
    gComp.running = 1;
    return gComp.prio;
}
static int cmpfl(const void* a, const void* b) { float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }
void mockvk_compositor_stop(char* out, size_t cap) {
    out[0] = 0;
    if (!gComp.running) return;
    gComp.stop = 1;
    pthread_join(gComp.thread, NULL);
    gComp.running = 0;
    if (gComp.log) fclose(gComp.log), gComp.log = NULL;
    if (gComp.n) {
        qsort(gComp.lat, (size_t)gComp.n, sizeof(float), cmpfl);
        snprintf(out, cap, "compositor model (priority %d, %.1f ms before each vsync): %d refreshes, job latency avg %.2f ms, p50 %.2f, p95 %.2f, p99 %.2f, max %.2f; %d done after their vsync (%.1f%%)",
                 gComp.prio, (double)gComp.lead / 1e6, gComp.n, gComp.sum / gComp.n, gComp.lat[gComp.n / 2], gComp.lat[(int)(gComp.n * 0.95)], gComp.lat[(int)(gComp.n * 0.99)],
                 gComp.worst, gComp.late, 100.0 * gComp.late / gComp.n);
    }
    CDEV(vkDeviceWaitIdle)(gComp.dev);
}
