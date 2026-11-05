// xrvkprobe: can the OpenXR runtime make LAS Recomp's Vulkan 1.3 instance and device?
//
// xrCreateInstance + xrGetSystem + xrGetVulkanGraphicsRequirements2KHR, then xrCreateVulkanInstanceKHR and
// xrCreateVulkanDeviceKHR with apiVersion 1.3 and the extension + feature chain runtime/nvn/vulkan/device.c asks for
// (create_device: every optional extension it enables when the GPU has it). No session, no frame loop, no swapchain:
// nothing shows in the headset and it takes well under a second.
//
//   xrvkprobe [--api 1.2|1.3] [--validate]      (XR_RUNTIME_JSON picks another runtime, e.g. a mock)
//
// The OpenXR loader is dlopened (LARECOMP_XR_LOADER, else SteamVR's), as the runtime does it.
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

static PFN_xrGetInstanceProcAddr gipa;
#define XRF(inst, name) PFN_##name name = NULL; gipa(inst, #name, (PFN_xrVoidFunction *)&name)
#define VSTR(v) VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v)

static bool has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL vvl_cb(VkDebugUtilsMessageSeverityFlagBitsEXT s, VkDebugUtilsMessageTypeFlagsEXT t,
                                             const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)t, (void)u;
    if (s >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) printf("  [vvl] %s\n", d->pMessage);
    return VK_FALSE;
}

int main(int argc, char **argv) {
    uint32_t api = VK_API_VERSION_1_3;
    bool validate = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--api") && i + 1 < argc) api = !strcmp(argv[++i], "1.2") ? VK_API_VERSION_1_2 : VK_API_VERSION_1_3;
        else if (!strcmp(argv[i], "--validate")) validate = true;
        else { fprintf(stderr, "usage: xrvkprobe [--api 1.2|1.3] [--validate]\n"); return 2; }
    }
    const char *lp = getenv("LARECOMP_XR_LOADER");
    void *lib = dlopen(lp ? lp : "/opt/steamvr/bin/linuxarm64/libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("FAIL no OpenXR loader: %s\n", dlerror()); return 1; }
    gipa = (PFN_xrGetInstanceProcAddr)dlsym(lib, "xrGetInstanceProcAddr");
    XRF(XR_NULL_HANDLE, xrCreateInstance);
    XRF(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties);

    uint32_t nx = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &nx, NULL);
    XrExtensionProperties *xp = calloc(nx, sizeof *xp);
    for (uint32_t i = 0; i < nx; i++) xp[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, nx, &nx, xp);
    bool en2 = false;
    for (uint32_t i = 0; i < nx; i++) en2 |= !strcmp(xp[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    free(xp);
    if (!en2) { printf("FAIL runtime has no XR_KHR_vulkan_enable2\n"); return 1; }

    const char *xext[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo xci = {.type = XR_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionCount = 1, .enabledExtensionNames = xext};
    strcpy(xci.applicationInfo.applicationName, "xrvkprobe");
    strcpy(xci.applicationInfo.engineName, "LARecomp");
    xci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrInstance xi = XR_NULL_HANDLE;
    XrResult xr = xrCreateInstance(&xci, &xi);
    if (XR_FAILED(xr)) { printf("FAIL xrCreateInstance %d\n", xr); return 1; }
    XRF(xi, xrGetInstanceProperties);
    XRF(xi, xrGetSystem);
    XRF(xi, xrDestroyInstance);
    XRF(xi, xrGetVulkanGraphicsRequirements2KHR);
    XRF(xi, xrCreateVulkanInstanceKHR);
    XRF(xi, xrGetVulkanGraphicsDevice2KHR);
    XRF(xi, xrCreateVulkanDeviceKHR);
    XrInstanceProperties ip = {.type = XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(xi, &ip);
    printf("runtime: %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
           XR_VERSION_PATCH(ip.runtimeVersion));
    XrSystemGetInfo sgi = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId sys = 0;
    xr = xrGetSystem(xi, &sgi, &sys);
    if (XR_FAILED(xr)) { printf("FAIL xrGetSystem %d\n", xr); xrDestroyInstance(xi); return 1; }
    XrGraphicsRequirementsVulkan2KHR req = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    xr = xrGetVulkanGraphicsRequirements2KHR(xi, sys, &req);
    printf("xrGetVulkanGraphicsRequirements2KHR: %d, min %u.%u.%u max %u.%u.%u\n", xr, XR_VERSION_MAJOR(req.minApiVersionSupported),
           XR_VERSION_MINOR(req.minApiVersionSupported), XR_VERSION_PATCH(req.minApiVersionSupported),
           XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported),
           XR_VERSION_PATCH(req.maxApiVersionSupported));

    // ---- instance (device.c create_instance, without SDL's surface extensions: XR mode has no window)
    const char *iext[2];
    uint32_t n_iext = 0;
    if (validate) iext[n_iext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LARecomp", .pEngineName = "LARecomp",
                             .apiVersion = api};
    VkInstanceCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = n_iext, .ppEnabledExtensionNames = iext,
                                .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    XrVulkanInstanceCreateInfoKHR xvi = {.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = sys,
                                         .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = &vci};
    VkInstance inst = VK_NULL_HANDLE;
    VkResult vr = VK_ERROR_UNKNOWN;
    xr = xrCreateVulkanInstanceKHR(xi, &xvi, &inst, &vr);
    printf("xrCreateVulkanInstanceKHR(apiVersion %u.%u): XrResult %d, VkResult %d\n", VK_API_VERSION_MAJOR(api),
           VK_API_VERSION_MINOR(api), xr, vr);
    if (XR_FAILED(xr) || vr != VK_SUCCESS) { xrDestroyInstance(xi); return 1; }
    VkDebugUtilsMessengerEXT msgr = VK_NULL_HANDLE;
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = vvl_cb};
        if (f) f(inst, &mi, NULL, &msgr);
    }

    XrVulkanGraphicsDeviceGetInfoKHR gdi = {.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = sys, .vulkanInstance = inst};
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    xr = xrGetVulkanGraphicsDevice2KHR(xi, &gdi, &phys);
    if (XR_FAILED(xr)) { printf("FAIL xrGetVulkanGraphicsDevice2KHR %d\n", xr); return 1; }
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(phys, &pp);
    printf("physical device: %s, api %u.%u.%u\n", pp.deviceName, VSTR(pp.apiVersion));

    // ---- device (device.c create_device: the same extensions when present, the same features)
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ep);
    const char *ext[24];
    uint32_t n_ext = 0;
    static const char *const optional[] = {
        VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_EXT_DEVICE_FAULT_EXTENSION_NAME, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
        VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME};
    bool on[sizeof optional / sizeof *optional];
    for (size_t i = 0; i < sizeof optional / sizeof *optional; i++) {
        on[i] = has_ext(ep, ne, optional[i]);
        if (on[i]) ext[n_ext++] = optional[i];
        else printf("  (device has no %s: left out, as device.c does)\n", optional[i]);
    }
    free(ep);
    bool cbc_on = on[0], div_on = on[1], fault_on = on[3], vids_on = on[5], placed_on = on[8] && on[9];

    VkPhysicalDeviceVulkan12Features have12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT have_placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    if (placed_on) have12.pNext = &have_placed;
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have12};
    vkGetPhysicalDeviceFeatures2(phys, &have);

    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
        .customBorderColors = VK_TRUE, .customBorderColorWithoutFormat = VK_TRUE, .pNext = div_on ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE, .shaderDemoteToHelperInvocation = VK_TRUE,
        .shaderTerminateInvocation = VK_TRUE, .maintenance4 = VK_TRUE,
        .pNext = cbc_on ? (void *)&cbc : div_on ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore = VK_TRUE, .shaderInt8 = have12.shaderInt8, .samplerMirrorClampToEdge = have12.samplerMirrorClampToEdge,
        .uniformAndStorageBuffer8BitAccess = have12.uniformAndStorageBuffer8BitAccess,
        .storageBuffer8BitAccess = have12.storageBuffer8BitAccess, .scalarBlockLayout = have12.scalarBlockLayout,
        .bufferDeviceAddress = have12.bufferDeviceAddress, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE, .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault_on ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
        .memoryMapPlaced = VK_TRUE, .memoryUnmapReserve = have_placed.memoryUnmapReserve, .pNext = f.pNext};
    if (placed_on && have_placed.memoryMapPlaced) f.pNext = &placed;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT en_vids = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, .vertexInputDynamicState = VK_TRUE};
    if (vids_on) en_vids.pNext = f.pNext, f.pNext = &en_vids;
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend, b->dualSrcBlend = h->dualSrcBlend, b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy, b->textureCompressionBC = h->textureCompressionBC, b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp, b->depthClamp = h->depthClamp, b->largePoints = h->largePoints, b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray, b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics, b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess, b->shaderClipDistance = h->shaderClipDistance;

    uint32_t nq = 0, qfam = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qp);
    for (uint32_t i = 0; i < nq && qfam == UINT32_MAX; i++)
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) qfam = i;
    printf("queue families %u, graphics+compute family %u with %u queue(s)\n", nq, qfam, qfam < nq ? qp[qfam].queueCount : 0);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1,
                                  .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    printf("device extensions asked (%u):", n_ext);
    for (uint32_t i = 0; i < n_ext; i++) printf(" %s", ext[i]);
    printf("\nfeatures: 1.3 dynamicRendering synchronization2 demote terminate maintenance4; 1.2 timeline int8 %d mirrorClamp %d "
           "8bit %d/%d scalar %d bda %d; 1.1 storageBuffer16; placed %d unmapReserve %d; vids %d; cbc %d; divisor %d; fault %d\n",
           have12.shaderInt8, have12.samplerMirrorClampToEdge, have12.uniformAndStorageBuffer8BitAccess, have12.storageBuffer8BitAccess,
           have12.scalarBlockLayout, have12.bufferDeviceAddress, f.pNext == &placed || placed.pNext != NULL ? have_placed.memoryMapPlaced : 0,
           have_placed.memoryUnmapReserve, vids_on, cbc_on, div_on, fault_on);
    XrVulkanDeviceCreateInfoKHR xdi = {.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = sys,
                                       .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanPhysicalDevice = phys,
                                       .vulkanCreateInfo = &di};
    VkDevice dev = VK_NULL_HANDLE;
    vr = VK_ERROR_UNKNOWN;
    xr = xrCreateVulkanDeviceKHR(xi, &xdi, &dev, &vr);
    printf("xrCreateVulkanDeviceKHR: XrResult %d, VkResult %d\n", xr, vr);
    int rc = XR_FAILED(xr) || vr != VK_SUCCESS;
    if (!rc) {
        // the 1.3 entry points LAS calls without the KHR suffix (record.c)
        const char *core13[] = {"vkCmdBeginRendering", "vkCmdPipelineBarrier2", "vkQueueSubmit2", "vkCmdSetVertexInputEXT"};
        for (int i = 0; i < 4; i++) printf("  %s: %s\n", core13[i], vkGetDeviceProcAddr(dev, core13[i]) ? "present" : "MISSING");
        vkDestroyDevice(dev, NULL);
    }
    if (msgr) ((PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkDestroyDebugUtilsMessengerEXT"))(inst, msgr, NULL);
    vkDestroyInstance(inst, NULL);
    xrDestroyInstance(xi);
    printf("%s\n", rc ? "RESULT: FAIL" : "RESULT: OK");
    return rc;
}
