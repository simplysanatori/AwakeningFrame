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
//   - both eyes of a set come from one xrLocateViews, the one their images were rendered with (xr_latch_views_id /
//     xr_set_views: the executor keeps the latch with each eye output, so an eye image drawn again into a later set
//     takes its old poses along), and those poses go into the projection layer for every refresh the set is shown;
//     the [xr] 5 s line counts the refreshes whose layer pose was not the render pose ("pose mismatches");
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
// (the stereo executor) One xrLocateViews for a game frame being executed before its set exists (the render thread
// translates a frame's command lists before its present opens the set), kept under the returned *id (the last 16 are
// kept). Located for the display time that frame is expected at (LARECOMP_XR_PREDICT: measured latch -> display,
// default; vsync: the latest vsync's display time). false: no session / no valid pose.
bool xr_latch_views_id(XrEyeView out[2], uint64_t *id);
// Between xr_set_begin and xr_set_submit: the projection layer of this set gets the views of the latches its eye images
// were rendered with (eye i's pose from latch id[i]; both ids are the same unless the eyes come from different frames),
// for every refresh it is shown. rendered: the views the eye cameras were built with, as the executor kept them (the
// pose check: compared with the latch's). Returns -1 when a latch is gone (the set then gets a fresh locate, counted
// as a mismatch), else a mask: 1 / 2 eye 0 / 1 differs from what it was rendered with, 4 the eyes come from different
// latches.
int xr_set_views(const uint64_t id[2], const XrEyeView rendered[2]);
// The eye swapchains' size (false before xr_start).
bool xr_eye_size(uint32_t *w, uint32_t *h);
// (lane 4) GPU ms of eye 0 and eye 1 of the latest finished game frame: xr_timing.log's gpu0 / gpu1 (-1: unknown).
void xr_note_eye_gpu(float ms0, float ms1);
// The predicted display time (XrTime, ns) of the latest vsync's game frame, and the display's refresh (Hz).
int64_t xr_target_display_time(void);
float xr_display_hz(void);
// The stub eye colour (linear 0..1, written as is into the UNORM view) for eye `eye` (LARECOMP_XR_EYE_TEST=1: left
// blue, right red, to check which image each eye sees).
void xr_stub_eye_colour(int eye, float rgba[4]);

// ---- controllers (lane 5: runtime/xr/xr_input.c; any thread)
// Link walks with the left stick, rotated by the diorama's yaw so that "up" walks him away from the player. The rig
// calls this whenever its yaw changes (default 0; LARECOMP_XR_YAW=<degrees> sets the first value, for tests): the
// rotation of the table about the vertical axis, counter-clockwise seen from above (T_table's Rot_y), taking the game's
// x to the table's x and the game's "up" on screen to the table's -z; at 0 up is up, and with the table turned 90
// degrees counter-clockwise (its "up" now points to the player's left) the stick has to be pushed left to walk up.
void xr_input_set_yaw(float yaw_radians);
// What the rig reads from the controllers (the game gets everything else; none of these reach the game):
//   right stick left / right    xr_input_take_snap(): -1 left, +1 right, 0 none. One per flick past 0.7, re-armed once
//                               the stick is back under 0.35. Consumed by the call (only the latest flick is kept).
//   right stick up / down       XrRigInput.tilt: +1 up .. -1 down, dead zone and all (continuous: the rig integrates)
//   left grip held (modifier)   XrRigInput.modifier; with it the left D-pad is the rig's, not the game's:
//                               up / down -> height +1 / -1, right / left -> size +1 / -1 (held: continuous)
//                               and a click of the right stick -> xr_input_take_recentre() (put the table in front of
//                               the player again), consumed by the call.
typedef struct {
    float tilt;
    float height, size;
    bool modifier;
} XrRigInput;
void xr_input_rig(XrRigInput *out);
int xr_input_take_snap(void);
bool xr_input_take_recentre(void);

// (the rig, runtime/xr/xr_rig.c) the app's space put at the head and turned to where it faces, as after SteamVR's own
// recentre (xr_session.c: the next refresh makes the new space; the table, placed relative to it, is in front again).
void xr_recentre(void);

#ifdef __cplusplus
}
#endif
