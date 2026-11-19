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

// LARECOMP_XR=1 (read once; a PC VR package: unless LARECOMP_XR=0). Everything below is for XR mode only.
bool xr_wanted(void);
// main.c: --vr / --flat into LARECOMP_XR (first thing), then a PC VR package's start (once the data folder is known:
// the window when there are no game files yet or no OpenXR runtime with a headset; docs/af/PCVR.md). game_dir: the
// game files' folder (exefs/, romfs/). true: VR with the game code built, so no start screen (the Frame's --play).
void xr_args(int argc, char **argv);
bool xr_pc_start(const char *game_dir);
// The session exists (xr_start succeeded) and the XR thread runs.
bool xr_active(void);
// Between xrBeginSession and xrEndSession (any thread).
bool xr_session_running(void);
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

// ---- eyes (the stereo executor renders the scene into these)
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
// (foveation) the eye gaze (unit direction in the app space of latch `id`, at its display time): false when there is
// none (no eye tracker / extension, not tracked) or the latch is gone.
bool xr_latch_gaze(uint64_t id, float dir[3]);
// Between xr_set_begin and xr_set_submit: the projection layer of this set gets the views of the latches its eye images
// were rendered with (eye i's pose from latch id[i]; both ids are the same unless the eyes come from different frames),
// for every refresh it is shown. rendered: the views the eye cameras were built with, as the executor kept them (the
// pose check: compared with the latch's). Returns -1 when a latch is gone (the set then gets a fresh locate, counted
// as a mismatch), else a mask: 1 / 2 eye 0 / 1 differs from what it was rendered with, 4 the eyes come from different
// latches.
int xr_set_views(const uint64_t id[2], const XrEyeView rendered[2]);
// ---- the refreshes that show a set again (LARECOMP_XR_REPROJ; runtime/nvn/vulkan/reproj.c, docs/af/STEREO.md
// "Re-shown refreshes"). At 60 FPS on a 120 Hz display a set is shown on two refreshes (four in the 30 FPS hold).
//   off     (default) every refresh shows the set's eye images with the poses they were rendered with
//   fresh   (a test) the refreshes after a set's first carry a pose located for that refresh, on the same images: a
//           compositor that reprojects by the layer's pose then pins them to the head; one that does not, changes nothing
//   rot / pos / follow  the refreshes after the first get eye images of their own, drawn at the present from the set's
//           for the head pose of each (rotation; and position from the scene depth; and the table's follow motion),
//           each in a submission of its own after the set's
enum { XR_REPROJ_OFF, XR_REPROJ_FRESH, XR_REPROJ_ROT, XR_REPROJ_POS, XR_REPROJ_FOLLOW };
int xr_reproj_mode(void);  // (XR_REPROJ_OFF while Motion Clarity is armed: those refreshes show nothing)
void xr_set_reproj_mode(int mode);  // (the VR tab; any thread: the next set)
// ---- Motion Clarity (LARECOMP_XR_BFI; docs/af/STEREO.md "Motion Clarity"): at 120 Hz with the game at 60 FPS each set
// is shown on one refresh, the one its poses were located for, and the projection layer is left out of the other (black;
// the HUD and the laser stay). Off by default; elsewhere (90 Hz, the 30 FPS hold, other rates) it waits, as off.
void xr_set_bfi(int on);  // (the VR tab; any thread: the next refresh)
// 0 off, 1 flashing, 2 waiting for 120 Hz with the game at 60 (another rate, the 30 FPS lock), 3 waiting while the game
// runs at 30 (the pacer's hold, or sets missing on most lit refreshes), 4 flashing where in-between frames are made
// (LARECOMP_XR_INBETWEEN: the refreshes with one are lit too, so none is dark while they come)
int xr_bfi_state(void);
// LARECOMP_XR_BFI_GAIN=<g> (off): the eye images' brightness lift while it flashes (0: none; the present / edge pass's pf.z)
float xr_bfi_gain(void);
// Between xr_set_begin and xr_set_submit, after the eye images and their views (xr_set_views): the display times of the
// refreshes this set is expected on (times[0] its first, on the display's grid; then one a refresh, as many as the game's
// cadence shows a frame, up to 4). Returns how many (0: none: no views, the session not running).
int xr_reshow_times(int64_t times[4]);
// The views for refresh k (1..3) of those, from one xrLocateViews at times[k] in the set's space.
bool xr_reshow_views(int k, XrEyeView out[2]);
// Eye `eye`'s image for refresh k (its own swapchains, made on the first call for k: the memory of 2 pairs x 2 eyes).
// eye -1: both eyes as the two layers of one image (LARECOMP_XR_REPROJ_MV); the view is then a 2D_ARRAY view.
bool xr_reshow_target(int k, int eye, XrEyeTarget *out);
// Both eyes of refresh k are recorded into submission `seq` (one after the set's own: device.c vk_frame_end, before
// xr_set_submit), drawn into the top left w x h of their images. Once that submission is done the layer shows them
// for refresh k with the views of xr_reshow_views; until then the newest finished image of the set stays up.
void xr_reshow_ready(int k, uint64_t seq, uint32_t w, uint32_t h);
// ---- in-between images (LARECOMP_XR_INBETWEEN; runtime/nvn/vulkan/inbetween.c, docs/af/STEREO.md "In-between
// frames"): the same refreshes, drawn after xr_set_submit from the frame's lists run again for each. mode 1: the set's
// own views are located for its first refresh on the display's grid and the XR thread holds a set finished early, as it
// does for rot / pos / follow; 2: the same, and the set's first refresh too gets an in-between (its own images only
// where one is missing): its grid0 is then a refresh before its first, never shown; 0: neither (any thread).
void xr_set_grid(int mode);
// After xr_set_submit, on the presenting thread: the display times of the set submitted last (times[0] its grid0: its
// first refresh, or with *own_hidden the refresh before it) and when its views were located (plat_now_ns; also
// without times, 0 if none). Returns how many (0: none: no views, no grid); refresh k (1..) of them gets in-between k.
int xr_ib_due(int64_t times[4], uint64_t *latch_ns, bool *own_hidden);
uint64_t xr_plat_time(int64_t t);  // an XrTime on plat_now_ns's clock
uint64_t xr_ib_fell(void);  // so far: refreshes whose drawn image (re-shown or in-between) was not finished in time
// The views for refresh k (1..3) of that set, from one xrLocateViews at times[k] in its space, as a latch of their own
// (*id: xr_latch_gaze).
bool xr_ib_views(int k, XrEyeView out[2], uint64_t *id);
// Eye `eye`'s image for refresh k of that set (the re-shown refreshes' swapchains).
bool xr_ib_target(int k, int eye, XrEyeTarget *out);
// Both eyes of refresh k are recorded into submission `seq`, drawn into the top left w x h of their images: they go to
// the set, pending or shown, and are shown as xr_reshow_ready's are. False: the set was replaced meanwhile, or an eye is
// missing or w is 0 (nothing drawn): the images went back unused.
bool xr_ib_ready(int k, uint64_t seq, uint32_t w, uint32_t h);
// The latch's display time (the one its views were located for), 0 when it is gone.
int64_t xr_latch_time(uint64_t id);
// The eye swapchains' size (false before xr_start).
bool xr_eye_size(uint32_t *w, uint32_t *h);
// GPU ms of eye 0 and eye 1 of the latest finished game frame: xr_timing.log's gpu0 / gpu1 (-1: unknown).
void xr_note_eye_gpu(float ms0, float ms1);
// The predicted display time (XrTime, ns) of the latest vsync's game frame, and the display's refresh (Hz).
int64_t xr_target_display_time(void);
float xr_display_hz(void);
// The stub eye colour (linear 0..1, written as is into the UNORM view) for eye `eye` (LARECOMP_XR_EYE_TEST=1: left
// blue, right red, to check which image each eye sees).
void xr_stub_eye_colour(int eye, float rgba[4]);

// ---- controllers (runtime/xr/xr_input.c; any thread)
// Link walks with the left stick, rotated by the diorama's yaw so that "up" walks him away from the player. The rig
// calls this whenever its yaw changes (default 0; LARECOMP_XR_YAW=<degrees> sets the first value, for tests): the
// rotation of the table about the vertical axis, counter-clockwise seen from above (T_table's Rot_y), taking the game's
// x to the table's x and the game's "up" on screen to the table's -z; at 0 up is up, and with the table turned 90
// degrees counter-clockwise (its "up" now points to the player's left) the stick has to be pushed left to walk up.
void xr_input_set_yaw(float yaw_radians);
// What the rig reads from the controllers (the game gets everything else; none of these reach the game):
//   right stick left / right    XrRigInput.turn: +1 right .. -1 left, while sideways is the stick's dominant axis (smooth
//                               turning: the rig integrates), and xr_input_take_snap(): -1 left, +1 right, 0 none, one per
//                               flick past 0.7 (re-armed once the stick is back under 0.35; snap turning). Consumed by the
//                               call (only the latest flick is kept).
//   right stick up / down       XrRigInput.tilt: +1 up .. -1 down, while up / down is the dominant axis (continuous: the
//                               rig integrates). The dominant axis is the one the stick was first pushed along, until the
//                               other is 1.4 times bigger; the other reads 0.
//   left grip held (modifier)   XrRigInput.modifier; with it the left D-pad is the rig's, not the game's:
//                               up / down -> height +1 / -1, right / left -> size +1 / -1 (held: continuous)
//   both stick clicks held 0.1 s, or the left grip and a click of the right stick: xr_input_take_reset_view() (the view back
//                               to the game's own angle and the table in front of the player again), consumed by the call.
//                               Until both clicks are let go the game sees neither; a single click (or both for under 0.1 s)
//                               goes to the game as the stick-click buttons. The same hold, 2 s from the press:
//                               xr_input_take_reset_full() (the view reset and tilt, height, size and distance back to the
//                               defaults), once per hold, consumed by the call.
typedef struct {
    float tilt, turn;
    float height, size;
    bool modifier;
} XrRigInput;
void xr_input_rig(XrRigInput *out);
int xr_input_take_snap(void);
bool xr_input_take_reset_view(void);
bool xr_input_take_reset_full(void);

// (the rig, runtime/xr/xr_rig.c) the app's space put at the head and turned to where it faces, as after SteamVR's own
// recentre (xr_session.c: the next refresh makes the new space; the table, placed relative to it, is in front again).
void xr_recentre(void);

#ifdef __cplusplus
}
#endif
