// The VR tab's hook into the HUD / menu quad (runtime/vr_settings.c): xr_loop.c reads g_xr.quad_dist / quad_width for
// every frame it ends, so a change made in the menu shows at once. A separate file, so the XR files stay as they are.
#include "xr_internal.h"
#include "../rt.h"

void vrui_set_hud_quad(float dist_m, float width_m) {
    if (dist_m > 0) g_xr.quad_dist = dist_m;
    if (width_m > 0) g_xr.quad_width = width_m;
    LOG("[xr] HUD panel (the VR tab): %.1f m wide at %.1f m\n", g_xr.quad_width, g_xr.quad_dist);
}
