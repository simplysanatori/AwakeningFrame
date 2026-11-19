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
// The runtime's clock to ours (plat_now_ns): XR_KHR_convert_timespec_time on Linux (CLOCK_MONOTONIC),
// XR_KHR_win32_convert_performance_counter_time on Windows (QueryPerformanceCounter, runtime/platform/windows.c)
#ifdef _WIN32
#include <windows.h>
#include <unknwn.h>  // (openxr_platform.h's Win32 part names IUnknown, which WIN32_LEAN_AND_MEAN leaves out)
#define XR_USE_PLATFORM_WIN32
#define XR_TIME_EXTENSION_NAME XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME
#else
#define XR_USE_TIMESPEC
#define XR_TIME_EXTENSION_NAME XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME
#endif
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
// An XrTime on plat_now_ns's clock (ns). false: the runtime offers no conversion (then its XrTime is taken to be that clock
// already, as SteamVR's on Linux and the mock's are; xr_thread_start predicts from the vsync then).
bool xr_time_to_plat(XrTime t, int64_t *ns);
bool xr_time_convertible(void);

extern __thread bool xr_rt_call;  // (xr_in_runtime_call)
#define XR_RT(call) ({ xr_rt_call = true; XrResult r_ = (call); xr_rt_call = false; r_; })
const char *xr_result_str(XrResult r);
#define XR_OK(call) xr_ok_((call), #call)
bool xr_ok_(XrResult r, const char *what);  // logs a failure; true when it succeeded

typedef struct {
    XrSwapchain sc;
    int64_t fmt;            // the swapchain's (sRGB) format
    VkFormat view_fmt;      // its UNORM twin
    uint32_t w, h, count;
    uint32_t layers;        // array layers (2: both eyes, layer = eye; the re-shown refreshes' LARECOMP_XR_REPROJ_MV)
    VkImage img[8];
    VkImageView view[8];    // (layers > 1: a 2D_ARRAY view of all of them)
} XrChain;

#define XR_LEADS 8
#define XR_LEAD_NONE (-1000)
#define XR_RESHOW_MAX 3     // re-shown refreshes a set carries images for (four refreshes a frame: the 30 FPS hold at 120 Hz)

typedef struct {
    bool valid;
    int pair;               // which pair of swapchains (g_xr.hud[pair], g_xr.eyes[pair])
    uint64_t seq;           // the timeline value of the submission holding its GPU work
    uint32_t hud;           // image indices
    uint32_t eye[2];
    bool has_eye[2];
    XrView views[2];        // the projection layer's (xr_set_views: the ones its eye images were rendered with)
    bool views_ok;
    XrSpace space;          // the app space the views were located in
    uint64_t made_ns;       // when it was submitted (stats)
    // (pose check) what the eye images were rendered with, as the stereo executor reports it; rendered: reported
    bool exec_eyes;         // the executor drew its eye images (xr_set_views was called)
    bool rendered;
    XrEyeView rview[2];
    uint64_t rid[2];        // the latch each eye's image came from (xr_latch_views_id)
    uint64_t latch_ns;      // plat_now_ns() of that latch (prediction: latch -> display)
    XrTime vtime;           // the display time its views were located for
    XrTime vref;            // the latch's vsync target then (predict_time: the grid line the prediction counts from)
    bool alpha;             // its eye images carry alpha for passthrough (edge.c; the Table Edge can change between sets)
    // (LARECOMP_XR_REPROJ, runtime/nvn/vulkan/reproj.c) the refreshes after its first, each with eye images of its own
    // drawn for that refresh's head pose: refresh k (1..rs_n) is due at grid0 + k periods. Each is its own submission
    // after the set's (rs_seq); the XR thread releases its images once that one is done, and a layer names them only then
    XrTime grid0;           // the display time of its first refresh (on the display's grid); 0: none due
    int64_t period;
    int rs_n;
    bool rs_ok[XR_RESHOW_MAX];        // both eyes recorded and submitted (rs_seq)
    bool rs_has[XR_RESHOW_MAX][2];    // acquired ([k][0] only when both eyes are layers of one image)
    bool rs_rel[XR_RESHOW_MAX];       // (XR thread) released: its submission is done, a layer may name it
    uint64_t rs_seq[XR_RESHOW_MAX];   // the submission drawing it
    uint64_t reads;                   // the last of those (they read its eye images): its own are released only once
                                      // the driver has it too (rec_submitted), so every read was submitted before
    uint32_t rs_img[XR_RESHOW_MAX][2];
    uint32_t rs_w[XR_RESHOW_MAX], rs_h[XR_RESHOW_MAX];  // the part drawn (LARECOMP_XR_REPROJ_RECT: smaller than the image)
    XrView rs_views[XR_RESHOW_MAX][2];
    XrTime ready;           // (XR thread) the first refresh it was found finished at (it may wait for grid0 then)
    // (LARECOMP_XR_BFI, XR thread) the refresh it is due to be lit on (grid0; else its first) and whether it has been
    XrTime bfi_slot;
    bool bfi_lit;
    uint64_t wait_ns;       // how long its xr_set_begin waited for the set before to be released
} XrSet;

// xr_latch_views_id's latches: the poses each frame's eyes were rendered with, kept until a set names them
#define XR_LATCHES 16
typedef struct {
    uint64_t id;            // 0: free
    XrView views[2];
    XrSpace space;
    XrTime time;            // the display time they were located for
    uint64_t made_ns;
    XrTime ref;             // the latest vsync's target time then (predict_time)
} XrLatch;

typedef struct {
    // instance / system
    void *loader;
    XrInstance inst;
    XrSystemId sys;
    bool has_alpha_blend;  // the runtime offers XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND (Table Edge Passthrough; xr_session.c)
    bool has_refresh_fb, has_format_list, has_gaze;  // has_gaze: XR_EXT_eye_gaze_interaction enabled (foveation)
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
    XrChain rs[2][XR_RESHOW_MAX][2];  // [pair][k][eye]: the re-shown refreshes' images (LARECOMP_XR_REPROJ), made on first use
    XrViewConfigurationView vcv[2];
    // the set hand-off (mu / cv)
    pthread_mutex_t mu;
    pthread_cond_t cv;
    XrSet building;         // the presenting thread's, between xr_set_begin and xr_set_submit (no lock: its own)
    XrLatch latch[XR_LATCHES];  // xr_latch_views_id (render thread); ring by id
    uint64_t latch_next;
    bool in_set;
    XrSet pending;          // submitted, not yet released (XR thread releases it)
    XrSet shown;            // the latest released: the layers name it
    bool shown_any;
    // the XR thread
    pthread_t thread;
    volatile bool stop;
    bool thread_on;
    _Atomic int64_t target_time;  // XrTime the latest vsync's game frame is predicted for
    _Atomic int64_t xr_minus_plat; // XrTime - plat_now_ns() (the clocks' offset, from the latest refresh)
    _Atomic int64_t lat_ns;        // smoothed latch -> first display of the sets (0: not measured yet)
    _Atomic int64_t grid_t, grid_period;  // the latest refresh's predicted display time and period (the display's grid)
    // (LARECOMP_XR_REPROJ rot / pos / follow) whole periods from a latch's vsync target to the first refresh its set could
    // be shown at: the latest of the last XR_LEADS sets' (XR_LEAD_NONE: too few yet). The XR thread's (xr_release_completed).
    _Atomic int lead;
    int8_t leads[XR_LEADS];
    int lead_n;
    // (LARECOMP_XR_BFI) a lit refresh's display time: the lit ones are a whole number of pairs of periods from it, and a
    // set's poses are predicted for one of them (predict_time). 0: none yet. The XR thread's
    _Atomic int64_t bfi_phase;
    int bfi_off;            // (lead_note) leads in a row whose refresh is off the phase
    int predict;                   // LARECOMP_XR_PREDICT: 1 measured (default), 0 vsync (the latest vsync's time)
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
bool xr_chain_create(XrChain *c, const char *what, uint32_t w, uint32_t h);  // (sRGB viewed as UNORM, one mip)
bool xr_chain_create_layers(XrChain *c, const char *what, uint32_t w, uint32_t h, uint32_t layers);
bool xr_chain_acquire(XrChain *c, uint32_t *index);
void xr_chain_release(XrChain *c);
// xr_loop.c
bool xr_thread_start(void);
void xr_thread_stop(void);
void xr_release_completed(XrTime pdt);  // (XR thread) the pending set, once its GPU work finished (pdt: this refresh's; 0 none)
// xr_vk.c: the layer's poses are the ones the set's eye images were rendered with (the pose check)
bool xr_layer_matches_render(const XrSet *s);
// xr_vk.c: one xrLocateViews for both eyes at display time t in `space` (false: no valid pose)
bool xr_locate_in(XrTime t, XrSpace space, XrView views[2]);
// xr_loop.c: Motion Clarity armed (LARECOMP_XR_BFI on, at a display rate that shows a 60 FPS game frame on two
// refreshes): the sets' poses predicted on the display's grid, on the lit refreshes' phase, and no redraws
bool xr_bfi_armed(void);
// (LARECOMP_XR_BFI) the display's period from its filtered rate: SteamVR now and then reports one period of two or three
// refreshes (hz_filter.h), which would make a step of one refresh look like none
static inline int64_t xr_bfi_period(void) { return g_xr.display_hz > 1 ? (int64_t)(1e9 / g_xr.display_hz) : 8333333; }
// xr_loop.c: the refreshes a game frame is shown on at the display's rate now (2 at 120 Hz, 4 in the 30 FPS hold; the
// larger count at an uneven rate)
int xr_refreshes_per_frame(void);

// record.c: the queue's external synchronisation
void vk_queue_lock(void);
void vk_queue_unlock(void);
uint64_t vk_completed_seq(void);  // (exec.c)
uint64_t rec_submitted(void);     // (record.c) the latest submission handed to the driver
