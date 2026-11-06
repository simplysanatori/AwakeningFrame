// The diorama rig (xr_rig.h; docs/af/RIG.md).
#include "xr_rig.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../platform.h"
#include "../rt.h"
#include "xr.h"

#define DEG ((float)(M_PI / 180.0))
#define SCREEN_X 15.0f    // one screen cell, game units (triforce.c)
#define SCREEN_Z 12.0f
#define CUT_JUMP 12.0f    // the game camera's focus moving this far in one frame: a camera cut (a warp, a door)
#define CUT_FAR 24.0f     // the anchor this far from where it should be: snap (as the comfort lane's follow)
#define SIDE_THIN 6.0f    // a zone thinner than this in z is a side view (as cam_pivot / widescreen.c)
#define SIDE_PITCH 12.0f  // degrees below the horizon: a side view is a wall at about eye height
#define MIN_ELEV 20.0f    // degrees: the head never looks along the table flatter than this (a tilt away is limited by it)
#define STABLE 3          // latches an area / room must hold before the rig takes it (a zone read flickering at a load)
// the blink: a snap turn / recentre fades out first; an automatic cut (a room change, a warp) goes black at once
#define T_OUT 0.08f
#define T_HOLD 0.05f
#define T_HOLD_RECENTRE 0.08f
#define T_IN 0.15f
#define T_IN_CUT 0.22f
#define T_BLACK_MAX 0.30f  // black and no game frame came (a menu): fade back in anyway
// the controls' rates
#define TILT_RATE 45.0f    // degrees / s at full stick
#define TILT_DEAD 0.30f    // stick dead zone for tilt (a snap flick is rarely level)
#define HEIGHT_RATE 0.30f  // metres / s
#define SIZE_RATE 0.70f    // e-folds / s (doubling in about a second)
#define TURN_RATE 80.0f    // degrees / s at full stick (smooth turning)
#define TURN_DEAD 0.20f    // stick dead zone for turning, past the input's radial one (rescaled: no jump at its edge)
#define TURN_TAU 0.12f     // s: the turn speed follows the stick's with this lag, so it starts and stops without a jerk

// runtime/ui/view_toast.mm: a short label on the HUD (weak: a build without the UI shows none)
extern void view_toast_show(const char *text) __attribute__((weak));

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
enum { F_IDLE, F_OUT, F_BLACK, F_HOLD, F_IN };
static struct {
    bool init;
    // configuration (environment)
    float s0, dist, pitch, tau;
    bool have_pitch, have_table;
    float table[3];
    char path[1024];
    bool save;
    // the player's
    XrRigUser u;
    bool dirty;
    uint64_t t_dirty;
    // anchoring
    bool valid;
    int area;
    float zlo[3], zhi[3];
    int cand_area, cand_n;
    float cand_lo[3], cand_hi[3];
    float p[3], last_raw[3];
    float side_yaw;
    uint64_t t_last;
    float sent_yaw;
    bool sent;
    // the blink
    int phase;
    uint64_t t0;
    float t_hold, t_in;
    int pend_turn;        // -1 / +1 snap turn waiting for black
    bool pend_recentre;
    bool pend_reset;      // reset view: yaw back to 0 (with the recentre) once black
    bool snap;            // snap turning (LARECOMP_VR_TURN=snap, the VR tab's Turning), else smooth
    float turn_vel;       // smooth turning's speed now, degrees / s (positive: the view goes round to the right)
    bool pend_yaw;        // xr_rig_set_user changed the yaw
    float pend_yaw_deg;
    // stats
    uint64_t cuts, turns, recentres, resets;
} R;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float smooth01(float x) { x = clampf(x, 0, 1); return x * x * (3 - 2 * x); }

static void clamp_user(XrRigUser *u) {
    u->height = clampf(isfinite(u->height) ? u->height : 0, XR_RIG_HEIGHT_MIN, XR_RIG_HEIGHT_MAX);
    u->size = clampf(isfinite(u->size) ? u->size : 1, XR_RIG_SIZE_MIN, XR_RIG_SIZE_MAX);
    u->tilt = clampf(isfinite(u->tilt) ? u->tilt : 0, XR_RIG_TILT_MIN, XR_RIG_TILT_MAX);
    u->yaw = isfinite(u->yaw) ? remainderf(u->yaw, 360.0f) : 0;
}

// ---- persistence (height, size, tilt)
static void load(void) {
    FILE *f = R.path[0] ? fopen(R.path, "r") : NULL;
    if (!f) return;
    char line[128];
    float v;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "height = %f", &v) == 1) R.u.height = v;
        else if (sscanf(line, "size = %f", &v) == 1) R.u.size = v;
        else if (sscanf(line, "tilt = %f", &v) == 1) R.u.tilt = v;
    }
    fclose(f);
    clamp_user(&R.u);
    LOG("[rig] %s: height %+.2f m, size x%.2f, tilt %.0f deg\n", R.path, R.u.height, R.u.size, R.u.tilt);
}
static void save_now(void) {
    R.dirty = false;
    if (!R.save || !R.path[0]) return;
    FILE *f = fopen(R.path, "w");
    if (!f) { LOG("[rig] cannot write %s\n", R.path); return; }
    fprintf(f, "# AwakeningFrame table placement (docs/af/RIG.md)\nheight = %.3f\nsize = %.3f\ntilt = %.1f\n", R.u.height, R.u.size, R.u.tilt);
    fclose(f);
}
static void mark_dirty(uint64_t now) { R.dirty = true, R.t_dirty = now; }

static void init(void) {
    const char *e;
    R.init = true;
    R.s0 = (e = getenv("LARECOMP_VR_SCALE")) && atof(e) > 0 ? (float)atof(e) : 0.067f;
    R.dist = (e = getenv("LARECOMP_VR_DIST")) && atof(e) > 0 ? (float)atof(e) : 1.0f;
    R.tau = (e = getenv("LARECOMP_VR_FOLLOW_TAU")) && atof(e) > 0 ? (float)atof(e) : 0.5f;
    if ((e = getenv("LARECOMP_VR_PITCH"))) R.pitch = (float)atof(e), R.have_pitch = true;
    if ((e = getenv("LARECOMP_VR_TABLE")) && sscanf(e, "%f,%f,%f", &R.table[0], &R.table[1], &R.table[2]) == 3) R.have_table = true;
    R.u = (XrRigUser){.height = 0, .size = 1, .tilt = 0, .yaw = 0};
    // the file: LARECOMP_VR_RIG, else next to LARECOMP_SETTINGS' file, else the data root (the working directory)
    const char *st = getenv("LARECOMP_SETTINGS");
    if ((e = getenv("LARECOMP_VR_RIG")) && *e) snprintf(R.path, sizeof R.path, "%s", e);
    else if (st && *st) {
        const char *sl = strrchr(st, '/');
        snprintf(R.path, sizeof R.path, "%.*svr_rig.ini", sl ? (int)(sl - st + 1) : 0, st);
    } else snprintf(R.path, sizeof R.path, "vr_rig.ini");
    R.save = !((e = getenv("LARECOMP_VR_RIG_SAVE")) && !strcmp(e, "0"));
    R.snap = (e = getenv("LARECOMP_VR_TURN")) && !strcasecmp(e, "snap");
    load();
    // (tests) start values
    if ((e = getenv("LARECOMP_VR_HEIGHT"))) R.u.height = (float)atof(e);
    if ((e = getenv("LARECOMP_VR_SIZE"))) R.u.size = (float)atof(e);
    if ((e = getenv("LARECOMP_VR_TILT"))) R.u.tilt = (float)atof(e);
    if ((e = getenv("LARECOMP_VR_YAW"))) R.u.yaw = (float)atof(e);
    clamp_user(&R.u);
    LOG("[rig] %.3f m per unit x%.2f, %.2f m away, height %+.2f m, tilt %.0f deg, yaw %.0f deg; settings file %s%s\n", R.s0, R.u.size,
        R.dist, R.u.height, R.u.tilt, R.u.yaw, R.path, R.save ? "" : " (not saved)");
}

// ---- the blink
static void advance(uint64_t now) {  // (locked)
    float t = (float)((now - R.t0) / 1e9);
    if (R.phase == F_OUT && t >= T_OUT) R.phase = F_BLACK, R.t0 = now;
    else if (R.phase == F_BLACK && t >= T_BLACK_MAX) {
        // no game frame since it went black (a menu): what needs none is done now, the rest at the next frame
        if (R.pend_recentre) xr_recentre(), R.pend_recentre = false;
        if (R.pend_reset) R.u.yaw = 0, R.turn_vel = 0, R.pend_reset = false;
        R.phase = F_HOLD, R.t0 = now, R.t_hold = T_HOLD;
    } else if (R.phase == F_HOLD && t >= R.t_hold) R.phase = F_IN, R.t0 = now;
    else if (R.phase == F_IN && t >= R.t_in) R.phase = F_IDLE;
}
static float fade_now(uint64_t now) {  // (locked)
    advance(now);
    float t = (float)((now - R.t0) / 1e9);
    switch (R.phase) {
    case F_OUT: return smooth01(t / T_OUT);
    case F_BLACK: case F_HOLD: return 1;
    case F_IN: return 1 - smooth01(t / R.t_in);
    default: return 0;
    }
}
static void blink_out(void) {  // (locked) a fade out; what is pending is done once black
    if (R.phase == F_IDLE || R.phase == F_IN) R.phase = F_OUT, R.t0 = plat_now_ns(), R.t_in = T_IN;
}
static void blink_cut(uint64_t now) {  // (locked) black at once, then in
    R.phase = F_HOLD, R.t0 = now, R.t_hold = T_HOLD, R.t_in = T_IN_CUT;
}

float xr_rig_fade(void) {
    pthread_mutex_lock(&g_mu);
    float f = R.init ? fade_now(plat_now_ns()) : 0;
    pthread_mutex_unlock(&g_mu);
    return f;
}

// ---- the user API
void xr_rig_get_user(XrRigUser *u) {
    pthread_mutex_lock(&g_mu);
    if (!R.init) init();
    *u = R.u;
    if (R.pend_yaw) u->yaw = R.pend_yaw_deg;
    pthread_mutex_unlock(&g_mu);
}
void xr_rig_set_user(const XrRigUser *in) {
    XrRigUser u = *in;
    clamp_user(&u);
    pthread_mutex_lock(&g_mu);
    if (!R.init) init();
    R.u.height = u.height, R.u.size = u.size, R.u.tilt = u.tilt;
    if (fabsf(remainderf(u.yaw - R.u.yaw, 360.0f)) > 0.01f) R.pend_yaw = true, R.pend_yaw_deg = u.yaw, blink_out();
    mark_dirty(plat_now_ns());
    pthread_mutex_unlock(&g_mu);
}
void xr_rig_reset_user(void) { xr_rig_set_user(&(XrRigUser){.height = 0, .size = 1, .tilt = 0, .yaw = 0}); }
void xr_rig_set_turn_mode(int snap) {  // the VR tab's Turning: 1 snap, 0 smooth
    pthread_mutex_lock(&g_mu);
    if (!R.init) init();
    R.snap = snap != 0, R.turn_vel = 0, R.pend_turn = 0;
    pthread_mutex_unlock(&g_mu);
}
void xr_rig_reset_view(void) {
    pthread_mutex_lock(&g_mu);
    if (!R.init) init();
    R.pend_reset = R.pend_recentre = true;
    blink_out();
    pthread_mutex_unlock(&g_mu);
}
void xr_rig_recentre(void) {
    pthread_mutex_lock(&g_mu);
    if (!R.init) init();
    R.pend_recentre = true;
    blink_out();
    pthread_mutex_unlock(&g_mu);
}

// ---- once per game frame
static int classify(const XrRigIn *in) {
    if (in->overworld == 1) return XR_RIG_OPEN;  // (its camera zones are areas, not rooms)
    if (in->zone) return in->zhi[2] - in->zlo[2] < SIDE_THIN ? XR_RIG_SIDE : XR_RIG_ROOM;
    if (fabsf(in->fwd[1]) < 0.2f) return XR_RIG_SIDE;
    // no zone in a dungeon or an interior: between rooms (a scroll, stairs, a fall); keep the area there was
    return in->overworld == 0 && R.valid && R.area != XR_RIG_OPEN ? R.area : XR_RIG_OPEN;
}
static bool box_differs(const float alo[3], const float ahi[3], const float blo[3], const float bhi[3]) {
    return fabsf(alo[0] - blo[0]) + fabsf(alo[2] - blo[2]) + fabsf(ahi[0] - bhi[0]) + fabsf(ahi[2] - bhi[2]) > 0.5f;
}
// a room's anchor: its centre when it is about a screen; a bigger room keeps the screen inside it (the game camera's
// focus clamped half a screen in from its walls), which is where the game's own camera is anyway
static float room_axis(float raw, float lo, float hi, float half) {
    if (hi - lo <= 2 * half + 1.0f) return (lo + hi) * 0.5f;
    return clampf(raw, lo + half, hi - half);
}
// the table point for this area, and the lowest tilt that keeps the head MIN_ELEV above the table's plane there
static void table_point(bool side, float A[3], float *tilt_min) {
    float pitch = (side ? SIDE_PITCH : R.pitch) * DEG;
    if (R.have_table) memcpy(A, R.table, 12);
    else A[0] = 0, A[1] = -R.dist * sinf(pitch), A[2] = -R.dist * cosf(pitch);
    A[1] += R.u.height;
    float n = sqrtf(A[0] * A[0] + A[1] * A[1] + A[2] * A[2]);
    float below = n > 1e-3f ? asinf(clampf(-A[1] / n, -1, 1)) / DEG : 45.0f;  // the table point's angle under the horizon
    *tilt_min = fmaxf(XR_RIG_TILT_MIN, MIN_ELEV - below);
}
static const char *area_name(int a) { return a == XR_RIG_ROOM ? "room" : a == XR_RIG_SIDE ? "side view" : "overworld"; }

void xr_rig_update(const XrRigIn *in, XrRigPose *out) {
    pthread_mutex_lock(&g_mu);
    if (!R.init) init();
    uint64_t now = plat_now_ns();
    float dt = R.t_last ? (float)((now - R.t_last) / 1e9) : 0;
    if (dt > 0.1f) dt = 0.1f;
    R.t_last = now;
    if (!R.have_pitch) {  // the game camera's own pitch at the first frame (about 45 at Mabe), so the first look is the game's
        float p = asinf(clampf(-in->fwd[1], -1, 1)) / DEG;
        R.pitch = clampf(p, 25.0f, 65.0f), R.have_pitch = true;
    }
    advance(now);

    // the player's controls
    XrRigInput ri;
    xr_input_rig(&ri);
    int snap = xr_input_take_snap();
    if (xr_input_take_reset_view()) R.pend_reset = R.pend_recentre = true, blink_out();
    if (snap && R.snap && R.area != XR_RIG_SIDE) R.pend_turn = snap, blink_out();
    {   // smooth turning: the stick sets a turn speed (an eased curve of the deflection), the speed follows it with a lag
        float t = fabsf(ri.turn) > TURN_DEAD ? (fabsf(ri.turn) - TURN_DEAD) / (1 - TURN_DEAD) : 0;
        float target = R.snap || R.area == XR_RIG_SIDE ? 0 : copysignf(t * (0.4f + 0.6f * t) * TURN_RATE, ri.turn);
        R.turn_vel += (target - R.turn_vel) * (1 - expf(-dt / TURN_TAU));
        if (target == 0 && fabsf(R.turn_vel) < 0.2f) R.turn_vel = 0;
        // (right: the view goes round to the right, the table turns clockwise: the yaw goes down, as the snap turn's)
        if (R.turn_vel != 0) R.u.yaw = remainderf(R.u.yaw - R.turn_vel * dt, 360.0f);
    }
    float tilt_in = fabsf(ri.tilt) > TILT_DEAD ? (ri.tilt - copysignf(TILT_DEAD, ri.tilt)) / (1 - TILT_DEAD) : 0;
    if (ri.height != 0) R.u.height = clampf(R.u.height + ri.height * HEIGHT_RATE * dt, XR_RIG_HEIGHT_MIN, XR_RIG_HEIGHT_MAX), mark_dirty(now);
    float A_[3], tilt_min;
    table_point(false, A_, &tilt_min);
    if (tilt_in != 0 && R.area != XR_RIG_SIDE)
        R.u.tilt = clampf(R.u.tilt + tilt_in * TILT_RATE * dt, fminf(tilt_min, fmaxf(R.u.tilt, XR_RIG_TILT_MIN)), XR_RIG_TILT_MAX), mark_dirty(now);
    if (ri.size != 0) R.u.size = clampf(R.u.size * expf(ri.size * SIZE_RATE * dt), XR_RIG_SIZE_MIN, XR_RIG_SIZE_MAX), mark_dirty(now);
    if (R.dirty && now - R.t_dirty > 1000000000ull) save_now();

    // black: what waited for it
    if (R.phase == F_BLACK) {
        if (R.pend_turn) {  // (right: the view goes a step round to the right, as GalaxyFrame: the table turns clockwise)
            R.u.yaw = remainderf(R.u.yaw - 45.0f * (float)R.pend_turn, 360.0f), R.turns++;
            LOG("[rig] snap turn %s: yaw %.0f deg\n", R.pend_turn > 0 ? "right" : "left", R.u.yaw);
        }
        if (R.pend_yaw) R.u.yaw = R.pend_yaw_deg;
        if (R.pend_reset) {
            R.u.yaw = 0, R.turn_vel = 0, R.resets++;
            LOG("[rig] reset view: yaw 0 deg (the game's own angle), tilt %.0f deg, height %+.2f m, size x%.2f kept\n", R.u.tilt, R.u.height, R.u.size);
            if (view_toast_show) view_toast_show("View Reset");
        }
        float hold = T_HOLD;
        if (R.pend_recentre) {
            xr_recentre(), R.recentres++, hold = T_HOLD_RECENTRE;
            LOG("[rig] recentre: the table goes in front of the player again\n");
        }
        R.pend_turn = 0, R.pend_yaw = R.pend_recentre = R.pend_reset = false;
        R.phase = F_HOLD, R.t0 = now, R.t_hold = hold;
    }
    if ((R.pend_turn || R.pend_yaw || R.pend_recentre || R.pend_reset) && (R.phase == F_IDLE || R.phase == F_IN)) blink_out();  // (asked mid-blink)

    // the area and the anchor
    int kind;
    float target[3];
    bool cut = false;
    float jump2 = 0;
    for (int i = 0; i < 3; i++) jump2 += (in->raw[i] - R.last_raw[i]) * (in->raw[i] - R.last_raw[i]);
    if (in->explicit_follow) {  // the comfort lane's follow as it is; jumps still blink
        kind = classify(in);
        memcpy(target, in->followed, sizeof target);
        float d2 = 0;
        for (int i = 0; i < 3; i++) d2 += (target[i] - R.p[i]) * (target[i] - R.p[i]);
        cut = !R.valid || d2 > CUT_JUMP * CUT_JUMP || kind != R.area;
        if (cut && R.valid) blink_cut(now), R.cuts++;
        if (kind == XR_RIG_SIDE && (cut || R.area != XR_RIG_SIDE)) R.side_yaw = -atan2f(-in->fwd[0], -in->fwd[2]) / DEG;
        R.area = kind, R.valid = true;
        memcpy(R.p, target, sizeof R.p);
    } else {
        // the area / room the frame shows, once it has held for STABLE frames (or at once on a camera cut)
        kind = classify(in);
        bool camcut = R.valid && jump2 > CUT_JUMP * CUT_JUMP;
        bool same_cand = kind == R.cand_area && (kind == XR_RIG_OPEN || !in->zone || !box_differs(in->zlo, in->zhi, R.cand_lo, R.cand_hi));
        R.cand_n = same_cand ? R.cand_n + 1 : 1;
        R.cand_area = kind;
        if (in->zone) memcpy(R.cand_lo, in->zlo, 12), memcpy(R.cand_hi, in->zhi, 12);
        bool changed = kind != R.area || (kind != XR_RIG_OPEN && in->zone && box_differs(in->zlo, in->zhi, R.zlo, R.zhi));
        bool take = !R.valid || (changed && (R.cand_n >= STABLE || camcut));
        if (!take) kind = R.area;  // (keep the old area's rule until the new one holds)
        bool use_in = in->zone && (take || !R.valid);
        const float *lo = use_in ? in->zlo : R.zlo, *hi = use_in ? in->zhi : R.zhi;
        memcpy(target, in->raw, sizeof target);
        if (kind == XR_RIG_ROOM && (in->zone || R.valid)) {
            target[0] = room_axis(in->raw[0], lo[0], hi[0], SCREEN_X * 0.5f);
            target[2] = room_axis(in->raw[2], lo[2], hi[2], SCREEN_Z * 0.5f);
        }
        float d2 = 0;
        for (int i = 0; i < 3; i++) d2 += (target[i] - R.p[i]) * (target[i] - R.p[i]);
        cut = !R.valid || (take && changed) || camcut || d2 > CUT_FAR * CUT_FAR;
        if (cut) {
            if (R.valid) {
                blink_cut(now), R.cuts++;
                if (R.cuts < 200 || R.cuts % 100 == 0)
                    LOG("[rig] %s -> %s%s: the table snaps behind a blink to (%.1f %.1f %.1f); zone %s %.0f x %.0f x %.0f (%llu so far)\n", area_name(R.area),
                        area_name(kind), camcut ? " (camera cut)" : take && changed && kind == R.area ? " (another room)" : "", target[0], target[1],
                        target[2], in->zone ? "" : "none", in->zhi[0] - in->zlo[0], in->zhi[1] - in->zlo[1], in->zhi[2] - in->zlo[2],
                        (unsigned long long)R.cuts);
            } else LOG("[rig] %s: table anchor at (%.1f %.1f %.1f)\n", area_name(kind), target[0], target[1], target[2]);
            if (kind == XR_RIG_SIDE)  // (the yaw that turns the game camera's horizontal forward away from the player)
                R.side_yaw = -atan2f(-in->fwd[0], -in->fwd[2]) / DEG;
            memcpy(R.p, target, sizeof R.p);
            R.area = kind, R.valid = true;
            if (in->zone) memcpy(R.zlo, in->zlo, 12), memcpy(R.zhi, in->zhi, 12);
        } else {
            // filtered (a single-screen room's target does not move; a room keeps the height it was entered at: Link's
            // jumps and falls into holes do not move the table)
            float a = 1 - expf(-dt / R.tau), ay = kind == XR_RIG_ROOM ? 0 : 1 - expf(-dt / (R.tau * 2));
            R.p[0] += (target[0] - R.p[0]) * a, R.p[2] += (target[2] - R.p[2]) * a, R.p[1] += (target[1] - R.p[1]) * ay;
        }
    }
    memcpy(R.last_raw, in->raw, sizeof R.last_raw);

    // the pose
    bool side = R.area == XR_RIG_SIDE;
    table_point(side, out->A, &tilt_min);
    float yaw = (side ? R.side_yaw : R.u.yaw) * DEG, tilt = (side ? 0 : fmaxf(R.u.tilt, tilt_min)) * DEG;
    out->s = R.s0 * R.u.size;
    // R = Rx(tilt) * Ry(yaw); Ry(yaw): x' = x cos + z sin, z' = -x sin + z cos; Rx(tilt): y' = y cos - z sin, z' = y sin + z cos
    float cy = cosf(yaw), sy = sinf(yaw), ct = cosf(tilt), st = sinf(tilt);
    const float ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy}, rx[9] = {1, 0, 0, 0, ct, -st, 0, st, ct};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) out->R[r * 3 + c] = rx[r * 3 + 0] * ry[0 * 3 + c] + rx[r * 3 + 1] * ry[1 * 3 + c] + rx[r * 3 + 2] * ry[2 * 3 + c];
    memcpy(out->anchor, R.p, sizeof out->anchor);
    out->area = R.area;
    if (!R.sent || fabsf(yaw - R.sent_yaw) > 1e-4f) xr_input_set_yaw(yaw), R.sent_yaw = yaw, R.sent = true;
    static int logrig = -1;  // LARECOMP_LOG_RIG=<frames>: the rig's state every that many game frames
    if (logrig < 0) logrig = getenv("LARECOMP_LOG_RIG") ? atoi(getenv("LARECOMP_LOG_RIG")) : 0;
    static uint64_t logged;
    if (logrig > 0 && in->frame >= logged + (uint64_t)logrig) {
        logged = in->frame;
        LOG("[rig] f%llu %s anchor (%.2f %.2f %.2f) raw (%.2f %.2f %.2f) A (%.2f %.2f %.2f) s %.4f yaw %.0f tilt %.1f height %+.2f size x%.2f fade %.2f\n",
            (unsigned long long)in->frame, area_name(R.area), R.p[0], R.p[1], R.p[2], in->raw[0], in->raw[1], in->raw[2], out->A[0], out->A[1],
            out->A[2], out->s, yaw / DEG, tilt / DEG, R.u.height, R.u.size, fade_now(now));
    }
    pthread_mutex_unlock(&g_mu);
}
