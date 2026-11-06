// Vulkan side of the mock OpenXR runtime (XR_KHR_vulkan_enable2), as seen by
// the core in xrmock.c.  Nothing here includes the OpenXR session logic.
#pragma once
#include <openxr/openxr.h>
#include <stddef.h>
#include <stdint.h>

int mockvk_owns_binding(XrStructureType type);
// Called with the session's graphics binding; writes a description for the trace.
int mockvk_describe_binding(const XrBaseInStructure* binding, char* out, size_t cap);
PFN_xrVoidFunction mockvk_get_proc(const char* name);
// Swapchain formats this device can really use (intersection of what SteamVR
// offers on the Frame and what the physical device supports).  Valid once the
// app's instance exists; resolved on first call after the physical device is known.
int mockvk_formats(const int64_t** list);
// Creates `count` images for a swapchain.  *err is 0 or an XrResult.
void* mockvk_create_images(const XrSwapchainCreateInfo* ci, int count, XrResult* err);
void mockvk_destroy_images(void* images);
XrResult mockvk_fill_images(void* images, int count, XrSwapchainImageBaseHeader* out);
// How long the app's queue took to go idle (ms).
double mockvk_release_wait_ms(void);
// Copies a rectangle of one image of a swapchain to an RGBA PNG.  The image
// is expected in the layout the app released it in
// (COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL).  Writes the
// mean colour into `stats`.  Returns 0 on success.
int mockvk_dump_png(void* images, int index, uint32_t layer, int x, int y, int w, int h, const char* path, char* stats, size_t statcap);
// The same image as RGBA8 in memory (malloc'd, the caller frees; NULL: failed).
uint8_t* mockvk_read_rgba(void* images, int index, uint32_t layer, int w, int h);
// Highest Vulkan API version offered (XR_MAKE_VERSION), default 1.3.
void mockvk_set_max_api(uint32_t major, uint32_t minor);
// MOCK_XR_SHIFT: an asynchronous copy of a released image's rect (layer `layer`) into a host buffer, with a tag;
// -1 when no probe slot is free or the format is not 8-bit colour.
int mockvk_probe_submit(void* images, int index, uint32_t layer, int x, int y, int w, int h, int tag);
// A finished probe, its luma box-averaged by `sub` into out (w/sub x h/sub); 1 when one was collected.
int mockvk_probe_collect(int* tag, int* w, int* h, float* out, int sub);
// MOCK_XR_COMPOSITOR: a compositor's GPU job on its own device, `lead_ms` before each vsync (t0 + n * period);
// returns the global priority it got (0: none), -1 on failure. Stop writes a summary line into out.
int mockvk_compositor_start(int64_t t0, int64_t period, double lead_ms, double from_s, int eye_w, int eye_h, const char* logpath);
void mockvk_compositor_stop(char* out, size_t cap);
