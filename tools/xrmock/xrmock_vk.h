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
// Highest Vulkan API version offered (XR_MAKE_VERSION), default 1.3.
void mockvk_set_max_api(uint32_t major, uint32_t minor);
