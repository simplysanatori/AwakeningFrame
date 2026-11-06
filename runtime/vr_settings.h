// The VR options of the settings menu's VR tab (runtime/ui/vr_tab.mm), kept in larecomp.ini (settings.c calls
// vr_settings_parse / vr_settings_save). Each option maps onto the variable the VR code already reads from the
// environment: at startup vr_settings_apply_env sets that variable from the saved value (only when the value is not the
// default and the variable is not already set: an environment variable always wins, and the tab shows the row greyed
// out). A change made in the menu reaches the running VR code through the hooks in vr_settings.c (weak symbols: a lane
// that has not landed yet leaves its row at "Applies After Restart"). docs/af/VR_SETTINGS.md.
#pragma once
#include <stdbool.h>
#include <stdio.h>

enum { VRS_LAYOUT_SWITCH, VRS_LAYOUT_COMFORT, VRS_N_LAYOUT };               // LARECOMP_XR_LAYOUT
enum { VRS_FOLLOW_SMOOTH, VRS_FOLLOW_ROOM, VRS_FOLLOW_FIXED, VRS_N_FOLLOW };  // LARECOMP_VR_FOLLOW
enum { VRS_EDGE_FOG, VRS_EDGE_GRADIENT, VRS_EDGE_BLACK, VRS_EDGE_PASSTHROUGH, VRS_N_EDGE };  // LARECOMP_VR_EDGE
// LARECOMP_VR_FOVEATION (VRS_FOV_DEFAULT: the variable is left alone, the foveation lane's own default)
enum { VRS_FOV_DEFAULT, VRS_FOV_OFF, VRS_FOV_LOW, VRS_FOV_MEDIUM, VRS_FOV_HIGH, VRS_FOV_FIXED, VRS_N_FOV };
enum { VRS_CULL_FIT, VRS_CULL_WIDE, VRS_CULL_STOCK, VRS_N_CULL };            // LARECOMP_VR_CULL
enum { VRS_REFRESH_120, VRS_REFRESH_90, VRS_N_REFRESH };                    // AF_REFRESH_HZ / LARECOMP_XR_HZ (60 / 30 FPS)

typedef struct {
    int refresh;       // VRS_REFRESH_*: the headset's refresh and the game's rate (120 Hz: 60 FPS, 90 Hz: 30 FPS); a relaunch
    int layout;        // VRS_LAYOUT_*
    int follow;        // VRS_FOLLOW_*
    int height_cm;     // the table's height, centimetres from where the table starts (rig lane)
    int size_pct;      // the table's size, percent of the default (LARECOMP_VR_SCALE 0.067 m per game unit)
    int tilt_deg;      // the table's tilt, degrees from the default view (rig lane)
    int edge;          // VRS_EDGE_*
    int foveation;     // VRS_FOV_*
    bool dynres;       // dynamic resolution of the eye images (diet lane)
    int cull;          // VRS_CULL_*
    float hud_dist;    // the HUD / menu panel: metres from the head
    float hud_width;   // and its width in metres (LARECOMP_XR_QUAD=dist,width)
} VrSettings;

#define VR_DEFAULTS {.refresh = VRS_REFRESH_120, .layout = VRS_LAYOUT_SWITCH, .follow = VRS_FOLLOW_SMOOTH, .height_cm = 0, \
                     .size_pct = 100, .tilt_deg = 0, .edge = VRS_EDGE_FOG, .foveation = VRS_FOV_DEFAULT, .dynres = true, \
                     .cull = VRS_CULL_FIT, .hud_dist = 2.0f, .hud_width = 2.2f}

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VRO_REFRESH, VRO_LAYOUT, VRO_FOLLOW, VRO_HEIGHT, VRO_SIZE, VRO_TILT, VRO_RECENTRE, VRO_EDGE, VRO_FOVEATION, VRO_DYNRES,
    VRO_CULL, VRO_HUD_DIST, VRO_HUD_WIDTH, VRO_COUNT
} VrOpt;

// the choices' names for the menu, and their larecomp.ini words
extern const char *const g_vr_refresh_names[VRS_N_REFRESH], *const g_vr_layout_names[VRS_N_LAYOUT],
    *const g_vr_follow_names[VRS_N_FOLLOW], *const g_vr_edge_names[VRS_N_EDGE], *const g_vr_fov_names[VRS_N_FOV],
    *const g_vr_cull_names[VRS_N_CULL];

// settings.c: one ini line (true: it was a VR key) / the VR keys
bool vr_settings_parse(const char *key, const char *val);
void vr_settings_save(FILE *f);
// After the ini is read, before anything reads the environment: the saved values into the variables the VR code reads
// (see the header comment). Also notes which variables the environment already had.
void vr_settings_apply_env(void);

// The tab: the variable that sets this option from the environment ("" none), whether it was already set when the
// program started (the row is then greyed out: the environment wins), and whether a change reaches the running VR code
// (else: after a restart / relaunch).
const char *vr_opt_env(VrOpt o);
bool vr_opt_overridden(VrOpt o);
bool vr_opt_live(VrOpt o);
// A change made in the menu (g_settings.vr already holds the new value): to the running VR code.
void vr_settings_changed(VrOpt o);
// The Recentre Table button.
void vr_settings_recentre(void);

#ifdef __cplusplus
}
#endif
