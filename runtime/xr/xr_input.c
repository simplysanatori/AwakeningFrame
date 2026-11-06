// The Frame controllers in VR mode: one action set, its bindings (the Steam Frame's own profile, XR_VALVE_frame_controller
// _interaction; the Touch profile as a fallback for other headsets), the sync once per display refresh, and the hand-off
// of the result to the game (runtime/hle/input.c input_xr_set, as the buttons and the left stick of a gamepad) and to
// the rig (xr.h: snap turn, tilt, height / size, recentre). The game's rumble (input_rumble_level) goes to both
// controllers as short haptic pulses renewed while it lasts (GalaxyFrame's scheme: 120 ms pulses every 80 ms, stopped
// at once when the rumble ends, so a runtime that ignores a pulse's duration can't leave a controller running).
// Reference: GalaxyFrame's platform/src/xr/xr_app.cpp (initActions, suggestFrame, updateInput).
//
// The layout is in docs/af/CONTROLS.md (with the game's meaning of each Switch button) and in k_map below.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../input_bind.h"
#include "../rt.h"
#include "xr_input.h"

// ---- the layout: which Frame control is which gamepad button (SDL names, Switch positions: the east button is A)
// Each control may drive several; the player's own controller bindings (Controls tab) turn them into game inputs.
enum { PADB_ZL = PADB_LTRIGGER, PADB_ZR = PADB_RTRIGGER };
typedef enum { C_A, C_B, C_X, C_Y, C_MENU, C_VIEW, C_DUP, C_DDOWN, C_DLEFT, C_DRIGHT, C_BUMP_L, C_BUMP_R, C_CLICK_L, C_CLICK_R,
               C_TRIG_L, C_TRIG_R, C_GRIP_L, C_GRIP_R, C_COUNT } Ctl;
static const struct { Ctl ctl; int host; } k_map[] = {
    {C_A, SDL_GAMEPAD_BUTTON_EAST},   {C_B, SDL_GAMEPAD_BUTTON_SOUTH},        {C_X, SDL_GAMEPAD_BUTTON_NORTH},
    {C_Y, SDL_GAMEPAD_BUTTON_WEST},   {C_MENU, SDL_GAMEPAD_BUTTON_START},      {C_VIEW, SDL_GAMEPAD_BUTTON_BACK},
    {C_DUP, SDL_GAMEPAD_BUTTON_DPAD_UP},     {C_DDOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {C_DLEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {C_DRIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {C_TRIG_R, SDL_GAMEPAD_BUTTON_SOUTH}, {C_TRIG_L, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {C_BUMP_R, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {C_BUMP_L, PADB_ZL},
    {C_GRIP_R, SDL_GAMEPAD_BUTTON_EAST},
    {C_CLICK_L, SDL_GAMEPAD_BUTTON_LEFT_STICK}, {C_CLICK_R, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
};

// ---- the actions
typedef enum { T_BOOL, T_FLOAT, T_VEC2, T_HAPTIC } Kind;
enum { A_A, A_B, A_X, A_Y, A_MENU, A_VIEW, A_DUP, A_DDOWN, A_DLEFT, A_DRIGHT, A_BUMP_L, A_BUMP_R, A_CLICK_L, A_CLICK_R,
       A_TRIG_L, A_TRIG_R, A_GRIP_L, A_GRIP_R, A_STICK_L, A_STICK_R, A_HAPTIC, A_COUNT };
#define L_ "/user/hand/left/"
#define R_ "/user/hand/right/"
static const struct { const char *name, *label; Kind kind; const char *frame, *touch; } k_act[A_COUNT] = {
    [A_A] = {"button_a", "A", T_BOOL, R_ "input/a/click", R_ "input/a/click"},
    [A_B] = {"button_b", "B", T_BOOL, R_ "input/b/click", R_ "input/b/click"},
    [A_X] = {"button_x", "X", T_BOOL, R_ "input/x/click", L_ "input/x/click"},
    [A_Y] = {"button_y", "Y", T_BOOL, R_ "input/y/click", L_ "input/y/click"},
    [A_MENU] = {"menu", "Menu", T_BOOL, R_ "input/menu/click", L_ "input/menu/click"},
    [A_VIEW] = {"view", "View", T_BOOL, L_ "input/view/click", NULL},
    [A_DUP] = {"dpad_up", "D-pad Up", T_BOOL, L_ "input/dpad_up/click", NULL},
    [A_DDOWN] = {"dpad_down", "D-pad Down", T_BOOL, L_ "input/dpad_down/click", NULL},
    [A_DLEFT] = {"dpad_left", "D-pad Left", T_BOOL, L_ "input/dpad_left/click", NULL},
    [A_DRIGHT] = {"dpad_right", "D-pad Right", T_BOOL, L_ "input/dpad_right/click", NULL},
    [A_BUMP_L] = {"bumper_left", "Left Bumper", T_BOOL, L_ "input/bumper/click", NULL},
    [A_BUMP_R] = {"bumper_right", "Right Bumper", T_BOOL, R_ "input/bumper/click", NULL},
    [A_CLICK_L] = {"stick_click_left", "Left Stick Click", T_BOOL, L_ "input/thumbstick/click", L_ "input/thumbstick/click"},
    [A_CLICK_R] = {"stick_click_right", "Right Stick Click", T_BOOL, R_ "input/thumbstick/click", R_ "input/thumbstick/click"},
    [A_TRIG_L] = {"trigger_left", "Left Trigger", T_FLOAT, L_ "input/trigger/value", L_ "input/trigger/value"},
    [A_TRIG_R] = {"trigger_right", "Right Trigger", T_FLOAT, R_ "input/trigger/value", R_ "input/trigger/value"},
    [A_GRIP_L] = {"grip_left", "Left Grip", T_FLOAT, L_ "input/%s", L_ "input/squeeze/value"},  // (%s: squeeze/value or grip/value)
    [A_GRIP_R] = {"grip_right", "Right Grip", T_FLOAT, R_ "input/%s", R_ "input/squeeze/value"},
    [A_STICK_L] = {"stick_left", "Left Stick", T_VEC2, L_ "input/thumbstick", L_ "input/thumbstick"},
    [A_STICK_R] = {"stick_right", "Right Stick", T_VEC2, R_ "input/thumbstick", R_ "input/thumbstick"},
    [A_HAPTIC] = {"haptic", "Haptic", T_HAPTIC, R_ "output/haptic", R_ "output/haptic"},  // (and the left one: both below)
};
// (the Frame's controls to the virtual buttons: A_* of a boolean control -> C_*, the floats go through thresholds)
static const int8_t k_bool_ctl[A_GRIP_L] = {[A_A] = C_A, [A_B] = C_B, [A_X] = C_X, [A_Y] = C_Y, [A_MENU] = C_MENU, [A_VIEW] = C_VIEW,
                                           [A_DUP] = C_DUP, [A_DDOWN] = C_DDOWN, [A_DLEFT] = C_DLEFT, [A_DRIGHT] = C_DRIGHT,
                                           [A_BUMP_L] = C_BUMP_L, [A_BUMP_R] = C_BUMP_R, [A_CLICK_L] = C_CLICK_L,
                                           [A_CLICK_R] = C_CLICK_R, [A_TRIG_L] = C_TRIG_L, [A_TRIG_R] = C_TRIG_R};

#define XRIN_FUNCS(F)                                                                                                 \
    F(xrStringToPath) F(xrCreateActionSet) F(xrDestroyActionSet) F(xrCreateAction) F(xrSuggestInteractionProfileBindings) \
    F(xrAttachSessionActionSets) F(xrSyncActions) F(xrGetActionStateBoolean) F(xrGetActionStateFloat)                 \
    F(xrGetActionStateVector2f) F(xrApplyHapticFeedback) F(xrStopHapticFeedback)
#define XRIN_DECL(n) static PFN_##n p_##n;
XRIN_FUNCS(XRIN_DECL)
#undef XRIN_DECL

static struct {
    bool ready, attached, valve, log;
    XrActionSet set;
    XrAction act[A_COUNT];
    XrPath hand[2];
    bool down[C_COUNT];        // the virtual buttons now (thresholds applied)
    bool synced_once, was_focused;
    // haptics
    uint64_t next_pulse_ns;
    bool buzzing;
    // outputs
    uint32_t last_host;
    int32_t last_lx, last_ly;
    int last_axis[2];          // the left stick as last sent to the menus (SDL axis events)
    bool snap_armed;
} S = {.snap_armed = true};

static _Atomic float g_yaw;
static _Atomic int g_snap;                // -1 / +1 pending, 0 none
static _Atomic bool g_recentre;
static _Atomic float g_tilt, g_height, g_size;
static _Atomic bool g_modifier;

void xr_input_set_yaw(float yaw) { atomic_store(&g_yaw, yaw); }
void xr_input_rig(XrRigInput *out) {
    out->tilt = atomic_load(&g_tilt), out->height = atomic_load(&g_height), out->size = atomic_load(&g_size);
    out->modifier = atomic_load(&g_modifier);
}
int xr_input_take_snap(void) { return atomic_exchange(&g_snap, 0); }
bool xr_input_take_recentre(void) { return atomic_exchange(&g_recentre, false); }

// ---- set-up
static XrPath path(const char *s) {
    XrPath p = XR_NULL_PATH;
    p_xrStringToPath(g_xr.inst, s, &p);
    return p;
}

// The profile's bindings; grip: the name of the grip input of the Frame's profile ("squeeze/value", else "grip/value").
static bool suggest(const char *profile, bool frame, const char *grip) {
    XrActionSuggestedBinding b[2 * A_COUNT];
    uint32_t n = 0;
    for (int i = 0; i < A_COUNT; i++) {
        const char *p = frame ? k_act[i].frame : k_act[i].touch;
        if (!p) continue;
        char buf[96];
        if (strstr(p, "%s")) snprintf(buf, sizeof buf, p, grip), p = buf;
        b[n++] = (XrActionSuggestedBinding){S.act[i], path(p)};
        if (i == A_HAPTIC) b[n++] = (XrActionSuggestedBinding){S.act[i], path(L_ "output/haptic")};
    }
    XrInteractionProfileSuggestedBinding sb = {.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                               .interactionProfile = path(profile), .suggestedBindings = b, .countSuggestedBindings = n};
    XrResult r = p_xrSuggestInteractionProfileBindings(g_xr.inst, &sb);
    LOG("[xr] controller bindings for %s%s%s: %s\n", profile, frame ? " (grip on " : "", frame ? grip : "", XR_SUCCEEDED(r) ? "ok" : xr_result_str(r));
    return XR_SUCCEEDED(r);
}

void xr_input_init(PFN_xrGetInstanceProcAddr gipa, bool valve) {
    memset(&S, 0, sizeof S);
    S.snap_armed = true;
    S.valve = valve;
    S.log = getenv("LARECOMP_XR_INPUT_LOG") != NULL;
    const char *y = getenv("LARECOMP_XR_YAW");
    if (y) atomic_store(&g_yaw, (float)atof(y) * 0.01745329252f);
#define XRIN_GET(nm) if (XR_FAILED(gipa(g_xr.inst, #nm, (PFN_xrVoidFunction *)&p_##nm)) || !p_##nm) { LOG("[xr] controllers: the runtime lacks %s\n", #nm); return; }
    XRIN_FUNCS(XRIN_GET)
#undef XRIN_GET
    S.hand[0] = path("/user/hand/left"), S.hand[1] = path("/user/hand/right");
    XrActionSetCreateInfo si = {.type = XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(si.actionSetName, sizeof si.actionSetName, "gameplay");
    snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "Gameplay");
    if (!XR_OK(p_xrCreateActionSet(g_xr.inst, &si, &S.set))) return;
    for (int i = 0; i < A_COUNT; i++) {
        XrActionCreateInfo ai = {.type = XR_TYPE_ACTION_CREATE_INFO};
        snprintf(ai.actionName, sizeof ai.actionName, "%s", k_act[i].name);
        snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", k_act[i].label);
        ai.actionType = k_act[i].kind == T_BOOL ? XR_ACTION_TYPE_BOOLEAN_INPUT : k_act[i].kind == T_FLOAT ? XR_ACTION_TYPE_FLOAT_INPUT
                      : k_act[i].kind == T_VEC2 ? XR_ACTION_TYPE_VECTOR2F_INPUT : XR_ACTION_TYPE_VIBRATION_OUTPUT;
        if (k_act[i].kind == T_HAPTIC) ai.countSubactionPaths = 2, ai.subactionPaths = S.hand;
        if (!XR_OK(p_xrCreateAction(S.set, &ai, &S.act[i]))) return;
    }
    bool ok = false;
    if (valve) ok = suggest("/interaction_profiles/valve/frame_controller", true, "squeeze/value") ||
                    suggest("/interaction_profiles/valve/frame_controller", true, "grip/value");
    // (other headsets: Touch controllers, for development; SteamVR on the Frame ignores the profile it doesn't use)
    ok |= suggest("/interaction_profiles/oculus/touch_controller", false, NULL);
    S.ready = ok;
    if (!ok) LOG("[xr] controllers: no interaction profile accepted the bindings\n");
}

void xr_input_attach(void) {
    if (!S.ready || S.attached) return;
    XrSessionActionSetsAttachInfo ai = {.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &S.set};
    S.attached = XR_OK(p_xrAttachSessionActionSets(g_xr.session, &ai));
    if (S.attached) LOG("[xr] controllers: %d actions attached (%s)\n", A_COUNT, S.valve ? "Steam Frame profile" : "Touch profile only");
}

void xr_input_shutdown(void) {
    if (S.attached && S.buzzing)
        for (int h = 0; h < 2; h++) {
            XrHapticActionInfo hi = {.type = XR_TYPE_HAPTIC_ACTION_INFO, .action = S.act[A_HAPTIC], .subactionPath = S.hand[h]};
            p_xrStopHapticFeedback(g_xr.session, &hi);
        }
    if (S.set) p_xrDestroyActionSet(S.set);
    S.set = XR_NULL_HANDLE, S.ready = S.attached = S.buzzing = false;
}

// ---- reading
static bool get_bool(int a) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = S.act[a]};
    XrActionStateBoolean st = {.type = XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(p_xrGetActionStateBoolean(g_xr.session, &gi, &st)) && st.isActive && st.currentState;
}
static float get_float(int a) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = S.act[a]};
    XrActionStateFloat st = {.type = XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(p_xrGetActionStateFloat(g_xr.session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}
static void get_vec2(int a, float v[2]) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = S.act[a]};
    XrActionStateVector2f st = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
    v[0] = v[1] = 0;
    if (XR_SUCCEEDED(p_xrGetActionStateVector2f(g_xr.session, &gi, &st)) && st.isActive) v[0] = st.currentState.x, v[1] = st.currentState.y;
}

// A radial dead zone: nothing inside dz, then rescaled so the edge of the zone is 0 and the rim is 1.
static void deadzone(float v[2], float dz) {
    float m = hypotf(v[0], v[1]);
    if (m <= dz) { v[0] = v[1] = 0; return; }
    float k = (m > 1 ? 1 : m) - dz;
    k = k / (1 - dz) / m;
    v[0] *= k, v[1] *= k;
}
// An analogue control as a button, with hysteresis
static bool threshold(bool was, float v) { return was ? v > 0.4f : v > 0.6f; }

static int32_t to_axis(float v) { return v >= 1 ? 32767 : v <= -1 ? -32767 : (int32_t)lroundf(v * 32767); }

// the game's rumble -> haptics (once per refresh)
static void haptics(void) {
    uint64_t now = plat_now_ns();
    float level = input_rumble_level();
    {   // LARECOMP_XR_HAPTIC_TEST="t0-t1": a rumble of 0.5 from t0 to t1 seconds after the first refresh (a test without a hit)
        static double a = -1, b = -1;
        static uint64_t t0;
        static int init;
        if (!init) {
            init = 1, t0 = now;
            const char *e = getenv("LARECOMP_XR_HAPTIC_TEST");
            if (e) sscanf(e, "%lf-%lf", &a, &b);
        }
        double t = (double)(now - t0) / 1e9;
        if (a >= 0 && t >= a && t < b && level < 0.5f) level = 0.5f;
    }
    if (level > 0) {
        if (now >= S.next_pulse_ns) {
            for (int h = 0; h < 2; h++) {
                XrHapticActionInfo hi = {.type = XR_TYPE_HAPTIC_ACTION_INFO, .action = S.act[A_HAPTIC], .subactionPath = S.hand[h]};
                XrHapticVibration v = {.type = XR_TYPE_HAPTIC_VIBRATION, .duration = 120000000, .frequency = XR_FREQUENCY_UNSPECIFIED,
                                       .amplitude = level > 1 ? 1.0f : level};
                static bool logged;
                XrResult r = p_xrApplyHapticFeedback(g_xr.session, &hi, (const XrHapticBaseHeader *)&v);
                if (!logged) logged = true, LOG("[xr] haptics: first pulse %s\n", XR_SUCCEEDED(r) ? "ok" : xr_result_str(r));
            }
            S.next_pulse_ns = now + 80000000;
        }
        S.buzzing = true;
    } else if (S.buzzing) {
        for (int h = 0; h < 2; h++) {
            XrHapticActionInfo hi = {.type = XR_TYPE_HAPTIC_ACTION_INFO, .action = S.act[A_HAPTIC], .subactionPath = S.hand[h]};
            p_xrStopHapticFeedback(g_xr.session, &hi);
        }
        S.buzzing = false, S.next_pulse_ns = 0;
    }
}

void xr_input_frame(void) {
    if (!S.attached) return;
    XrActiveActionSet as = {.actionSet = S.set, .subactionPath = XR_NULL_PATH};
    XrActionsSyncInfo si = {.type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &as};
    XrResult r = p_xrSyncActions(g_xr.session, &si);
    bool focused = r == XR_SUCCESS;  // (XR_SESSION_NOT_FOCUSED: another app has the controllers: everything is released)
    if (XR_FAILED(r)) { xr_ok_(r, "xrSyncActions"); focused = false; }
    if (focused != S.was_focused) {
        LOG("[xr] controllers %s\n", focused ? "in the game's hands" : "released (the session is not focused)");
        S.was_focused = focused;
    }
    float sl[2] = {0, 0}, sr[2] = {0, 0};
    if (focused) {
        for (int a = 0; a < A_GRIP_L; a++)
            if (k_act[a].kind == T_BOOL) S.down[k_bool_ctl[a]] = get_bool(a);
            else S.down[k_bool_ctl[a]] = threshold(S.down[k_bool_ctl[a]], get_float(a));
        S.down[C_GRIP_L] = threshold(S.down[C_GRIP_L], get_float(A_GRIP_L));
        S.down[C_GRIP_R] = threshold(S.down[C_GRIP_R], get_float(A_GRIP_R));
        get_vec2(A_STICK_L, sl), get_vec2(A_STICK_R, sr);
        deadzone(sl, 0.15f), deadzone(sr, 0.15f);
    } else {
        memset(S.down, 0, sizeof S.down);
    }
    bool mod = S.down[C_GRIP_L];

    // the rig's: the left grip makes the D-pad (and a click of the right stick) the rig's, not the game's
    bool rig_ctl[C_COUNT] = {0};
    if (mod) rig_ctl[C_DUP] = rig_ctl[C_DDOWN] = rig_ctl[C_DLEFT] = rig_ctl[C_DRIGHT] = rig_ctl[C_CLICK_R] = true;
    atomic_store(&g_modifier, mod);
    atomic_store(&g_tilt, sr[1]);
    atomic_store(&g_height, mod ? (float)(S.down[C_DUP] - S.down[C_DDOWN]) : 0.0f);
    atomic_store(&g_size, mod ? (float)(S.down[C_DRIGHT] - S.down[C_DLEFT]) : 0.0f);
    static bool click_was;
    if (mod && S.down[C_CLICK_R] && !click_was) atomic_store(&g_recentre, true);
    click_was = mod && S.down[C_CLICK_R];
    float ax = fabsf(sr[0]);
    if (ax < 0.35f) S.snap_armed = true;
    else if (ax > 0.7f && S.snap_armed) S.snap_armed = false, atomic_store(&g_snap, sr[0] > 0 ? 1 : -1);

    // the game's: buttons through the layout table, the left stick turned by the table's yaw
    uint32_t host = 0;
    for (size_t i = 0; i < sizeof k_map / sizeof *k_map; i++)
        if (S.down[k_map[i].ctl] && !rig_ctl[k_map[i].ctl] && k_map[i].host >= 0 && k_map[i].host < 32) host |= 1u << k_map[i].host;
    float yaw = atomic_load(&g_yaw), c = cosf(yaw), s = sinf(yaw);
    // (app space: x right, y ahead; the table's frame is the app's turned by -yaw, the game's up is the table's -z)
    int32_t lx = to_axis(sl[0] * c + sl[1] * s), ly = to_axis(-sl[0] * s + sl[1] * c);
    if (S.log && (host != S.last_host || lx != S.last_lx || ly != S.last_ly))
        LOG("[xr] input: buttons %#x left stick (%.2f, %.2f) yaw %.0f deg -> game stick %d,%d\n", host, sl[0], sl[1], yaw * 57.29578f, lx, ly);
    S.last_host = host, S.last_lx = lx, S.last_ly = ly;
    input_xr_set(host, lx, ly);
    haptics();
}
