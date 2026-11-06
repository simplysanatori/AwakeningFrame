// VR tab (VR mode, or LARECOMP_SHOW_VR_TAB=1): the VR options of runtime/vr_settings.h, saved in larecomp.ini.
// An option set by an environment variable (LARECOMP_VR_EDGE, ...) is greyed out and says so; one whose change can't reach
// the running VR code yet says "Applies After Restart" (a relaunch for the refresh rate). docs/af/VR_SETTINGS.md.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "i18n.h"
#include "option_rows.h"

extern "C" {
#include "../settings.h"
#include "../vr_settings.h"
bool xr_wanted(void) __attribute__((weak));  // (VR builds only)
}

extern "C" bool vr_tab_visible(void) {
    static int force = -1;
    if (force < 0) force = getenv("LARECOMP_SHOW_VR_TAB") && atoi(getenv("LARECOMP_SHOW_VR_TAB")) != 0;
    return force || (xr_wanted && xr_wanted());
}

// The row's description with how a change takes effect, or why the row can't be changed: the text goes through L() as a
// whole, so a translation of the base text alone isn't used (the VR tab is English for now, like the rest of its text).
static const char *desc(VrOpt o, const char *base) {
    static char buf[4][512];
    static int n;
    char *b = buf[n++ & 3];
    if (vr_opt_overridden(o))
        snprintf(b, 512, "%s %s %s", L(base), L("Set by"), vr_opt_env(o));
    else if (!vr_opt_live(o))
        snprintf(b, 512, "%s %s", L(base), o == VRO_REFRESH ? L("Applies After Relaunch.") : L("Applies After Restart."));
    else
        return base;
    if (vr_opt_overridden(o)) snprintf(b + strlen(b), 512 - strlen(b), " %s", L("in the launch environment."));
    return b;
}
// (the row after is greyed out when the environment sets it)
static void grey(VrOpt o) {
    if (vr_opt_overridden(o)) opt_disable_next();
}
static void changed(VrOpt o) {
    settings_save();
    vr_settings_changed(o);
}

extern "C" void vr_tab_draw(float scale) {
    VrSettings &v = g_settings.vr;
    vr_settings_sync_rig();  // Height, Size and Tilt are the rig's: a controller change shows here
    opt_begin("##vr", scale);

    opt_section("Display");
    grey(VRO_REFRESH);
    if (opt_choice("Refresh Rate", &v.refresh, g_vr_refresh_names, VRS_N_REFRESH, VRS_REFRESH_90,
                   desc(VRO_REFRESH, v.refresh == VRS_REFRESH_90_60
                                         ? "Plays at 60 FPS on a 90 Hz display. Motion is less even because frames alternate "
                                           "between one and two refreshes. For testing."
                                         : "The headset's refresh rate, and with it the game's frame rate: 90 Hz plays at 30 FPS "
                                           "and asks less of the GPU.")))
        changed(VRO_REFRESH);

    opt_section("Controls");
    grey(VRO_LAYOUT);
    if (opt_choice("Controller Layout", &v.layout, g_vr_layout_names, VRS_N_LAYOUT, VRS_LAYOUT_SWITCH,
                   desc(VRO_LAYOUT, "Switch puts every button where a Switch Pro Controller has it. Comfort puts the sword "
                                    "on the right trigger and the shield on the left.")))
        changed(VRO_LAYOUT);
    // (the Controls tab's setting: xr_input.c reads it live)
    opt_toggle("Buttons Match Labels", &g_settings.face_by_label, false,
               "The Frame controllers' A, B, X and Y work as printed on them. Off matches the Switch's button positions.");

    opt_section("Table");
    grey(VRO_FOLLOW);
    if (opt_choice("Follow Mode", &v.follow, g_vr_follow_names, VRS_N_FOLLOW, VRS_FOLLOW_SMOOTH,
                   desc(VRO_FOLLOW, "How the table follows the game's camera. Smooth glides after it, Room holds still until "
                                    "the camera changes room, Fixed never moves.")))
        changed(VRO_FOLLOW);
    grey(VRO_HEIGHT);
    if (opt_int("Height", &v.height_cm, -50, 40, 5, 0, "%+d cm", desc(VRO_HEIGHT, "How high the table stands.")))
        changed(VRO_HEIGHT);
    grey(VRO_SIZE);
    if (opt_int("Size", &v.size_pct, 40, 300, 10, 100, "%d%%", desc(VRO_SIZE, "How big the table is.")))
        changed(VRO_SIZE);
    grey(VRO_TILT);
    if (opt_int("Tilt", &v.tilt_deg, -20, 50, 5, 0, "%+d\xC2\xB0", desc(VRO_TILT, "How far the table leans toward you.")))
        changed(VRO_TILT);
    if (!vr_opt_live(VRO_RECENTRE)) opt_disable_next();
    if (opt_button("Recentre Table", "Puts the table in front of you again.")) vr_settings_recentre();

    opt_section("Edge");
    grey(VRO_EDGE);
    if (opt_choice("Table Edge", &v.edge, g_vr_edge_names, VRS_N_EDGE, VRS_EDGE_OFF,
                   desc(VRO_EDGE, "What fills the empty space past the world outdoors. Haze fades it into the sky, Sky Only "
                                  "fills the space without touching the world, Passthrough shows the room.")))
        changed(VRO_EDGE);

    opt_section("Performance");
    grey(VRO_FOVEATION);
    if (opt_choice("Foveation", &v.foveation, g_vr_fov_names, VRS_N_FOV, VRS_FOV_MEDIUM,
                   desc(VRO_FOVEATION, "Follows your eyes and draws what you aren't looking at in less detail. Higher levels save "
                                       "more GPU time. Without eye tracking it draws everything in full detail.")))
        changed(VRO_FOVEATION);
    grey(VRO_DYNRES);
    if (opt_toggle("Dynamic Resolution", &v.dynres, false,
                   desc(VRO_DYNRES, "Lowers the resolution when the GPU can't keep up and raises it again when it can.")))
        changed(VRO_DYNRES);
    grey(VRO_CULL);
    if (opt_choice("Culling", &v.cull, g_vr_cull_names, VRS_N_CULL, VRS_CULL_FIT,
                   desc(VRO_CULL, "How much of the world outside the table is skipped. Fit follows the table's edges, Wide "
                                  "keeps more, Stock keeps everything the game would draw.")))
        changed(VRO_CULL);

    opt_section("Menu Panel");
    {
        bool over = vr_opt_overridden(VRO_HUD_DIST);
        if (over) opt_disable_next();
        if (opt_slider("Panel Distance", &v.hud_dist, 0.8f, 4.0f, 0.1f, 2.0f, "%.1f m",
                       desc(VRO_HUD_DIST, "How far away the HUD and menus float.")))
            changed(VRO_HUD_DIST);
        if (over) opt_disable_next();
        if (opt_slider("Panel Width", &v.hud_width, 0.6f, 4.0f, 0.1f, 2.2f, "%.1f m",
                       desc(VRO_HUD_WIDTH, "How wide the HUD and menus are.")))
            changed(VRO_HUD_WIDTH);
    }

    opt_end();
}
