// The VR options of larecomp.ini and their way into the VR code (vr_settings.h; docs/af/VR_SETTINGS.md).
#include "vr_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "settings.h"

#define VR g_settings.vr

const char *const g_vr_refresh_names[VRS_N_REFRESH] = {"120 Hz (60 FPS)", "90 Hz (30 FPS)"};
const char *const g_vr_layout_names[VRS_N_LAYOUT] = {"Switch", "Comfort"};
const char *const g_vr_follow_names[VRS_N_FOLLOW] = {"Smooth", "Room", "Fixed"};
const char *const g_vr_edge_names[VRS_N_EDGE] = {"Fog", "Gradient", "Black", "Passthrough"};
const char *const g_vr_fov_names[VRS_N_FOV] = {"Off", "Low", "Medium", "High"};
const char *const g_vr_cull_names[VRS_N_CULL] = {"Fit", "Wide", "Stock"};

// larecomp.ini words; the environment variables take the same ones (LARECOMP_VR_EDGE=fog, ...)
static const char *const k_layout[] = {"switch", "comfort"};
static const char *const k_follow[] = {"smooth", "room", "fixed"};
static const char *const k_edge[] = {"fog", "gradient", "black", "passthrough"};
static const char *const k_fov[] = {"off", "low", "medium", "high"};
static const char *const k_cull[] = {"fit", "wide", "stock"};

// ---------------------------------------------------------------------------------------------------------------------
// TODO(overseer): the hooks. Each is the function a lane provides to change its option while the game runs (weak: an
// absent one leaves the row at "Applies After Restart", and nothing breaks). Names are this tab's proposal; connect them
// to what the lanes ended up calling theirs (rig: runtime/xr/xr_rig.h, edge: edge.c, fov, diet, vr_cull.c, xr_input.c,
// stereo.c) - either by providing these names or by changing the calls in vr_settings_changed below.
// The rig reads g_settings.vr.height_cm / tilt_deg / size_pct itself when it starts (height and tilt have no environment
// variable; size also goes through LARECOMP_VR_SCALE).
#define WEAK __attribute__((weak))
extern void stereo_set_follow(int mode) WEAK;             // VRS_FOLLOW_*: stereo.c cfg.follow
extern void xr_rig_set_height_cm(float cm) WEAK;          // table height: cm above / below the default (negative: lower)
extern void xr_rig_set_size_pct(float pct) WEAK;          // table size: percent of the default scale
extern void xr_rig_set_tilt_deg(float deg) WEAK;          // table tilt: degrees from the default view (positive: toward the player)
extern void xr_rig_recentre(void) WEAK;                   // put the table in front of the player again
extern void edge_set_mode(int mode) WEAK;                 // VRS_EDGE_*: edge.c cfg.mode
extern void fov_set_level(int level) WEAK;                // VRS_FOV_*: the foveation lane
extern void diet_set_dynres(int on) WEAK;                 // the diet lane's dynamic eye resolution
extern void vr_cull_set_mode(int mode) WEAK;              // VRS_CULL_*: vr_cull.c C.mode
extern void xr_input_set_layout(int comfort) WEAK;        // xr_input.c S.comfort (VRS_LAYOUT_COMFORT: 1)
// Provided here, in runtime/xr/vrui_hook.c (XR builds only): the HUD / menu panel's place, read by xr_loop.c every frame.
extern void vrui_set_hud_quad(float dist_m, float width_m) WEAK;

// ---------------------------------------------------------------------------------------------------------------------
static int pick(const char *const *keys, int n, const char *v, int def) {
    for (int i = 0; i < n; i++)
        if (!strcasecmp(v, keys[i])) return i;
    return def;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

bool vr_settings_parse(const char *key, const char *val) {
    if (strncmp(key, "vr_", 3)) return false;
    key += 3;
    if (!strcmp(key, "refresh")) VR.refresh = atoi(val) == 90 ? VRS_REFRESH_90 : VRS_REFRESH_120;
    else if (!strcmp(key, "layout")) VR.layout = pick(k_layout, VRS_N_LAYOUT, val, VRS_LAYOUT_SWITCH);
    else if (!strcmp(key, "follow")) VR.follow = pick(k_follow, VRS_N_FOLLOW, val, VRS_FOLLOW_SMOOTH);
    else if (!strcmp(key, "table_height")) VR.height_cm = clampi(atoi(val), -50, 50);
    else if (!strcmp(key, "table_size")) VR.size_pct = clampi(atoi(val), 50, 200);
    else if (!strcmp(key, "table_tilt")) VR.tilt_deg = clampi(atoi(val), -30, 30);
    else if (!strcmp(key, "edge")) VR.edge = pick(k_edge, VRS_N_EDGE, val, VRS_EDGE_FOG);
    else if (!strcmp(key, "foveation")) VR.foveation = pick(k_fov, VRS_N_FOV, val, VRS_FOV_MEDIUM);
    else if (!strcmp(key, "dynamic_resolution")) VR.dynres = atoi(val) != 0;
    else if (!strcmp(key, "culling")) VR.cull = pick(k_cull, VRS_N_CULL, val, VRS_CULL_FIT);
    else if (!strcmp(key, "panel_distance")) VR.hud_dist = clampf((float)atof(val), 0.8f, 4.0f);
    else if (!strcmp(key, "panel_width")) VR.hud_width = clampf((float)atof(val), 0.6f, 4.0f);
    else return false;
    return true;
}

void vr_settings_save(FILE *f) {
    fprintf(f, "vr_refresh = %d\n", VR.refresh == VRS_REFRESH_90 ? 90 : 120);
    fprintf(f, "vr_layout = %s\n", k_layout[VR.layout]);
    fprintf(f, "vr_follow = %s\n", k_follow[VR.follow]);
    fprintf(f, "vr_table_height = %d\nvr_table_size = %d\nvr_table_tilt = %d\n", VR.height_cm, VR.size_pct, VR.tilt_deg);
    fprintf(f, "vr_edge = %s\n", k_edge[VR.edge]);
    fprintf(f, "vr_foveation = %s\n", k_fov[VR.foveation]);
    fprintf(f, "vr_dynamic_resolution = %d\n", VR.dynres);
    fprintf(f, "vr_culling = %s\n", k_cull[VR.cull]);
    fprintf(f, "vr_panel_distance = %.2f\nvr_panel_width = %.2f\n", VR.hud_dist, VR.hud_width);
}

// ---------------------------------------------------------------------------------------------------------------------
static bool g_given[VRO_COUNT];  // the variable was in the environment before settings were applied
static const char *const k_env[VRO_COUNT] = {
    [VRO_REFRESH] = "LARECOMP_XR_HZ", [VRO_LAYOUT] = "LARECOMP_XR_LAYOUT", [VRO_FOLLOW] = "LARECOMP_VR_FOLLOW",
    [VRO_HEIGHT] = "LARECOMP_VR_TABLE", [VRO_SIZE] = "LARECOMP_VR_SCALE", [VRO_TILT] = "LARECOMP_VR_PITCH",
    [VRO_RECENTRE] = "", [VRO_EDGE] = "LARECOMP_VR_EDGE", [VRO_FOVEATION] = "LARECOMP_VR_FOVEATION",
    [VRO_DYNRES] = "LARECOMP_VR_DYNRES", [VRO_CULL] = "LARECOMP_VR_CULL", [VRO_HUD_DIST] = "LARECOMP_XR_QUAD",
    [VRO_HUD_WIDTH] = "LARECOMP_XR_QUAD"};

const char *vr_opt_env(VrOpt o) {
    // (the refresh rate is taken by the launch wrapper from AF_REFRESH_HZ before larecomp.ini)
    if (o == VRO_REFRESH) return "AF_REFRESH_HZ";
    return k_env[o];
}

bool vr_opt_overridden(VrOpt o) {
    if (o == VRO_REFRESH) {  // the wrapper says where it took the rate from; without it (a dev run): LARECOMP_XR_HZ
        const char *s = getenv("AF_REFRESH_FROM");
        return s ? !strcmp(s, "env") : g_given[o];
    }
    return g_given[o];
}

// (stderr: the program's log isn't open yet when the settings load)
static void put(const char *var, const char *val) {
    if (setenv(var, val, 0) == 0) fprintf(stderr, "[vr settings] %s=%s (larecomp.ini)\n", var, val);
}

void vr_settings_apply_env(void) {
    static bool done;
    if (done) return;
    done = true;
    for (int i = 0; i < VRO_COUNT; i++) g_given[i] = k_env[i][0] && getenv(k_env[i]);
    // (only values that are not the default: the default is whatever the code behind the variable does without it)
    char b[64];
    if (!g_given[VRO_REFRESH] && VR.refresh == VRS_REFRESH_90) put("LARECOMP_XR_HZ", "90");
    if (!g_given[VRO_LAYOUT] && VR.layout != VRS_LAYOUT_SWITCH) put("LARECOMP_XR_LAYOUT", k_layout[VR.layout]);
    if (!g_given[VRO_FOLLOW] && VR.follow != VRS_FOLLOW_SMOOTH) put("LARECOMP_VR_FOLLOW", k_follow[VR.follow]);
    if (!g_given[VRO_SIZE] && VR.size_pct != 100) {  // 0.067 m per game unit (stereo.c)
        snprintf(b, sizeof b, "%.5f", 0.067 * VR.size_pct / 100.0);
        put("LARECOMP_VR_SCALE", b);
    }
    if (!g_given[VRO_EDGE] && VR.edge != VRS_EDGE_FOG) put("LARECOMP_VR_EDGE", k_edge[VR.edge]);
    if (!g_given[VRO_FOVEATION] && VR.foveation != VRS_FOV_MEDIUM) put("LARECOMP_VR_FOVEATION", k_fov[VR.foveation]);
    if (!g_given[VRO_DYNRES] && VR.dynres) put("LARECOMP_VR_DYNRES", "1");  // off by default (owner), so only On is passed on
    if (!g_given[VRO_CULL] && VR.cull != VRS_CULL_FIT) put("LARECOMP_VR_CULL", k_cull[VR.cull]);
    if (!g_given[VRO_HUD_DIST] && (VR.hud_dist != 2.0f || VR.hud_width != 2.2f)) {
        snprintf(b, sizeof b, "%.2f,%.2f", VR.hud_dist, VR.hud_width);
        put("LARECOMP_XR_QUAD", b);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
bool vr_opt_live(VrOpt o) {
    switch (o) {
    case VRO_REFRESH: return false;  // SteamVR's per-app rate is set by the launch wrapper: a relaunch
    case VRO_LAYOUT: return xr_input_set_layout != NULL;
    case VRO_FOLLOW: return stereo_set_follow != NULL;
    case VRO_HEIGHT: return xr_rig_set_height_cm != NULL;
    case VRO_SIZE: return xr_rig_set_size_pct != NULL;
    case VRO_TILT: return xr_rig_set_tilt_deg != NULL;
    case VRO_RECENTRE: return xr_rig_recentre != NULL;
    case VRO_EDGE: return edge_set_mode != NULL;
    case VRO_FOVEATION: return fov_set_level != NULL;
    case VRO_DYNRES: return diet_set_dynres != NULL;
    case VRO_CULL: return vr_cull_set_mode != NULL;
    case VRO_HUD_DIST: case VRO_HUD_WIDTH: return vrui_set_hud_quad != NULL;
    default: return false;
    }
}

void vr_settings_changed(VrOpt o) {
    if (vr_opt_overridden(o)) return;  // (greyed out in the tab: can't get here, but the environment wins)
    switch (o) {
    case VRO_LAYOUT: if (xr_input_set_layout) xr_input_set_layout(VR.layout == VRS_LAYOUT_COMFORT); break;
    case VRO_FOLLOW: if (stereo_set_follow) stereo_set_follow(VR.follow); break;
    case VRO_HEIGHT: if (xr_rig_set_height_cm) xr_rig_set_height_cm((float)VR.height_cm); break;
    case VRO_SIZE: if (xr_rig_set_size_pct) xr_rig_set_size_pct((float)VR.size_pct); break;
    case VRO_TILT: if (xr_rig_set_tilt_deg) xr_rig_set_tilt_deg((float)VR.tilt_deg); break;
    case VRO_EDGE: if (edge_set_mode) edge_set_mode(VR.edge); break;
    case VRO_FOVEATION: if (fov_set_level) fov_set_level(VR.foveation); break;
    case VRO_DYNRES: if (diet_set_dynres) diet_set_dynres(VR.dynres); break;
    case VRO_CULL: if (vr_cull_set_mode) vr_cull_set_mode(VR.cull); break;
    case VRO_HUD_DIST: case VRO_HUD_WIDTH: if (vrui_set_hud_quad) vrui_set_hud_quad(VR.hud_dist, VR.hud_width); break;
    default: break;
    }
}

void vr_settings_recentre(void) {
    if (xr_rig_recentre) xr_rig_recentre();
}
