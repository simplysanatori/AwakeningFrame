// The VR options of larecomp.ini and their way into the VR code (vr_settings.h; docs/af/VR_SETTINGS.md).
#include "vr_settings.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rt.h"
#include "settings.h"

#define VR g_settings.vr

const char *const g_vr_refresh_names[VRS_N_REFRESH] = {"120 Hz (60 FPS)", "90 Hz (30 FPS)", "90 Hz (60 FPS, Test)"};
const char *const g_vr_layout_names[VRS_N_LAYOUT] = {"Switch", "Comfort"};
const char *const g_vr_follow_names[VRS_N_FOLLOW] = {"Smooth", "Room", "Fixed"};
const char *const g_vr_edge_names[VRS_N_EDGE] = {"Off", "Fade", "Passthrough"};
const char *const g_vr_scene_names[VRS_N_SCENE] = {"Off", "Low Quality", "Backdrop", "Full"};
const char *const g_vr_fov_names[VRS_N_FOV] = {"Off", "Low", "Medium", "High"};
const char *const g_vr_turn_names[VRS_N_TURN] = {"Smooth", "Snap"};
const char *const g_vr_hand_names[VRS_N_HAND] = {"Right", "Left"};

// larecomp.ini words; the environment variables take the same ones (LARECOMP_VR_EDGE=fade, ...). The edge's words are the
// variable's: off is "black"; the variable's older "fog" and "gradient" stay usable from the environment but are not in the menu.
static const char *const k_layout[] = {"switch", "comfort"};
static const char *const k_turn[] = {"smooth", "snap"};
static const char *const k_follow[] = {"smooth", "room", "fixed"};
static const char *const k_edge[] = {"black", "fade", "passthrough"};
static const char *const k_scene[] = {"off", "low", "backdrop", "full"};
static const char *const k_fov[] = {"off", "low", "medium", "high"};
static const char *const k_hand[] = {"right", "left"};

// ---------------------------------------------------------------------------------------------------------------------
// The hooks. Each is the function that changes one option while the game runs (weak: an absent one, as in a flat
// build, leaves the row at "Applies After Restart", and nothing breaks). Where they live: the rig (runtime/xr/xr_rig.h),
// edge.c, foveation.c, output_scale.c, xr_input.c and stereo.c.
// Height, size and tilt belong to the rig (runtime/xr/xr_rig.h, vr_rig.ini): the three xr_rig_set_* hooks are provided by
// runtime/xr/xr_rig_settings.c (XR builds only), the tab reads the live values back through xr_rig_get_user, and
// larecomp.ini no longer keeps them (the old vr_table_* keys are read and ignored).
#define WEAK __attribute__((weak))
#include "xr/xr_rig.h"  // XrRigUser, the rig's ranges (the functions below are weak: a flat build has none)
extern void xr_rig_get_user(XrRigUser *u) WEAK;
extern void stereo_set_follow(int mode) WEAK;             // VRS_FOLLOW_*: stereo.c cfg.follow
extern void xr_rig_set_height_cm(float cm) WEAK;          // table height: cm above / below the default (negative: lower)
extern void xr_rig_set_size_pct(float pct) WEAK;          // table size: percent of the default scale
extern void xr_rig_set_tilt_deg(float deg) WEAK;          // table tilt: degrees from the default view (positive: toward the player)
extern void xr_rig_recentre(void) WEAK;                   // put the table in front of the player again (xr_rig.c)
extern void xr_rig_set_turn_mode(int snap) WEAK;          // VRS_TURN_SNAP: 1, VRS_TURN_SMOOTH: 0 (xr_rig_settings.c)
extern void edge_set_mode(int mode) WEAK;                 // VRS_EDGE_*: edge.c (the mode, read per eye)
extern void vr_scene_set_mode(int mode) WEAK;             // VRS_SCENE_*: nvn/vr_cull.c (the culling box and ring draws, read per frame)
extern void fov_set_level(int level) WEAK;                // VRS_FOV_*: foveation.c
extern void diet_set_dynres(int on) WEAK;                 // the dynamic eye resolution (output_scale.c)
extern void xr_input_set_layout(int comfort) WEAK;        // xr_input.c S.comfort (VRS_LAYOUT_COMFORT: 1)
extern void xr_set_game_fps(int fps) WEAK;                // the game's rate at the display's: 60 (the 90 Hz test) or 0 (xr_loop.c)
extern float xr_display_hz(void) WEAK;                    // the display's rate now (0: no session yet)
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
    // vr_refresh and vr_fps together make the Refresh Rate choice (in either order): 120 Hz, 90 Hz at 30, 90 Hz at 60
    static int ini_hz = 120, ini_fps = 30;
    if (!strcmp(key, "refresh") || !strcmp(key, "fps")) {
        if (key[0] == 'r') ini_hz = atoi(val) == 90 ? 90 : 120;
        else ini_fps = atoi(val) == 60 ? 60 : 30;
        VR.refresh = ini_hz == 120 ? VRS_REFRESH_120 : ini_fps == 60 ? VRS_REFRESH_90_60 : VRS_REFRESH_90;
    }
    else if (!strcmp(key, "layout")) VR.layout = pick(k_layout, VRS_N_LAYOUT, val, VRS_LAYOUT_SWITCH);
    else if (!strcmp(key, "turn")) VR.turn = pick(k_turn, VRS_N_TURN, val, VRS_TURN_SMOOTH);
    else if (!strcmp(key, "follow")) VR.follow = pick(k_follow, VRS_N_FOLLOW, val, VRS_FOLLOW_SMOOTH);
    else if (!strcmp(key, "table_height") || !strcmp(key, "table_size") || !strcmp(key, "table_tilt"))
        ;  // old keys: the rig keeps these now (vr_rig.ini); read and dropped
    else if (!strcmp(key, "edge_style")) VR.edge = pick(k_edge, VRS_N_EDGE, val, VRS_EDGE_OFF);
    else if (!strcmp(key, "edge_mode") || !strcmp(key, "edge") || !strcmp(key, "culling"))
        ;  // old keys (haze / sky edge choices, the Culling row): read and dropped, so what was saved with them does not carry over
    else if (!strcmp(key, "surroundings")) VR.scene = pick(k_scene, VRS_N_SCENE, val, VRS_SCENE_OFF);
    else if (!strcmp(key, "foveation")) VR.foveation = pick(k_fov, VRS_N_FOV, val, VRS_FOV_HIGH);
    else if (!strcmp(key, "dynamic_resolution")) VR.dynres = atoi(val) != 0;
    else if (!strcmp(key, "panel_distance")) VR.hud_dist = clampf((float)atof(val), 0.8f, 4.0f);
    else if (!strcmp(key, "panel_width")) VR.hud_width = clampf((float)atof(val), 0.6f, 4.0f);
    else if (!strcmp(key, "pointer_hand")) VR.pointer_hand = pick(k_hand, VRS_N_HAND, val, VRS_HAND_RIGHT);
    else return false;
    return true;
}

void vr_settings_save(FILE *f) {
    // (the wrapper, steam-frame/awakeningframe.sh, reads the digits of these two lines)
    fprintf(f, "vr_refresh = %d\nvr_fps = %d\n", VR.refresh == VRS_REFRESH_120 ? 120 : 90, VR.refresh == VRS_REFRESH_90 ? 30 : 60);
    fprintf(f, "vr_layout = %s\nvr_turn = %s\n", k_layout[VR.layout], k_turn[VR.turn]);
    fprintf(f, "vr_follow = %s\n", k_follow[VR.follow]);
    fprintf(f, "vr_edge_style = %s\n", k_edge[VR.edge]);
    fprintf(f, "vr_surroundings = %s\n", k_scene[VR.scene]);
    fprintf(f, "vr_foveation = %s\n", k_fov[VR.foveation]);
    fprintf(f, "vr_dynamic_resolution = %d\n", VR.dynres);
    fprintf(f, "vr_panel_distance = %.2f\nvr_panel_width = %.2f\n", VR.hud_dist, VR.hud_width);
    fprintf(f, "vr_pointer_hand = %s\n", k_hand[VR.pointer_hand]);
}

// ---------------------------------------------------------------------------------------------------------------------
static bool g_given[VRO_COUNT];  // the variable was in the environment before settings were applied
static bool g_fps_given;         // LARECOMP_XR_FPS was (session.env's AF_FPS through the wrapper): the game's rate is the environment's
static const char *const k_env[VRO_COUNT] = {
    [VRO_REFRESH] = "LARECOMP_XR_HZ", [VRO_LAYOUT] = "LARECOMP_XR_LAYOUT", [VRO_TURN] = "LARECOMP_VR_TURN", [VRO_FOLLOW] = "LARECOMP_VR_FOLLOW",
    // (height, size and tilt: the rig's, vr_rig.ini; LARECOMP_VR_TABLE / _PITCH / _SCALE are its base and the player's values
    // come on top, so the environment does not grey these rows)
    [VRO_HEIGHT] = "", [VRO_SIZE] = "", [VRO_TILT] = "",
    [VRO_RECENTRE] = "", [VRO_EDGE] = "LARECOMP_VR_EDGE", [VRO_SCENE] = "LARECOMP_VR_SCENE", [VRO_FOVEATION] = "LARECOMP_VR_FOVEATION",
    [VRO_DYNRES] = "LARECOMP_VR_DYNRES", [VRO_HUD_DIST] = "LARECOMP_XR_QUAD",
    [VRO_HUD_WIDTH] = "LARECOMP_XR_QUAD", [VRO_POINTER_HAND] = "LARECOMP_VR_POINTER_HAND"};

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
    g_fps_given = getenv("LARECOMP_XR_FPS") != NULL;
    // (a value is passed on only when it is not what the code behind the variable does without it: refresh 120, layout
    // switch, turn smooth, follow smooth, edge off; foveation and dynamic resolution are passed on always, for the code's own
    // defaults (medium, off) are not the tab's (high, on))
    char b[64];
    // (120 Hz is what the code behind LARECOMP_XR_HZ does without it, and the tab's default; 90 Hz is passed on)
    if (!g_given[VRO_REFRESH] && VR.refresh != VRS_REFRESH_120) put("LARECOMP_XR_HZ", "90");
    // (the test: 60 fps at 90 Hz; the environment's LARECOMP_XR_FPS, or session.env's AF_FPS through the wrapper, wins)
    if (!getenv("LARECOMP_XR_FPS") && VR.refresh == VRS_REFRESH_90_60) put("LARECOMP_XR_FPS", "60");
    if (!g_given[VRO_LAYOUT] && VR.layout != VRS_LAYOUT_SWITCH) put("LARECOMP_XR_LAYOUT", k_layout[VR.layout]);
    if (!g_given[VRO_TURN] && VR.turn != VRS_TURN_SMOOTH) put("LARECOMP_VR_TURN", k_turn[VR.turn]);
    if (!g_given[VRO_FOLLOW] && VR.follow != VRS_FOLLOW_SMOOTH) put("LARECOMP_VR_FOLLOW", k_follow[VR.follow]);
    if (!g_given[VRO_EDGE] && VR.edge != VRS_EDGE_OFF) put("LARECOMP_VR_EDGE", k_edge[VR.edge]);
    if (!g_given[VRO_SCENE] && VR.scene != VRS_SCENE_OFF) put("LARECOMP_VR_SCENE", k_scene[VR.scene]);
    if (!g_given[VRO_FOVEATION]) put("LARECOMP_VR_FOVEATION", k_fov[VR.foveation]);
    if (!g_given[VRO_DYNRES]) put("LARECOMP_VR_DYNRES", VR.dynres ? "1" : "0");
    if (!g_given[VRO_HUD_DIST] && (VR.hud_dist != 2.0f || VR.hud_width != 2.2f)) {
        snprintf(b, sizeof b, "%.2f,%.2f", VR.hud_dist, VR.hud_width);
        put("LARECOMP_XR_QUAD", b);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The rig's live height / size / tilt into the rows' values (the tab calls this each time it draws, so a change made with
// the controller shows). Without the rig (a flat build) the rows keep their defaults.
void vr_settings_sync_rig(void) {
    if (!xr_rig_get_user) return;
    XrRigUser u;
    xr_rig_get_user(&u);
    VR.height_cm = clampi((int)lroundf(u.height * 100.0f), (int)lroundf(XR_RIG_HEIGHT_MIN * 100.0f), (int)lroundf(XR_RIG_HEIGHT_MAX * 100.0f));
    VR.size_pct = clampi((int)lroundf(u.size * 100.0f), (int)lroundf(XR_RIG_SIZE_MIN * 100.0f), (int)lroundf(XR_RIG_SIZE_MAX * 100.0f));
    VR.tilt_deg = clampi((int)lroundf(u.tilt), (int)XR_RIG_TILT_MIN, (int)XR_RIG_TILT_MAX);
}

bool vr_opt_live(VrOpt o) {
    switch (o) {
    case VRO_REFRESH: {
        // SteamVR's per-app rate is set by the launch wrapper (the runtime refuses xrRequestDisplayRefreshRateFB): a change of
        // the display's rate needs a relaunch. The game's rate at the display's rate is ours: between the two 90 Hz choices
        // (30 FPS, 60 FPS test) the change is live, when the display runs at 90 Hz now and LARECOMP_XR_FPS isn't set.
        float hz = xr_display_hz ? xr_display_hz() : 0;
        int want = VR.refresh == VRS_REFRESH_120 ? 120 : 90;
        return xr_set_game_fps && !g_fps_given && hz > 0 && fabsf(hz - (float)want) < 2.0f && want == 90;
    }
    case VRO_LAYOUT: return xr_input_set_layout != NULL;
    case VRO_TURN: return xr_rig_set_turn_mode != NULL;
    case VRO_FOLLOW: return stereo_set_follow != NULL;
    case VRO_HEIGHT: return xr_rig_set_height_cm != NULL;
    case VRO_SIZE: return xr_rig_set_size_pct != NULL;
    case VRO_TILT: return xr_rig_set_tilt_deg != NULL;
    case VRO_RECENTRE: return xr_rig_recentre != NULL;
    case VRO_EDGE: return edge_set_mode != NULL;
    case VRO_SCENE: return vr_scene_set_mode != NULL;
    case VRO_FOVEATION: return fov_set_level != NULL;
    case VRO_DYNRES: return diet_set_dynres != NULL;
    case VRO_HUD_DIST: case VRO_HUD_WIDTH: return vrui_set_hud_quad != NULL;
    case VRO_POINTER_HAND: return true;  // (xr_input.c reads g_settings.vr.pointer_hand each refresh)
    default: return false;
    }
}

void vr_settings_changed(VrOpt o) {
    if (vr_opt_overridden(o)) return;  // (greyed out in the tab: can't get here, but the environment wins)
    switch (o) {
    case VRO_REFRESH: if (vr_opt_live(o)) xr_set_game_fps(VR.refresh == VRS_REFRESH_90_60 ? 60 : 0); break;
    case VRO_LAYOUT: if (xr_input_set_layout) xr_input_set_layout(VR.layout == VRS_LAYOUT_COMFORT); break;
    case VRO_TURN: if (xr_rig_set_turn_mode) xr_rig_set_turn_mode(VR.turn == VRS_TURN_SNAP); break;
    case VRO_FOLLOW: if (stereo_set_follow) stereo_set_follow(VR.follow); break;
    case VRO_HEIGHT: if (xr_rig_set_height_cm) xr_rig_set_height_cm((float)VR.height_cm); break;
    case VRO_SIZE: if (xr_rig_set_size_pct) xr_rig_set_size_pct((float)VR.size_pct); break;
    case VRO_TILT: if (xr_rig_set_tilt_deg) xr_rig_set_tilt_deg((float)VR.tilt_deg); break;
    case VRO_EDGE: if (edge_set_mode) edge_set_mode(VR.edge); break;
    case VRO_SCENE: if (vr_scene_set_mode) vr_scene_set_mode(VR.scene); break;
    case VRO_FOVEATION: if (fov_set_level) fov_set_level(VR.foveation); break;
    case VRO_DYNRES: if (diet_set_dynres) diet_set_dynres(VR.dynres); break;
    case VRO_HUD_DIST: case VRO_HUD_WIDTH: if (vrui_set_hud_quad) vrui_set_hud_quad(VR.hud_dist, VR.hud_width); break;
    default: break;
    }
}

void vr_settings_recentre(void) {
    if (xr_rig_recentre) xr_rig_recentre();
}

// ---------------------------------------------------------------------------------------------------------------------
// LARECOMP_VR_SET_AT (vr_settings.h): a scripted run's way through the same path as the tab.
void vr_settings_test_tick(uint64_t frame) {
    static const char *next;
    static bool init;
    if (!init) init = true, next = getenv("LARECOMP_VR_SET_AT");
    while (next && *next) {
        long at;
        char key[40], val[40], k2[48];
        if (sscanf(next, "%ld:%39[^=]=%39[^,]", &at, key, val) != 3) { next = NULL; return; }
        if ((long)frame < at) return;
        next = strchr(next, ',');
        if (next) next++;
        const char *k = !strcmp(key, "edge") ? "edge_style" : !strcmp(key, "dynres") ? "dynamic_resolution" : key;
        VrOpt o = VRO_COUNT;
        if (!strcmp(key, "label")) {
            g_settings.face_by_label = atoi(val) != 0;
            LOG("[vr settings] test: frame %llu label=%s\n", (unsigned long long)frame, val);
            settings_save();
            continue;
        }
        static const struct { const char *k; VrOpt o; } map[] = {
            {"edge_style", VRO_EDGE}, {"surroundings", VRO_SCENE}, {"foveation", VRO_FOVEATION}, {"dynamic_resolution", VRO_DYNRES}, {"follow", VRO_FOLLOW},
            {"layout", VRO_LAYOUT}, {"turn", VRO_TURN}, {"panel_distance", VRO_HUD_DIST}, {"panel_width", VRO_HUD_WIDTH},
            {"refresh", VRO_REFRESH}, {"fps", VRO_REFRESH}};
        for (size_t i = 0; i < sizeof map / sizeof *map; i++)
            if (!strcmp(k, map[i].k)) o = map[i].o;
        snprintf(k2, sizeof k2, "vr_%s", k);
        if (o == VRO_COUNT || !vr_settings_parse(k2, val)) {
            LOG("[vr settings] test: frame %llu %s=%s unknown\n", (unsigned long long)frame, key, val);
            continue;
        }
        LOG("[vr settings] test: frame %llu %s=%s (%s)\n", (unsigned long long)frame, k, val,
                vr_opt_overridden(o) ? "set by the environment: ignored" : vr_opt_live(o) ? "live" : "after a restart");
        settings_save();
        vr_settings_changed(o);
    }
}
