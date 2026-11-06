// The Frame controllers in VR mode: one action set, its bindings (the Steam Frame's own profile, XR_VALVE_frame_controller
// _interaction; the Touch profile as a fallback for other headsets), the sync once per display refresh, and the hand-off
// of the result to the game (runtime/hle/input.c input_xr_set, as the buttons and the left stick of a gamepad) and to
// the rig (xr.h: turn, tilt, height / size, reset view). The game's rumble (input_rumble_level) goes to both
// controllers as short haptic pulses renewed while it lasts (GalaxyFrame's scheme: 120 ms pulses every 80 ms, stopped
// at once when the rumble ends, so a runtime that ignores a pulse's duration can't leave a controller running).
// Reference: GalaxyFrame's platform/src/xr/xr_app.cpp (initActions, suggestFrame, updateInput).
//
// The layout is in docs/af/CONTROLS.md (with the game's meaning of each Switch button) and in k_map_switch / k_map_comfort below (LARECOMP_XR_LAYOUT=comfort picks the second).
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#include "../input_bind.h"
#include "../rt.h"
#include "../settings.h"
#include "xr_input.h"

// ---- the layout: which Frame control is which gamepad button (SDL names, Switch positions: the east button is A)
// Each control may drive several; the player's own controller bindings (Controls tab) turn them into game inputs.
enum { PADB_ZL = PADB_LTRIGGER, PADB_ZR = PADB_RTRIGGER };
typedef enum { C_A, C_B, C_X, C_Y, C_MENU, C_VIEW, C_DUP, C_DDOWN, C_DLEFT, C_DRIGHT, C_BUMP_L, C_BUMP_R, C_CLICK_L, C_CLICK_R,
               C_TRIG_L, C_TRIG_R, C_GRIP_L, C_GRIP_R, C_COUNT } Ctl;
typedef struct { Ctl ctl; int host; } MapEnt;
// The default ("switch"): a Switch Pro controller, control for control, so the game's own prompts (L, R, ZL, ZR, A...) name the
// Frame control that does it: bumpers = L / R, triggers = ZL / ZR, Menu = Plus, View = Minus, the grips: right = A, left = the
// rig's modifier (not a game button). The face buttons are the four buttons of the Switch's diamond *by position* (the Frame's
// bottom button = the Switch's bottom one, B) unless the player's "Buttons match labels" is on (then the Frame's A is A:
// face_host below); the Frame's A B X Y are taken to be laid out as an Xbox pad's (A bottom, B right, X left, Y top).
static const MapEnt k_map_switch[] = {
    {C_MENU, SDL_GAMEPAD_BUTTON_START}, {C_VIEW, SDL_GAMEPAD_BUTTON_BACK},
    {C_DUP, SDL_GAMEPAD_BUTTON_DPAD_UP},     {C_DDOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {C_DLEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {C_DRIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {C_BUMP_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {C_BUMP_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {C_TRIG_L, PADB_ZL}, {C_TRIG_R, PADB_ZR},
    {C_GRIP_R, SDL_GAMEPAD_BUTTON_EAST},
    {C_CLICK_L, SDL_GAMEPAD_BUTTON_LEFT_STICK}, {C_CLICK_R, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
};
// LARECOMP_XR_LAYOUT=comfort: the sword on the right trigger, the shield on the left, the Pegasus Boots on the right bumper,
// the face buttons by letter (the Frame's A is the Switch's A), the left bumper ZL (as before the Switch layout was the default).
static const MapEnt k_map_comfort[] = {
    {C_A, SDL_GAMEPAD_BUTTON_EAST},   {C_B, SDL_GAMEPAD_BUTTON_SOUTH},        {C_X, SDL_GAMEPAD_BUTTON_NORTH},
    {C_Y, SDL_GAMEPAD_BUTTON_WEST},   {C_MENU, SDL_GAMEPAD_BUTTON_START},      {C_VIEW, SDL_GAMEPAD_BUTTON_BACK},
    {C_DUP, SDL_GAMEPAD_BUTTON_DPAD_UP},     {C_DDOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {C_DLEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {C_DRIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {C_TRIG_R, SDL_GAMEPAD_BUTTON_SOUTH}, {C_TRIG_L, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {C_BUMP_R, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {C_BUMP_L, PADB_ZL},
    {C_GRIP_R, SDL_GAMEPAD_BUTTON_EAST},
    {C_CLICK_L, SDL_GAMEPAD_BUTTON_LEFT_STICK}, {C_CLICK_R, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
};
// The face buttons of the switch layout: by position (default) or by label (g_settings.face_by_label)
static int face_host(Ctl c, bool by_label) {
    switch (c) {
    case C_A: return by_label ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
    case C_B: return by_label ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
    case C_X: return by_label ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST;
    default: return by_label ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH;  // C_Y
    }
}

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

#define RESET_HOLD_NS 100000000ull  // both sticks clicked together this long: reset the view

#define XRIN_FUNCS(F)                                                                                                 \
    F(xrStringToPath) F(xrCreateActionSet) F(xrDestroyActionSet) F(xrCreateAction) F(xrSuggestInteractionProfileBindings) \
    F(xrAttachSessionActionSets) F(xrSyncActions) F(xrGetActionStateBoolean) F(xrGetActionStateFloat)                 \
    F(xrGetActionStateVector2f) F(xrApplyHapticFeedback) F(xrStopHapticFeedback) F(xrCreateActionSpace)
#define XRIN_DECL(n) static PFN_##n p_##n;
XRIN_FUNCS(XRIN_DECL)
#undef XRIN_DECL

static struct {
    bool ready, attached, valve, log, comfort;
    XrActionSet set;
    XrActionSet gaze_set;      // the eye gaze (XR_EXT_eye_gaze_interaction: foveation), a set of its own
    XrAction gaze_act;
    XrSpace gaze_space;
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
    int dom;                   // the right stick's dominant axis: 0 none, 1 sideways (turn), 2 up / down (tilt)
    uint64_t both_ns;          // when both stick clicks went down together (0: not both down)
    bool both_fired;           // the reset for this press was sent
} S = {.snap_armed = true};

static _Atomic float g_yaw;
static _Atomic int g_snap;                // -1 / +1 pending, 0 none
static _Atomic bool g_reset;
static _Atomic float g_tilt, g_turn, g_height, g_size;
static _Atomic bool g_modifier;

void xr_input_set_yaw(float yaw) { atomic_store(&g_yaw, yaw); }
void xr_input_rig(XrRigInput *out) {
    out->tilt = atomic_load(&g_tilt), out->turn = atomic_load(&g_turn), out->height = atomic_load(&g_height), out->size = atomic_load(&g_size);
    out->modifier = atomic_load(&g_modifier);
}
int xr_input_take_snap(void) { return atomic_exchange(&g_snap, 0); }
bool xr_input_take_reset_view(void) { return atomic_exchange(&g_reset, false); }

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
    const char *lay = getenv("LARECOMP_XR_LAYOUT");
    S.comfort = lay && !strcasecmp(lay, "comfort");
    LOG("[xr] controller layout: %s\n", S.comfort ? "comfort (sword on the right trigger, shield on the left)" : "switch (a Switch Pro controller, control for control)");
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
    if (ok && g_xr.has_gaze) {  // (the eye tracker: one pose action, for the foveation of the eye passes)
        snprintf(si.actionSetName, sizeof si.actionSetName, "eye_gaze");
        snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "Eye Gaze");
        XrActionCreateInfo ai = {.type = XR_TYPE_ACTION_CREATE_INFO, .actionType = XR_ACTION_TYPE_POSE_INPUT};
        snprintf(ai.actionName, sizeof ai.actionName, "gaze");
        snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "Gaze");
        XrActionSuggestedBinding b = {XR_NULL_HANDLE, path("/user/eyes_ext/input/gaze_ext/pose")};
        XrInteractionProfileSuggestedBinding sb = {.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                                   .interactionProfile = path("/interaction_profiles/ext/eye_gaze_interaction"),
                                                   .suggestedBindings = &b, .countSuggestedBindings = 1};
        XrResult r = XR_ERROR_RUNTIME_FAILURE;
        if (XR_OK(p_xrCreateActionSet(g_xr.inst, &si, &S.gaze_set)) && XR_OK(p_xrCreateAction(S.gaze_set, &ai, &S.gaze_act)))
            b.action = S.gaze_act, r = p_xrSuggestInteractionProfileBindings(g_xr.inst, &sb);
        LOG("[xr] eye gaze binding: %s\n", XR_SUCCEEDED(r) ? "ok" : xr_result_str(r));
        if (XR_FAILED(r) && S.gaze_set) p_xrDestroyActionSet(S.gaze_set), S.gaze_set = XR_NULL_HANDLE;
    }
}

void xr_input_attach(void) {
    if (!S.ready || S.attached) return;
    XrActionSet sets[2] = {S.set, S.gaze_set};
    XrSessionActionSetsAttachInfo ai = {.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = S.gaze_set ? 2 : 1,
                                        .actionSets = sets};
    S.attached = XR_OK(p_xrAttachSessionActionSets(g_xr.session, &ai));
    if (S.attached && S.gaze_set) {
        XrActionSpaceCreateInfo sc = {.type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = S.gaze_act,
                                      .poseInActionSpace = {.orientation = {0, 0, 0, 1}}};
        if (!XR_OK(p_xrCreateActionSpace(g_xr.session, &sc, &S.gaze_space))) S.gaze_space = XR_NULL_HANDLE;
    }
    if (S.attached) LOG("[xr] controllers: %d actions attached (%s)\n", A_COUNT, S.valve ? "Steam Frame profile" : "Touch profile only");
}

void xr_input_shutdown(void) {
    if (S.attached && S.buzzing)
        for (int h = 0; h < 2; h++) {
            XrHapticActionInfo hi = {.type = XR_TYPE_HAPTIC_ACTION_INFO, .action = S.act[A_HAPTIC], .subactionPath = S.hand[h]};
            p_xrStopHapticFeedback(g_xr.session, &hi);
        }
    if (S.gaze_space) p_xrDestroySpace(S.gaze_space);
    if (S.gaze_set) p_xrDestroyActionSet(S.gaze_set);
    if (S.set) p_xrDestroyActionSet(S.set);
    S.set = S.gaze_set = XR_NULL_HANDLE, S.gaze_space = XR_NULL_HANDLE, S.ready = S.attached = S.buzzing = false;
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

// View + Menu (the overlay menu's chord: menu.mm, Back + Start) must not reach the game as Minus / Plus too: whichever
// was pressed first used to (the game's map or pause menu opened under the overlay). A press of either is held back
// for LARECOMP_XR_CHORD_MS (120) or until it is released: the other one joining in that time makes the chord, which goes
// to the menus only (both, in one refresh) and keeps both from the game until both are up; otherwise the press goes to
// the game (a release inside the window: a tap, replayed to the game as long as it was held, 60 ms at least). Once
// one has gone to the game, the other joining later is no chord: it goes to the game too, the menus see only the first.
// host: the buttons held now; *game: the game's, *menus: the menus' events.
static void chord_filter(uint32_t host, uint64_t now, uint32_t *game, uint32_t *menus) {
    static const uint32_t VM = 1u << SDL_GAMEPAD_BUTTON_BACK | 1u << SDL_GAMEPAD_BUTTON_START;
    static uint32_t prev, pend, passed, first, replay;
    static uint64_t pend_t0, window_ns, replay_end;
    static bool chord;
    if (!window_ns) {
        const char *e = getenv("LARECOMP_XR_CHORD_MS");
        window_ns = (uint64_t)((e ? atof(e) : 120.0) * 1e6);
        if (!window_ns) window_ns = 1;
    }
    uint32_t r = host & VM, out = 0;
    if (chord) {
        if (!r) chord = false;
        *game = host & ~VM, *menus = (host & ~VM) | r;
        prev = r;
        return;
    }
    if (r == VM && !passed) {  // the chord: the menus get both, the game neither
        static unsigned n;
        if (n++ < 20) LOG("[xr] View + Menu: the menu (%.0f ms apart; not passed to the game)\n", pend ? (now - pend_t0) / 1e6 : 0.0);
        chord = true, pend = passed = replay = 0, prev = r;
        *game = host & ~VM, *menus = host;
        return;
    }
    uint32_t down = r & ~prev & ~passed;
    if (down && !pend) pend_t0 = now;
    pend |= down;
    uint32_t tap = pend & ~r;  // (released inside the window)
    pend &= ~tap;
    if (tap) {
        uint64_t held = now - pend_t0;
        replay |= tap, replay_end = now + (held > 60000000 ? held : 60000000);
    }
    if (replay && now >= replay_end) replay = 0;
    if (pend && now - pend_t0 >= window_ns) {
        if (!passed) first = pend;
        passed |= pend, pend = 0;
    }
    passed &= r;
    if (!passed) first = 0;
    out = passed | replay;
    prev = r;
    *game = (host & ~VM) | out;
    *menus = (host & ~VM) | (out == VM && first ? first : out);
}

void xr_input_frame(void) {
    if (!S.attached) return;
    XrActiveActionSet as[2] = {{.actionSet = S.set, .subactionPath = XR_NULL_PATH}, {.actionSet = S.gaze_set, .subactionPath = XR_NULL_PATH}};
    XrActionsSyncInfo si = {.type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = S.gaze_set ? 2 : 1, .activeActionSets = as};
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

    // the rig's: the left grip makes the D-pad (and a click of the right stick) the rig's, not the game's; so does a click
    // of both sticks, held for RESET_HOLD: a single click, or both for less than that, goes to the game as it always did
    bool rig_ctl[C_COUNT] = {0};
    if (mod) rig_ctl[C_DUP] = rig_ctl[C_DDOWN] = rig_ctl[C_DLEFT] = rig_ctl[C_DRIGHT] = rig_ctl[C_CLICK_R] = true;
    atomic_store(&g_modifier, mod);
    {
        uint64_t now = plat_now_ns();
        if (S.down[C_CLICK_L] && S.down[C_CLICK_R]) {
            if (!S.both_ns) S.both_ns = now, S.both_fired = false;
            if (now - S.both_ns >= RESET_HOLD_NS) {
                if (!S.both_fired) S.both_fired = true, atomic_store(&g_reset, true);
                rig_ctl[C_CLICK_L] = rig_ctl[C_CLICK_R] = true;  // (the reset's: the game sees neither until both are let go)
            }
        } else if (S.both_fired && (S.down[C_CLICK_L] || S.down[C_CLICK_R])) {
            rig_ctl[C_CLICK_L] = rig_ctl[C_CLICK_R] = true;      // (one let go, the other still down: still not a click of the game's)
        } else S.both_ns = 0, S.both_fired = false;
    }
    // the right stick: sideways turns the table, up / down tilts it; the axis it was first pushed along wins until the other
    // is clearly bigger (1.4 times), so a thumb that wanders does not tilt a turn or turn a tilt
    float ax = fabsf(sr[0]), ay = fabsf(sr[1]);
    if (ax < 0.01f && ay < 0.01f) S.dom = 0;
    else if (S.dom == 0) S.dom = ax >= ay ? 1 : 2;
    else if (S.dom == 1 && ay > ax * 1.4f) S.dom = 2;
    else if (S.dom == 2 && ax > ay * 1.4f) S.dom = 1;
    atomic_store(&g_tilt, S.dom == 2 ? sr[1] : 0.0f);
    atomic_store(&g_turn, S.dom == 1 ? sr[0] : 0.0f);
    atomic_store(&g_height, mod ? (float)(S.down[C_DUP] - S.down[C_DDOWN]) : 0.0f);
    atomic_store(&g_size, mod ? (float)(S.down[C_DRIGHT] - S.down[C_DLEFT]) : 0.0f);
    static bool click_was;  // (left grip + a click of the right stick: the same reset as both sticks)
    if (mod && S.down[C_CLICK_R] && !click_was) atomic_store(&g_reset, true);
    click_was = mod && S.down[C_CLICK_R];
    if (ax < 0.35f) S.snap_armed = true;
    else if (ax > 0.7f && S.snap_armed && S.dom == 1) S.snap_armed = false, atomic_store(&g_snap, sr[0] > 0 ? 1 : -1);

    // the game's: buttons through the layout table, the left stick turned by the table's yaw
    uint32_t host = 0;
    const MapEnt *map = S.comfort ? k_map_comfort : k_map_switch;
    size_t nmap = S.comfort ? sizeof k_map_comfort / sizeof *k_map_comfort : sizeof k_map_switch / sizeof *k_map_switch;
    for (size_t i = 0; i < nmap; i++)
        if (S.down[map[i].ctl] && !rig_ctl[map[i].ctl] && map[i].host >= 0 && map[i].host < 32) host |= 1u << map[i].host;
    if (!S.comfort)
        for (Ctl c = C_A; c <= C_Y; c++)
            if (S.down[c]) host |= 1u << face_host(c, g_settings.face_by_label);
    float yaw = atomic_load(&g_yaw), c = cosf(yaw), s = sinf(yaw);
    // (app space: x right, y ahead; the table's frame is the app's turned by -yaw, the game's up is the table's -z)
    int32_t lx = to_axis(sl[0] * c + sl[1] * s), ly = to_axis(-sl[0] * s + sl[1] * c);
    if (S.log && (host != S.last_host || lx != S.last_lx || ly != S.last_ly)) {
        // (the Switch inputs those host buttons are now bound to: input.c input_xr_set's own lookup, for the log)
        static const char *const sw[BIND_PAD_COUNT] = {"A", "B", "X", "Y", "LStick", "RStick", "L", "R", "ZL", "ZR", "Plus", "Minus",
                                                       "Left", "Up", "Right", "Down"};
        char names[96] = "";
        for (int i = 0; i < BIND_PAD_COUNT; i++)
            if ((unsigned)g_bind_pad[i] < 32 && (host >> g_bind_pad[i] & 1)) snprintf(names + strlen(names), sizeof names - strlen(names), " %s", sw[i]);
        LOG("[xr] input: buttons %#x = Switch [%s ] left stick (%.2f, %.2f) yaw %.0f deg -> game stick %d,%d\n", host, names, sl[0], sl[1],
            yaw * 57.29578f, lx, ly);
    }
    S.last_host = host, S.last_lx = lx, S.last_ly = ly;
    uint32_t game, menus;
    chord_filter(host, plat_now_ns(), &game, &menus);
    static uint32_t last_game, last_menus;
    if (S.log && (game != last_game || menus != last_menus)) LOG("[xr] input: to the game %#x, to the menus %#x\n", game, menus);
    last_game = game, last_menus = menus;
    input_xr_set2(game, menus, lx, ly);
    haptics();
}

// (xr_vk.c xr_latch_gaze) the eye gaze's direction in `base` at `t` (unit, -z of the gaze pose); false: no gaze then
// (no eye tracker, the session not focused, or not tracked).
bool xr_input_gaze(XrSpace base, XrTime t, float dir[3]) {
    if (!S.gaze_space || !base) return false;
    XrSpaceLocation loc = {.type = XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if (XR_FAILED(p_xrLocateSpace(S.gaze_space, base, t, &loc)) || (loc.locationFlags & need) != need) return false;
    const XrQuaternionf q = loc.pose.orientation;  // rotate (0, 0, -1) by q
    dir[0] = -2 * (q.x * q.z + q.w * q.y);
    dir[1] = -2 * (q.y * q.z - q.w * q.x);
    dir[2] = -(1 - 2 * (q.x * q.x + q.y * q.y));
    return true;
}
