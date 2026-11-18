// The OpenXR instance, system, session, reference spaces and events (from GalaxyFrame's xr_app.cpp: initInstance,
// initSession, handleEvents, setAppSpace / updateAppSpace, requestRefreshRate).
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>

#include "../rt.h"
#include "../settings.h"
#include "xr_input.h"
#include "xr_internal.h"

XrCore g_xr = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr;
static PFN_xrCreateInstance p_xrCreateInstance;
static PFN_xrEnumerateInstanceExtensionProperties p_xrEnumerateInstanceExtensionProperties;
#define XR_DEF(n) PFN_##n p_##n;
XR_FUNCS(XR_DEF)
#undef XR_DEF
PFN_xrEnumerateDisplayRefreshRatesFB p_xrEnumerateDisplayRefreshRatesFB;
PFN_xrGetDisplayRefreshRateFB p_xrGetDisplayRefreshRateFB;
PFN_xrRequestDisplayRefreshRateFB p_xrRequestDisplayRefreshRateFB;
#ifdef _WIN32
static PFN_xrConvertTimeToWin32PerformanceCounterKHR p_xrConvertTime;
#else
static PFN_xrConvertTimeToTimespecTimeKHR p_xrConvertTime;
#endif

bool xr_time_convertible(void) { return p_xrConvertTime != NULL; }
bool xr_time_to_plat(XrTime t, int64_t *ns) {
    if (!p_xrConvertTime) return false;
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER pc;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    if (XR_FAILED(p_xrConvertTime(g_xr.inst, t, &pc))) return false;
    *ns = (int64_t)((unsigned __int128)pc.QuadPart * 1000000000u / (uint64_t)f.QuadPart);  // (plat_now_ns's formula)
#else
    struct timespec ts;
    if (XR_FAILED(p_xrConvertTime(g_xr.inst, t, &ts))) return false;
    *ns = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
#endif
    return true;
}

// LARECOMP_XR=1 (the Frame's launcher sets it; --vr, xr_args), read once. A PC VR package (LARECOMP_PCVR, make PCVR=1)
// is in VR unless LARECOMP_XR=0 (--flat), or xr_pc_start turned it off.
static int g_wanted = -1;
bool xr_wanted(void) {
    if (g_wanted < 0) {
        const char *e = getenv("LARECOMP_XR");
#ifdef LARECOMP_PCVR
        g_wanted = !e || !*e || atoi(e) != 0;
#else
        g_wanted = e && *e && atoi(e) != 0;
#endif
    }
    return g_wanted;
}

// --vr / --flat on the command line (main.c, first thing: before anything reads LARECOMP_XR). The re-exec of a start
// whose guest window was taken keeps the command line, so it keeps the choice.
void xr_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--vr")) setenv("LARECOMP_XR", "1", 1);
        else if (!strcmp(argv[i], "--flat")) setenv("LARECOMP_XR", "0", 1);
    }
}

const char *xr_result_str(XrResult r) {
    static __thread char b[XR_MAX_RESULT_STRING_SIZE];
    if (!g_xr.inst || !p_xrResultToString || XR_FAILED(p_xrResultToString(g_xr.inst, r, b))) snprintf(b, sizeof b, "%d", (int)r);
    return b;
}

bool xr_ok_(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return true;
    LOG("[xr] %s failed: %s\n", what, xr_result_str(r));
    return false;
}

// The loader: LARECOMP_XR_LOADER, else SteamVR's on the Frame (no soname: by path), else the one the package carries or
// the system's (Linux: the AppImage's usr/lib comes first, through the program's RUNPATH; Windows: LoadLibrary looks in
// the exe's folder first). XR_RUNTIME_JSON (the loader's own variable) picks another runtime, e.g. tools/xrmock.
#ifdef _WIN32
static void *lib_open(const char *p) { return (void *)LoadLibraryA(p); }
static void *lib_sym(void *l, const char *n) { return (void *)GetProcAddress((HMODULE)l, n); }
static const char *lib_error(void) {
    static char b[48];
    snprintf(b, sizeof b, "Windows error %lu", GetLastError());
    return b;
}
// (the Khronos release's name, then a MinGW build's)
static const char *const k_loaders[] = {"openxr_loader.dll", "libopenxr_loader.dll"};
#else
#include <dlfcn.h>
static void *lib_open(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void *lib_sym(void *l, const char *n) { return dlsym(l, n); }
static const char *lib_error(void) { return dlerror(); }
static const char *const k_loaders[] = {"/opt/steamvr/bin/linuxarm64/libopenxr_loader.so", "libopenxr_loader.so.1", "libopenxr_loader.so"};
#endif
bool xr_load_loader(void) {
    if (g_xr.loader) return true;
    const char *own = getenv("LARECOMP_XR_LOADER");
    if (own && *own) g_xr.loader = lib_open(own);
    for (size_t i = 0; i < sizeof k_loaders / sizeof *k_loaders && !g_xr.loader; i++) g_xr.loader = lib_open(k_loaders[i]);
    if (!g_xr.loader) { LOG("[xr] no OpenXR loader (%s)\n", lib_error()); return false; }
    p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)lib_sym(g_xr.loader, "xrGetInstanceProcAddr");
    if (!p_xrGetInstanceProcAddr) { LOG("[xr] the OpenXR loader has no xrGetInstanceProcAddr\n"); return false; }
    p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&p_xrCreateInstance);
    p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                            (PFN_xrVoidFunction *)&p_xrEnumerateInstanceExtensionProperties);
    return p_xrCreateInstance && p_xrEnumerateInstanceExtensionProperties;
}

// xrCreateInstance with the Vulkan binding (and the refresh-rate / format-list extensions when offered), then the HMD
// system. Called from xr_vk_create_instance (xr_vk.c).
bool xr_instance_create(void) {
    if (g_xr.inst) return true;
    if (!xr_load_loader()) return false;
    uint32_t n = 0;
    if (!XR_OK(p_xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL))) return false;
    XrExtensionProperties *xp = calloc(n ? n : 1, sizeof *xp);
    for (uint32_t i = 0; i < n; i++) xp[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    p_xrEnumerateInstanceExtensionProperties(NULL, n, &n, xp);
    bool en2 = false, refresh = false, fmtlist = false, timespec = false, valve = false, gaze = false;
    for (uint32_t i = 0; i < n; i++) {
        en2 |= !strcmp(xp[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        refresh |= !strcmp(xp[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        fmtlist |= !strcmp(xp[i].extensionName, XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME);
        timespec |= !strcmp(xp[i].extensionName, XR_TIME_EXTENSION_NAME);
        valve |= !strcmp(xp[i].extensionName, XR_INPUT_VALVE_EXT);
        gaze |= !strcmp(xp[i].extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    }
    free(xp);
    if (!en2) { LOG("[xr] the OpenXR runtime has no Vulkan binding (XR_KHR_vulkan_enable2)\n"); return false; }
    const char *ext[6];
    uint32_t ne = 0;
    ext[ne++] = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
    if (refresh) ext[ne++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    if (fmtlist) ext[ne++] = XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME;
    if (timespec) ext[ne++] = XR_TIME_EXTENSION_NAME;  // (xr_timing.log, the pose prediction: display times on our clock)
    if (valve) ext[ne++] = XR_INPUT_VALVE_EXT;  // (the Frame controllers' own profile: xr_input.c)
    if (gaze) ext[ne++] = XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME;  // (the eye tracker: foveation, xr_input.c)
    XrInstanceCreateInfo ci = {.type = XR_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionCount = ne, .enabledExtensionNames = ext};
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "AwakeningFrame");
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "LARecomp");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    if (!XR_OK(p_xrCreateInstance(&ci, &g_xr.inst))) return false;
#define XR_GET(nm) p_xrGetInstanceProcAddr(g_xr.inst, #nm, (PFN_xrVoidFunction *)&p_##nm);
    XR_FUNCS(XR_GET)
#undef XR_GET
#define XR_CHECKFN(nm) if (!p_##nm) { LOG("[xr] runtime lacks %s\n", #nm); return false; }
    XR_FUNCS(XR_CHECKFN)
#undef XR_CHECKFN
    if (refresh) {
        p_xrGetInstanceProcAddr(g_xr.inst, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&p_xrEnumerateDisplayRefreshRatesFB);
        p_xrGetInstanceProcAddr(g_xr.inst, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction *)&p_xrGetDisplayRefreshRateFB);
        p_xrGetInstanceProcAddr(g_xr.inst, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&p_xrRequestDisplayRefreshRateFB);
    }
#ifdef _WIN32
    if (timespec) p_xrGetInstanceProcAddr(g_xr.inst, "xrConvertTimeToWin32PerformanceCounterKHR", (PFN_xrVoidFunction *)&p_xrConvertTime);
#else
    if (timespec) p_xrGetInstanceProcAddr(g_xr.inst, "xrConvertTimeToTimespecTimeKHR", (PFN_xrVoidFunction *)&p_xrConvertTime);
#endif
    g_xr.has_gaze = gaze;
    xr_input_init(p_xrGetInstanceProcAddr, valve);
    g_xr.has_refresh_fb = refresh && p_xrRequestDisplayRefreshRateFB;
    g_xr.has_format_list = fmtlist;
    XrInstanceProperties ip = {.type = XR_TYPE_INSTANCE_PROPERTIES};
    p_xrGetInstanceProperties(g_xr.inst, &ip);
    LOG("[xr] runtime %s %u.%u.%u (refresh rate extension %s, swapchain format list %s)\n", ip.runtimeName,
        XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion),
        refresh ? "yes" : "no", fmtlist ? "yes" : "no");
    XrSystemGetInfo sgi = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    if (!XR_OK(p_xrGetSystem(g_xr.inst, &sgi, &g_xr.sys))) return false;
    XrSystemEyeGazeInteractionPropertiesEXT gp = {.type = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemProperties sp = {.type = XR_TYPE_SYSTEM_PROPERTIES, .next = gaze ? &gp : NULL};
    if (XR_SUCCEEDED(p_xrGetSystemProperties(g_xr.inst, g_xr.sys, &sp)))
        LOG("[xr] system: %s (max swapchain %ux%u, %u layers; eye gaze %s)\n", sp.systemName, sp.graphicsProperties.maxSwapchainImageWidth,
            sp.graphicsProperties.maxSwapchainImageHeight, sp.graphicsProperties.maxLayerCount,
            !gaze ? "no extension" : gp.supportsEyeGazeInteraction ? "supported" : "extension, but not supported");
    return true;
}

static XrSpace ref_space(XrReferenceSpaceType type, XrPosef pose) {
    XrReferenceSpaceCreateInfo rs = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = type, .poseInReferenceSpace = pose};
    XrSpace s = XR_NULL_HANDLE;
    XR_OK(p_xrCreateReferenceSpace(g_xr.session, &rs, &s));
    return s;
}
static const XrPosef identity = {.orientation = {0, 0, 0, 1}};

__thread bool xr_rt_call;
bool xr_in_runtime_call(void) { return xr_rt_call; }

bool xr_session_create(void) {
    XrGraphicsBindingVulkan2KHR gb = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = g_xr.vinst,
                                      .physicalDevice = g_xr.vphys, .device = g_xr.vdev, .queueFamilyIndex = g_xr.qfam,
                                      .queueIndex = 0};
    XrSessionCreateInfo ci = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &gb, .systemId = g_xr.sys};
    XrResult r = XR_RT(p_xrCreateSession(g_xr.inst, &ci, &g_xr.session));
    if (!xr_ok_(r, "xrCreateSession")) return false;
    g_xr.local = ref_space(XR_REFERENCE_SPACE_TYPE_LOCAL, identity);
    g_xr.view = ref_space(XR_REFERENCE_SPACE_TYPE_VIEW, identity);
    g_xr.app = ref_space(XR_REFERENCE_SPACE_TYPE_LOCAL, identity);
    if (!g_xr.local || !g_xr.view || !g_xr.app) return false;
    xr_input_attach();
    {   // Passthrough (Table Edge) needs ALPHA_BLEND; a runtime without it keeps OPAQUE (the void then shows black)
        PFN_xrEnumerateEnvironmentBlendModes eb = NULL;
        XrEnvironmentBlendMode bm[8];
        uint32_t nb = 0;
        p_xrGetInstanceProcAddr(g_xr.inst, "xrEnumerateEnvironmentBlendModes", (PFN_xrVoidFunction *)&eb);
        if (eb && XR_SUCCEEDED(eb(g_xr.inst, g_xr.sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &nb, bm)))
            for (uint32_t i = 0; i < nb && i < 8; i++) g_xr.has_alpha_blend |= bm[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
        LOG("[xr] environment blend modes: %u, alpha blend %s\n", nb, g_xr.has_alpha_blend ? "yes (Passthrough)" : "no (Passthrough shows black)");
    }
    const char *q = getenv("LARECOMP_XR_QUAD");  // "distance,width" in metres
    g_xr.quad_dist = 2.0f, g_xr.quad_width = 2.2f;
    if (q) sscanf(q, "%f,%f", &g_xr.quad_dist, &g_xr.quad_width);
    return true;
}

// The display's refresh: LARECOMP_XR_HZ (120 by default: two refreshes per 60 Hz game frame; when it is not set and the
// runtime offers no 120, 90: the game held at 30 shows every frame for three). SteamVR on the Frame lists only the rate
// it runs at and refuses even that; the launch wrapper sets the per-app rate (preferredRefreshRate) before the start.
// SteamVR on a PC takes the rate from its own settings (docs/af/PCVR.md); runtimes that take requests get this one.
static void request_refresh_rate(void) {
    g_xr.display_hz = 0;
    if (!g_xr.has_refresh_fb) return;
    float rates[16];
    uint32_t n = 0;
    char list[128] = "";
    if (p_xrEnumerateDisplayRefreshRatesFB && XR_SUCCEEDED(p_xrEnumerateDisplayRefreshRatesFB(g_xr.session, 16, &n, rates)))
        for (uint32_t i = 0; i < n && i < 16; i++) snprintf(list + strlen(list), sizeof list - strlen(list), " %.0f", rates[i]);
    const char *e = getenv("LARECOMP_XR_HZ");
    float want = e ? (float)atof(e) : 120.0f;
    bool offered = false, has90 = false;
    for (uint32_t i = 0; i < n && i < 16; i++) offered |= fabsf(rates[i] - want) < 0.5f, has90 |= fabsf(rates[i] - 90) < 0.5f;
    if (!e && !offered && has90) want = 90, offered = true;
    XrResult r = offered ? p_xrRequestDisplayRefreshRateFB(g_xr.session, want) : XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB;
    float now = 0;
    if (p_xrGetDisplayRefreshRateFB && XR_SUCCEEDED(p_xrGetDisplayRefreshRateFB(g_xr.session, &now))) g_xr.display_hz = now;
    LOG("[xr] display refresh rates:%s Hz; %.0f Hz wanted: %s; now %.0f Hz\n", list, want,
        offered ? xr_result_str(r) : "not offered (the launch wrapper sets SteamVR's per-app rate)", now);
}

void xr_drain_gpu(const char *before);  // xr_vk.c

void xr_poll_events(bool *quit) {
    XrEventDataBuffer ev = {.type = XR_TYPE_EVENT_DATA_BUFFER};
    while (p_xrPollEvent(g_xr.inst, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const XrEventDataSessionStateChanged *sc = (const XrEventDataSessionStateChanged *)&ev;
            g_xr.state = sc->state;
            LOG("[xr] session state %d\n", (int)sc->state);
            if (sc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {.type = XR_TYPE_SESSION_BEGIN_INFO,
                                         .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                if (XR_OK(p_xrBeginSession(g_xr.session, &bi))) {
                    g_xr.running = true;
                    g_xr.begin_ns = plat_now_ns();
                    g_xr.face_due = true;  // the view starts where the player faces (xr_update_app_space)
                    g_xr.face_watch = true;
                    g_xr.face_after = 0;
                    request_refresh_rate();
                }
            } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                xr_drain_gpu("xrEndSession");
                XR_OK(p_xrEndSession(g_xr.session));
                g_xr.running = false;
            } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                *quit = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            *quit = true;
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            // The player recentred (SteamVR's LOCAL change): the view is put in front of them again once LOCAL moved.
            const XrEventDataReferenceSpaceChangePending *rc = (const XrEventDataReferenceSpaceChangePending *)&ev;
            LOG("[xr] reference space %d changes\n", (int)rc->referenceSpaceType);
            if (rc->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                g_xr.face_due = true;
                g_xr.face_after = rc->changeTime;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            const XrEventDataDisplayRefreshRateChangedFB *rc = (const XrEventDataDisplayRefreshRateChangedFB *)&ev;
            LOG("[xr] display refresh rate %.0f -> %.0f Hz\n", rc->fromDisplayRefreshRate, rc->toDisplayRefreshRate);
            g_xr.display_hz = rc->toDisplayRefreshRate;
            break;
        }
        default:
            break;
        }
        ev = (XrEventDataBuffer){.type = XR_TYPE_EVENT_DATA_BUFFER};
    }
}

static void quat_rotate(const XrQuaternionf *q, const float v[3], float out[3]) {
    // v + 2 w (u x v) + 2 u x (u x v), u = q.xyz
    float u[3] = {q->x, q->y, q->z}, w = q->w;
    float t[3] = {2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]), 2 * (u[0] * v[1] - u[1] * v[0])};
    out[0] = v[0] + w * t[0] + (u[1] * t[2] - u[2] * t[1]);
    out[1] = v[1] + w * t[1] + (u[2] * t[0] - u[0] * t[2]);
    out[2] = v[2] + w * t[2] + (u[0] * t[1] - u[1] * t[0]);
}

// The app's space: LOCAL moved to the head and turned to where it faces (about the vertical only), at a session's first
// tracked frame, after a recentre, and when the head's pose jumps in a session's first 5 s (a headset just woken up
// still finding its place). (XR thread, after xrWaitFrame.) Earlier spaces stay alive for two more changes: a set
// located in one may still be shown.
void xr_update_app_space(XrTime time) {
    if (g_xr.face_watch) {
        g_xr.face_watch = false;
        g_xr.face_watch_until = time + 5000000000ll;
        g_xr.head_valid = false;
    }
    if (!g_xr.face_due && time >= g_xr.face_watch_until) return;
    XrSpaceLocation loc = {.type = XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                      XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if (XR_FAILED(p_xrLocateSpace(g_xr.view, g_xr.local, time, &loc)) || (loc.locationFlags & need) != need) {
        g_xr.head_valid = false;
        return;
    }
    static const float x_axis[3] = {1, 0, 0};
    float right[3];
    quat_rotate(&loc.pose.orientation, x_axis, right);
    float yaw = atan2f(-right[2], right[0]);
    const XrVector3f *p = &loc.pose.position;
    if (!g_xr.face_due && g_xr.head_valid) {
        float turned = fabsf(remainderf(yaw - g_xr.head_yaw, 6.2831853f));
        float dx = p->x - g_xr.head_pos[0], dy = p->y - g_xr.head_pos[1], dz = p->z - g_xr.head_pos[2];
        float moved = sqrtf(dx * dx + dy * dy + dz * dz);
        if (turned > 0.52f || moved > 0.5f) {
            LOG("[xr] the head's pose jumped %.0f deg and %.2f m in a frame: tracking settled\n", turned * 57.29578f, moved);
            g_xr.face_due = true;
        }
    }
    g_xr.head_valid = true;
    g_xr.head_yaw = yaw;
    g_xr.head_pos[0] = p->x, g_xr.head_pos[1] = p->y, g_xr.head_pos[2] = p->z;
    if (!g_xr.face_due || time < g_xr.face_after) return;
    g_xr.face_due = false;
    XrPosef pose = {.orientation = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)}, .position = *p};
    XrSpace s = ref_space(XR_REFERENCE_SPACE_TYPE_LOCAL, pose);
    if (!s) return;
    pthread_mutex_lock(&g_xr.mu);
    if (g_xr.retired[1]) p_xrDestroySpace(g_xr.retired[1]);
    g_xr.retired[1] = g_xr.retired[0];
    g_xr.retired[0] = g_xr.app;
    g_xr.app = s;
    pthread_mutex_unlock(&g_xr.mu);
    LOG("[xr] the view starts where the player faces: %.0f deg round from the runtime's own, head at %.2f %.2f %.2f m\n",
        yaw * 57.29578f, p->x, p->y, p->z);
}

void xr_recentre(void) {  // (any thread; read by xr_update_app_space on the XR thread)
    __atomic_store_n(&g_xr.face_after, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_xr.face_due, true, __ATOMIC_RELEASE);
}

void xr_session_destroy(void) {
    xr_input_shutdown();
    if (g_xr.session) {
        if (g_xr.running && XR_OK(p_xrRequestExitSession(g_xr.session))) {
            // the runtime moves the session to STOPPING; xr_poll_events ends it there (half a second at most)
            bool quit = false;
            uint64_t t0 = plat_now_ns();
            while (g_xr.running && plat_now_ns() - t0 < 500000000ull) {
                xr_poll_events(&quit);
                if (g_xr.running) plat_sleep_until_ns(plat_now_ns() + 5000000ull);
            }
            if (g_xr.running) LOG("[xr] the session did not stop within 0.5 s: destroyed running\n");
        }
        XrSpace sp[] = {g_xr.app, g_xr.retired[0], g_xr.retired[1], g_xr.view, g_xr.local};
        for (size_t i = 0; i < sizeof sp / sizeof *sp; i++) if (sp[i]) p_xrDestroySpace(sp[i]);
        g_xr.app = g_xr.retired[0] = g_xr.retired[1] = g_xr.view = g_xr.local = XR_NULL_HANDLE;
        xr_drain_gpu("xrDestroySession");
        p_xrDestroySession(g_xr.session);
        g_xr.session = XR_NULL_HANDLE;
    }
    if (g_xr.inst) {
        p_xrDestroyInstance(g_xr.inst);
        g_xr.inst = XR_NULL_HANDLE;
    }
}

// The session ended (EXITING, or the runtime is going away): the app closes as when its window is closed.
void xr_request_app_quit(void) {
    rt_request_quit("the XR session ended");
}

// A PC VR package's start (main.c, once the data folder is known; nothing in other builds). It starts in VR unless told
// otherwise, so a double click or a Steam library entry needs no options, but:
//   - no game files yet: the start screen opens in the window, where its folder picker (a desktop dialog) can be seen;
//     VR from the next start (--vr: VR now);
//   - no OpenXR runtime, or none with a headset (the instance or the system fails): the window, with a log line, instead
//     of a start that stops. Asked for with --vr or LARECOMP_XR=1, a failure still stops with the error.
// The start screen's update check is off: it names upstream's flat releases, not this package's.
void xr_pc_start(const char *game_dir) {
#ifdef LARECOMP_PCVR
    setenv("LARECOMP_NO_UPDATE_CHECK", "1", 0);
    if (!xr_wanted()) { LOG("[xr] PC VR package: in the window (--flat or LARECOMP_XR=0)\n"); return; }
    const char *e = getenv("LARECOMP_XR");
    if (e && *e) { LOG("[xr] PC VR package: VR mode (asked for)\n"); return; }
    char p[1100];
    struct stat st;
    snprintf(p, sizeof p, "%s/exefs/main", game_dir);
    if (stat(p, &st) != 0) {
        LOG("[xr] PC VR package: no game files yet: the start screen opens in the window to set them up; VR from the next "
            "start (--vr: VR now)\n");
        g_wanted = 0;
        return;
    }
    settings_load();  // (the VR tab's rows into the environment first: xr_input_init reads the layout)
    if (!xr_instance_create()) {
        LOG("[xr] PC VR package: no OpenXR runtime with a headset (see above): starting in the window. Start SteamVR with "
            "the headset connected, then start the game again (--vr: stop with the error instead)\n");
        if (g_xr.inst) p_xrDestroyInstance(g_xr.inst), g_xr.inst = XR_NULL_HANDLE;
        g_wanted = 0;
        return;
    }
    LOG("[xr] PC VR package: VR mode (--flat for the window)\n");
#else
    (void)game_dir;
#endif
}
