// The VR options of the settings menu's VR tab (runtime/ui/vr_tab.mm), kept in larecomp.ini (settings.c calls
// vr_settings_parse / vr_settings_save). Each option maps onto the variable the VR code already reads from the
// environment: at startup vr_settings_apply_env sets that variable from the saved value (only when the value is not the
// default and the variable is not already set: an environment variable always wins, and the tab shows the row greyed
// out). A change made in the menu reaches the running VR code through the hooks in vr_settings.c (weak symbols: an absent
// one, as in a flat build, leaves its row at "Applies After Restart"; Height, Size and Tilt are the rig's, vr_rig.ini). docs/af/VR_SETTINGS.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { VRS_LAYOUT_SWITCH, VRS_LAYOUT_COMFORT, VRS_N_LAYOUT };               // LARECOMP_XR_LAYOUT
enum { VRS_FOLLOW_SMOOTH, VRS_FOLLOW_ROOM, VRS_FOLLOW_FIXED, VRS_N_FOLLOW };  // LARECOMP_VR_FOLLOW
enum { VRS_EDGE_OFF, VRS_EDGE_FADE, VRS_EDGE_PASSTHROUGH, VRS_N_EDGE };       // LARECOMP_VR_EDGE = black, fade, passthrough
// LARECOMP_VR_FOVEATION: eye-tracked only (no fixed mode); without gaze foveation.c renders full density
enum { VRS_FOV_OFF, VRS_FOV_LOW, VRS_FOV_MEDIUM, VRS_FOV_HIGH, VRS_N_FOV };
enum { VRS_TURN_SMOOTH, VRS_TURN_SNAP, VRS_N_TURN };                         // LARECOMP_VR_TURN (the rig's right stick)
// AF_REFRESH_HZ / LARECOMP_XR_HZ / LARECOMP_XR_FPS: 120 Hz at 60 FPS (the default), 90 Hz at 30 FPS, 90 Hz at 60 FPS (a test)
enum { VRS_REFRESH_120, VRS_REFRESH_90, VRS_REFRESH_90_60, VRS_N_REFRESH };
enum { VRS_HAND_RIGHT, VRS_HAND_LEFT, VRS_N_HAND };                          // LARECOMP_VR_POINTER_HAND (the menu laser)
// LARECOMP_VR_SCENE = off, low, backdrop, full: the overworld past the table (vr_cull.h VR_SCENE_*, the same order)
enum { VRS_SCENE_OFF, VRS_SCENE_LOW, VRS_SCENE_BACKDROP, VRS_SCENE_FULL, VRS_N_SCENE };

typedef struct {
    int refresh;       // VRS_REFRESH_*: the headset's refresh and the game's rate (120 Hz: 60 FPS, 90 Hz: 30 FPS or, as a test, 60); a relaunch
    int layout;        // VRS_LAYOUT_*
    int follow;        // VRS_FOLLOW_*
    int height_cm;     // the table's height, cm from the default (the rig's: not saved here, read back by vr_settings_sync_rig)
    int dist_cm;       // the table's distance, cm from the head; the rig's, not saved here
    int size_pct;      // the table's size, percent of the default (0.067 m per game unit); the rig's, not saved here
    int tilt_deg;      // the table's tilt, degrees from the default view; the rig's, not saved here
    int turn;          // VRS_TURN_*
    int edge;          // VRS_EDGE_*
    int scene;         // VRS_SCENE_*: the world drawn past the table
    int foveation;     // VRS_FOV_*
    bool dynres;       // dynamic resolution of the eye images (output_scale.c)
    float hud_dist;    // the HUD / menu panel: metres from the head
    float hud_width;   // and its width in metres (LARECOMP_XR_QUAD=dist,width)
    int pointer_hand;  // VRS_HAND_*: the controller whose laser points at the menu (xr_input.c reads it live)
} VrSettings;

#define VR_DEFAULTS {.refresh = VRS_REFRESH_120, .layout = VRS_LAYOUT_SWITCH, .turn = VRS_TURN_SMOOTH, .follow = VRS_FOLLOW_SMOOTH, \
                     .height_cm = 0, .dist_cm = 120, .size_pct = 100, .tilt_deg = 30, .edge = VRS_EDGE_OFF, .scene = VRS_SCENE_OFF, .foveation = VRS_FOV_HIGH, .dynres = true, \
                     .hud_dist = 2.0f, .hud_width = 2.2f}

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VRO_REFRESH, VRO_LAYOUT, VRO_TURN, VRO_FOLLOW, VRO_HEIGHT, VRO_DIST, VRO_SIZE, VRO_TILT, VRO_RECENTRE, VRO_EDGE, VRO_SCENE, VRO_FOVEATION, VRO_DYNRES,
    VRO_HUD_DIST, VRO_HUD_WIDTH, VRO_POINTER_HAND, VRO_COUNT
} VrOpt;

// the choices' names for the menu, and their larecomp.ini words
extern const char *const g_vr_refresh_names[VRS_N_REFRESH], *const g_vr_layout_names[VRS_N_LAYOUT],
    *const g_vr_follow_names[VRS_N_FOLLOW], *const g_vr_edge_names[VRS_N_EDGE], *const g_vr_scene_names[VRS_N_SCENE], *const g_vr_fov_names[VRS_N_FOV],
    *const g_vr_turn_names[VRS_N_TURN], *const g_vr_hand_names[VRS_N_HAND];

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
// The tab's Height / Size / Tilt rows show the rig's live values (xr_rig_get_user, weak: a flat build keeps the defaults).
void vr_settings_sync_rig(void);
// The Recentre Table button.
void vr_settings_recentre(void);
// (test hook, once per game frame: hle/warp.c) LARECOMP_VR_SET_AT=<frame>:<key>=<value>[,<frame>:<key>=<value>...]: at
// that game frame, the change as the VR tab makes it (g_settings.vr, the ini saved, vr_settings_changed). Keys: the ini's
// without vr_ (edge_style, surroundings, foveation, dynamic_resolution, follow, layout, turn, panel_distance, panel_width, refresh, fps)
// and edge, dynres, label (Buttons Match Labels, 0 / 1); values as in the ini. Frames in increasing order.
void vr_settings_test_tick(uint64_t frame);

#ifdef __cplusplus
}
#endif
