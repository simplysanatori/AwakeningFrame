// xrblendprobe: what does the OpenXR runtime offer for compositing the void (passthrough / alpha blend)?
//
// Lists every instance extension the runtime offers (marking the passthrough / alpha related ones), then
// xrEnumerateEnvironmentBlendModes for the HMD's primary stereo view configuration (and every other configuration it
// reports), and the system properties. No session, no swapchain, no frame loop: nothing shows in the headset.
//
//   xrblendprobe          (XR_RUNTIME_JSON picks another runtime, e.g. the mock; LARECOMP_XR_LOADER the loader)
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>

static PFN_xrGetInstanceProcAddr gipa;
#define XRF(inst, name) PFN_##name name = NULL; gipa(inst, #name, (PFN_xrVoidFunction *)&name)

static const char *blend_name(XrEnvironmentBlendMode m) {
    switch (m) {
    case XR_ENVIRONMENT_BLEND_MODE_OPAQUE: return "OPAQUE";
    case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE: return "ADDITIVE";
    case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND: return "ALPHA_BLEND";
    default: return "?";
    }
}
static const char *view_name(XrViewConfigurationType t) {
    switch (t) {
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO: return "PRIMARY_MONO";
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO: return "PRIMARY_STEREO";
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO: return "PRIMARY_QUAD_VARJO";
    case XR_VIEW_CONFIGURATION_TYPE_SECONDARY_MONO_FIRST_PERSON_OBSERVER_MSFT: return "SECONDARY_MONO_FIRST_PERSON_OBSERVER_MSFT";
    default: return "?";
    }
}

int main(void) {
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
    static const char *const want[] = {"XR_FB_passthrough", "XR_HTC_passthrough", "XR_EXT_composition_layer_inverted_alpha",
                                       "XR_FB_composition_layer_alpha_blend", "XR_KHR_composition_layer_color_scale_bias",
                                       "XR_META_", "XR_FB_", "XR_HTC_", "XR_VARJO_", "XR_PICO_", "XR_ML_", "XR_MSFT_",
                                       "XR_KHR_composition_layer_depth", "XR_EXT_hand_tracking", "XR_EXT_eye_gaze"};
    printf("instance extensions (%u):\n", nx);
    for (uint32_t i = 0; i < nx; i++) {
        bool hot = false;
        for (size_t k = 0; k < sizeof want / sizeof *want; k++) hot |= !strncmp(xp[i].extensionName, want[k], strlen(want[k]));
        printf("  %s v%u%s\n", xp[i].extensionName, xp[i].extensionVersion, hot ? "   <-- passthrough/alpha/vendor" : "");
    }
    const char *names[] = {"XR_FB_passthrough", "XR_HTC_passthrough", "XR_EXT_composition_layer_inverted_alpha",
                           "XR_FB_composition_layer_alpha_blend", "XR_KHR_composition_layer_color_scale_bias"};
    for (int k = 0; k < 5; k++) {
        bool y = false;
        for (uint32_t i = 0; i < nx; i++) y |= !strcmp(xp[i].extensionName, names[k]);
        printf("  %s: %s\n", names[k], y ? "OFFERED" : "not offered");
    }
    uint32_t n_meta = 0;
    for (uint32_t i = 0; i < nx; i++) n_meta += !strncmp(xp[i].extensionName, "XR_META_", 8);
    printf("  XR_META_* extensions offered: %u\n", n_meta);
    free(xp);

    XrInstanceCreateInfo xci = {.type = XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(xci.applicationInfo.applicationName, "xrblendprobe");
    strcpy(xci.applicationInfo.engineName, "LARecomp");
    xci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrInstance xi = XR_NULL_HANDLE;
    XrResult xr = xrCreateInstance(&xci, &xi);
    if (XR_FAILED(xr)) { printf("FAIL xrCreateInstance %d\n", xr); return 1; }
    XRF(xi, xrGetInstanceProperties);
    XRF(xi, xrGetSystem);
    XRF(xi, xrGetSystemProperties);
    XRF(xi, xrDestroyInstance);
    XRF(xi, xrEnumerateViewConfigurations);
    XRF(xi, xrEnumerateEnvironmentBlendModes);
    XrInstanceProperties ip = {.type = XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(xi, &ip);
    printf("runtime: %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
           XR_VERSION_PATCH(ip.runtimeVersion));
    XrSystemGetInfo sgi = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId sys = 0;
    xr = xrGetSystem(xi, &sgi, &sys);
    if (XR_FAILED(xr)) { printf("FAIL xrGetSystem %d\n", xr); xrDestroyInstance(xi); return 1; }
    XrSystemProperties sp = {.type = XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(xi, sys, &sp);
    printf("system: %s, max swapchain %ux%u, layers %u, orientation %d position %d\n", sp.systemName,
           sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight,
           sp.graphicsProperties.maxLayerCount, sp.trackingProperties.orientationTracking, sp.trackingProperties.positionTracking);

    uint32_t nv = 0;
    xrEnumerateViewConfigurations(xi, sys, 0, &nv, NULL);
    XrViewConfigurationType *vt = calloc(nv ? nv : 1, sizeof *vt);
    xrEnumerateViewConfigurations(xi, sys, nv, &nv, vt);
    for (uint32_t v = 0; v < nv; v++) {
        uint32_t nb = 0;
        xr = xrEnumerateEnvironmentBlendModes(xi, sys, vt[v], 0, &nb, NULL);
        XrEnvironmentBlendMode *bm = calloc(nb ? nb : 1, sizeof *bm);
        if (XR_SUCCEEDED(xr)) xr = xrEnumerateEnvironmentBlendModes(xi, sys, vt[v], nb, &nb, bm);
        printf("view configuration %s (%d): xrEnumerateEnvironmentBlendModes -> %d, %u mode(s):", view_name(vt[v]), vt[v], xr, nb);
        for (uint32_t i = 0; i < nb; i++) printf(" %s(%d)", blend_name(bm[i]), bm[i]);
        printf("\n");
        free(bm);
    }
    free(vt);
    xrDestroyInstance(xi);
    return 0;
}
