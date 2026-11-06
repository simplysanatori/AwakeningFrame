// runtime/xr internals: the OpenXR entry points (the loader is dlopened: no link-time dependency on a runtime) and
// the state shared by xr_session.c, xr_vk.c and xr_loop.c.
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>
#include <time.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_TIMESPEC
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "xr.h"

#define XR_FUNCS(F)                                                                                                   \
    F(xrDestroyInstance) F(xrGetInstanceProperties) F(xrGetSystem) F(xrGetSystemProperties) F(xrPollEvent)            \
    F(xrResultToString) F(xrCreateSession) F(xrDestroySession) F(xrBeginSession) F(xrEndSession)                      \
    F(xrRequestExitSession) F(xrCreateReferenceSpace) F(xrDestroySpace) F(xrLocateSpace) F(xrLocateViews)             \
    F(xrEnumerateViewConfigurationViews) F(xrEnumerateSwapchainFormats) F(xrCreateSwapchain) F(xrDestroySwapchain)    \
    F(xrEnumerateSwapchainImages) F(xrAcquireSwapchainImage) F(xrWaitSwapchainImage) F(xrReleaseSwapchainImage)       \
    F(xrWaitFrame) F(xrBeginFrame) F(xrEndFrame) F(xrGetVulkanGraphicsRequirements2KHR)                               \
    F(xrCreateVulkanInstanceKHR) F(xrGetVulkanGraphicsDevice2KHR) F(xrCreateVulkanDeviceKHR)
#define XR_DECL(n) extern PFN_##n p_##n;
XR_FUNCS(XR_DECL)
#undef XR_DECL
// optional (XR_FB_display_refresh_rate)
extern PFN_xrEnumerateDisplayRefreshRatesFB p_xrEnumerateDisplayRefreshRatesFB;
extern PFN_xrGetDisplayRefreshRateFB p_xrGetDisplayRefreshRateFB;
extern PFN_xrRequestDisplayRefreshRateFB p_xrRequestDisplayRefreshRateFB;
extern PFN_xrConvertTimeToTimespecTimeKHR p_xrConvertTimeToTimespecTimeKHR;  // (XR_KHR_convert_timespec_time)

const char *xr_result_str(XrResult r);
#define XR_OK(call) xr_ok_((call), #call)
bool xr_ok_(XrResult r, const char *what);  // logs a failure; true when it succeeded

typedef struct {
    XrSwapchain sc;
    int64_t fmt;            // the swapchain's (sRGB) format
    VkFormat view_fmt;      // its UNORM twin
    uint32_t w, h, count;
    VkImage img[8];
    VkImageView view[8];
} XrChain;

typedef struct {
    bool valid;
    int pair;               // which pair of swapchains (g_xr.hud[pair], g_xr.eyes[pair])
    uint64_t seq;           // the timeline value of the submission holding its GPU work
    uint32_t hud;           // image indices
    uint32_t eye[2];
    bool has_eye[2];
    XrView views[2];        // latched (xr_latch_views)
    bool views_ok;
    XrSpace space;          // the app space the views were located in
    uint64_t made_ns;       // when it was submitted (stats)
} XrSet;

typedef struct {
    // instance / system
    void *loader;
    XrInstance inst;
    XrSystemId sys;
    bool has_refresh_fb, has_format_list;
    // session
    XrSession session;
    XrSessionState state;
    bool running;           // between xrBeginSession and xrEndSession
    uint64_t begin_ns;      // plat_now_ns() at xrBeginSession (xr_timing.log's clock)
    XrSpace local, view, app;
    XrSpace retired[2];     // earlier app spaces a set may still name
    bool face_due, face_watch;
    XrTime face_after, face_watch_until;
    bool head_valid;
    float head_yaw, head_pos[3];
    float display_hz;
    // Vulkan
    VkInstance vinst;
    VkPhysicalDevice vphys;
    VkDevice vdev;
    uint32_t qfam;
    VkQueue queue;
    // Two of each, used by alternate sets: a swapchain a layer names has no image acquired at xrEndFrame (the set being
    // made is on the other pair; the mock checks it, and SteamVR may show an acquired image otherwise)
    XrChain hud[2], eyes[2][2];  // [pair], [pair][eye]
    XrViewConfigurationView vcv[2];
    // the set hand-off (mu / cv)
    pthread_mutex_t mu;
    pthread_cond_t cv;
    XrSet building;         // the presenting thread's, between xr_set_begin and xr_set_submit (no lock: its own)
    bool in_set;
    XrSet pending;          // submitted, not yet released (XR thread releases it)
    XrSet shown;            // the latest released: the layers name it
    bool shown_any;
    // the XR thread
    pthread_t thread;
    volatile bool stop;
    bool thread_on;
    _Atomic int64_t target_time;  // XrTime the latest vsync's game frame is predicted for
    float quad_dist, quad_width;
} XrCore;
extern XrCore g_xr;

// xr_session.c
bool xr_load_loader(void);
bool xr_instance_create(void);
void xr_request_app_quit(void);
bool xr_session_create(void);
void xr_poll_events(bool *quit);
void xr_update_app_space(XrTime t);
void xr_session_destroy(void);
// xr_vk.c
void xr_drain_gpu(const char *before);
bool xr_chains_create(uint32_t hud_w, uint32_t hud_h);
void xr_chains_destroy(void);
bool xr_chain_acquire(XrChain *c, uint32_t *index);
void xr_chain_release(XrChain *c);
// xr_loop.c
bool xr_thread_start(void);
void xr_thread_stop(void);
void xr_release_completed(void);  // (XR thread) the pending set, once its GPU work finished

// record.c: the queue's external synchronisation
void vk_queue_lock(void);
void vk_queue_unlock(void);
uint64_t vk_completed_seq(void);  // (exec.c)
