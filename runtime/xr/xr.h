// The VR mode's core (runtime/xr): OpenXR session, Vulkan through the runtime (XR_KHR_vulkan_enable2), eye and HUD
// swapchains, and the XR thread that paces the display (xrWaitFrame / xrBeginFrame / xrEndFrame) and drives the
// game's vsync. On with LARECOMP_XR=1 (Linux builds with the OpenXR headers: steam-frame/fetch-openxr.sh); off, none
// of this runs and the flat path is unchanged.
//
// Threads:
//   presenting thread  (the start screen's main thread, or the render thread in game) makes a *set*: the HUD image
//                      (what the window showed in flat mode: the start screen, or the game's image and the menus) and,
//                      when it renders them, the two eye images. device.c vk_frame_begin / vk_frame_end call
//                      xr_set_begin / xr_set_submit around it.
//   XR thread          (xr_loop.c) every display refresh: xrWaitFrame, the vsync to the game (two refreshes of three at
//                      90 Hz, every second at 120 Hz: 60 Hz either way), xrBeginFrame, the release of the newest set
//                      once the GPU has finished it, xrEndFrame with the projection layer (eyes) and the quad (HUD).
//   recorder thread    (record.c) the queue's owner; vk_queue_lock() keeps it out while the runtime uses the queue
//                      (xrBeginFrame, xrEndFrame, xrAcquireSwapchainImage, xrReleaseSwapchainImage).
//
// Rules carried over from GalaxyFrame (Steam Frame lessons):
//   - a set is released only once vk_completed_seq() has passed its submission: SteamVR shows a released image at
//     once, finished or not;
//   - one image waited per swapchain at a time: the next set is acquired only after the last one was released
//     (xr_set_begin waits for that: back-pressure, at most one set ahead of the display);
//   - both eyes of a set come from one xrLocateViews (xr_latch_views), and those poses go into the projection layer
//     for every refresh the set is shown;
//   - swapchains have one mip level; images are in COLOR_ATTACHMENT_OPTIMAL when acquired and when released;
//   - the GPU is drained before the session goes (exit faults otherwise);
//   - no layer names a swapchain that has an image acquired at xrEndFrame: every swapchain exists twice (a pair of
//     HUD + two eyes), alternate sets use alternate pairs, and the layers name the pair of the set shown.
//
// Image formats: the swapchains are sRGB (the compositor decodes them) made with MUTABLE_FORMAT, and the views handed
// out are the UNORM twin: LAS writes sRGB-encoded bytes into UNORM targets (the window's swapchain was UNORM), so the
// bytes are the flat ones and the headset shows the colours the flat window did.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// LARECOMP_XR=1 (read once). Everything below is for XR mode only.
bool xr_wanted(void);
// The session exists (xr_start succeeded) and the XR thread runs.
bool xr_active(void);
// This thread is inside an OpenXR call that may use our device (xrCreateSession, swapchain calls, xrBeginFrame,
// xrEndFrame): SteamVR makes Vulkan objects of its own on our device there - shader modules declaring
// ShaderViewportIndexLayerEXT, fragment density map images - and device.c's validation callback tells their messages
// apart from ours.
bool xr_in_runtime_call(void);

// ---- device.c: instance and device through the runtime (keeping LAS's own create infos, pNext chains intact)
// xrCreateInstance + xrGetSystem + the runtime's Vulkan requirements, then xrCreateVulkanInstanceKHR(ci).
// Returns the VkResult (VK_ERROR_INITIALIZATION_FAILED when the OpenXR side failed: logged).
VkResult xr_vk_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out);
VkPhysicalDevice xr_vk_physical_device(VkInstance inst);  // xrGetVulkanGraphicsDevice2KHR (NULL: failed, logged)
VkResult xr_vk_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, VkDevice *out);
// After the device and its queue (queue family qfam, index 0; before the recorder thread starts): the session, the two
// HUD swapchains of hud_w x hud_h, the eye swapchains (the runtime's recommended size x LARECOMP_XR_EYE_SCALE), and the
// XR thread. hud: both HUD swapchains' images and UNORM views, the first pair's then the second's (device.c puts them
// where the window's swapchain images were).
typedef struct {
    VkFormat format;      // the views' format (UNORM)
    VkExtent2D extent;
    uint32_t count;
    VkImage images[8];
    VkImageView views[8];
} XrChainInfo;
bool xr_start(VkQueue queue, uint32_t qfam, uint32_t hud_w, uint32_t hud_h, XrChainInfo *hud);
// Quitting (device.c backend_quit, after the recorder stopped and the GPU idled): the XR thread stops, the session
// ends, swapchains / spaces / session / instance go.
void xr_shutdown(void);

// ---- the presenting thread: one set per presented frame (device.c vk_frame_begin / vk_frame_end)
// Waits until the previous set was released (at most timeout_ns: false then, nothing acquired - skip the frame), then
// acquires and waits the HUD image of the pair not shown. *hud_index: its index in XrChainInfo.images.
bool xr_set_begin(uint64_t timeout_ns, uint32_t *hud_index);
// The submission with timeline value seq holds all of this set's GPU work (queued: the recorder may not have made it
// yet). Latches the views if nobody did (xr_latch_views), and hands the set to the XR thread.
void xr_set_submit(uint64_t seq);

// ---- eyes (lane 4: the stereo executor renders the scene into these)
// Between xr_set_begin and xr_set_submit, on the presenting thread (or the render thread that executes the frame, as
// long as calls do not overlap). Acquires and waits eye `eye` (0 left, 1 right) of the current set once; later calls
// in the same set return the same image. The image is in COLOR_ATTACHMENT_OPTIMAL and must be left in it. Returns
// false outside a set.
typedef struct {
    VkImage image;
    VkImageView view;   // UNORM view (write sRGB-encoded bytes, as into LAS's own targets)
    VkFormat format;    // view format
    VkExtent2D extent;
} XrEyeTarget;
bool xr_eye_target(int eye, XrEyeTarget *out);
bool xr_eye_used(int eye);  // this set acquired eye `eye` (device.c clears the eyes nobody drew: the stub)
// One xrLocateViews for both eyes at the display time the XR thread predicted for the game frame now starting (the
// time it published with the latest vsync, two refreshes ahead), in the app's space. The first call in a set latches
// them; later calls in the same set return the latched ones. These go into the projection layer for this set.
typedef struct {
    float orientation[4];  // x y z w, the eye in the app's space (y up, -z ahead; metres)
    float position[3];
    float fov[4];          // angleLeft, angleRight, angleUp, angleDown (radians; left / down negative)
} XrEyeView;
bool xr_latch_views(XrEyeView out[2]);
// The predicted display time (XrTime, ns) of the latest vsync's game frame, and the display's refresh (Hz).
int64_t xr_target_display_time(void);
float xr_display_hz(void);
// The stub eye colour (linear 0..1, written as is into the UNORM view) for eye `eye` (LARECOMP_XR_EYE_TEST=1: left
// blue, right red, to check which image each eye sees).
void xr_stub_eye_colour(int eye, float rgba[4]);

#ifdef __cplusplus
}
#endif
