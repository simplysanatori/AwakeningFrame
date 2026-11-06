// Stereo executor: switches and the v1 diorama's eye cameras (stereo.h).
#include "stereo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../platform.h"
#include "../../rt.h"
#include "../cam_override.h"
#include "foveation.h"
#ifdef LARECOMP_HAVE_XR
#include "../../xr/xr.h"
#include "../../xr/xr_rig.h"
#else
typedef struct { float orientation[4], position[3], fov[4]; } XrEyeView;
#endif

bool stereo_wanted(void) {
    static int on = -1;
    if (on < 0) {
        const char *x = getenv("LARECOMP_XR"), *s = getenv("LARECOMP_STEREO");
        on = x && atoi(x) != 0 && !(s && !strcmp(s, "0"));
#ifndef LARECOMP_HAVE_XR
        on = 0;
#endif
    }
    return on;
}

bool stereo_eye_size(uint32_t *w, uint32_t *h) {
#ifdef LARECOMP_HAVE_XR
    if (!stereo_wanted() || !xr_active()) return false;
    static int ow = -1, oh;
    if (ow < 0) {
        const char *e = getenv("LARECOMP_STEREO_SIZE");
        ow = 0;
        if (e && sscanf(e, "%dx%d", &ow, &oh) != 2) ow = 0;
    }
    if (ow > 0 && oh > 0) { *w = (uint32_t)ow, *h = (uint32_t)oh; return true; }
    return xr_eye_size(w, h);
#else
    (void)w, (void)h;
    return false;
#endif
}

bool stereo_active(void) {
    uint32_t w, h;
    return stereo_wanted() && stereo_eye_size(&w, &h);
}

// ---------------------------------------------------------------- the diorama's eye cameras
// World (game units) to the app's space (metres; origin = the head at the last recentre, y up, -z ahead):
//   app = A + s * R * (world - anchor)
// A, s, R and the anchor are the rig's (runtime/xr/xr_rig.c, docs/af/RIG.md), once per game frame: the table moves only
// on new game frames. The raw pivot is the game camera's screen centre at Link's height. The eye cameras keep the
// game's depth terms (near / far: the shaders that linearise depth with the game's constants stay right) unless
// LARECOMP_VR_NEAR=<units>.
static struct {
    bool init, explicit_follow;
    float near_;
    int follow;        // LARECOMP_VR_FOLLOW: FOLLOW_SMOOTH, FOLLOW_ROOM, FOLLOW_FIXED (unset: the rig's per-area anchoring)
    float tau;         // LARECOMP_VR_FOLLOW_TAU: seconds (smooth)
} cfg;
enum { FOLLOW_SMOOTH, FOLLOW_ROOM, FOLLOW_FIXED };

static void cfg_init(void) {
    const char *e;
    cfg.init = true;
    cfg.near_ = (e = getenv("LARECOMP_VR_NEAR")) ? (float)atof(e) : 0.0f;
    e = getenv("LARECOMP_VR_FOLLOW");
    cfg.explicit_follow = e && *e && strcmp(e, "auto");
    cfg.follow = e && !strcmp(e, "room") ? FOLLOW_ROOM : e && !strcmp(e, "fixed") ? FOLLOW_FIXED : FOLLOW_SMOOTH;
    cfg.tau = (e = getenv("LARECOMP_VR_FOLLOW_TAU")) && atof(e) > 0 ? (float)atof(e) : 0.5f;
    if (cfg.explicit_follow)
        LOG("[stereo] the table follows the game's camera: %s (%.2f s time constant; a cut snaps it)\n",
        cfg.follow == FOLLOW_ROOM ? "room (held; glides to a new camera zone)" : cfg.follow == FOLLOW_FIXED ? "fixed (held)" : "smooth", cfg.tau);
}

// The VR tab's Follow Mode while the game runs (runtime/vr_settings.c; VRS_FOLLOW_*): Smooth is the tab's default, which
// leaves LARECOMP_VR_FOLLOW unset at the start (the rig's per-area anchoring), so it goes back to that; Room and Fixed are
// the explicit follow. Taken by the next latch (eye_fn), where the explicit anchor starts at the table's current one: the
// table does not jump.
static int g_follow_req = -1;  // (atomic) -1 none, else VRS_FOLLOW_*
void stereo_set_follow(int mode) {
    if (mode < 0 || mode > 2) return;
    __atomic_store_n(&g_follow_req, mode, __ATOMIC_RELEASE);
}

// The table's anchor in the world: where the game's camera looks (the raw pivot, once per game frame) followed per
// LARECOMP_VR_FOLLOW, on new game frames only (called once per latch):
//   smooth  filtered (time constant cfg.tau; the height twice that: Link's jumps)
//   room    held; when the camera zone changes it glides (as smooth) until the game's camera has settled, then holds
//   fixed   held
// A cut (the raw pivot more than CUT_UNITS away: a level change, a teleport, a camera cut) snaps in every mode.
#define CUT_UNITS 24.0f
static struct { bool ok, gliding; float p[3], last[3], lo[3], hi[3]; bool zone; uint64_t t_ns, still_ns; uint64_t snaps, glides; } anc;
static void anchor_update(const float raw[3], float out[3]) {
    float lo[3], hi[3];
    bool zone = cam_override_zone(lo, hi);
    uint64_t now = plat_now_ns();
    bool zchg = anc.ok && (zone != anc.zone || (zone && (fabsf(lo[0] - anc.lo[0]) + fabsf(lo[2] - anc.lo[2]) + fabsf(hi[0] - anc.hi[0]) +
                                                       fabsf(hi[2] - anc.hi[2]) > 0.5f)));
    float d2 = 0, m2 = 0;
    for (int i = 0; i < 3; i++) d2 += (raw[i] - anc.p[i]) * (raw[i] - anc.p[i]), m2 += (raw[i] - anc.last[i]) * (raw[i] - anc.last[i]);
    float dt = anc.ok ? (float)((now - anc.t_ns) / 1e9) : 0;
    if (dt > 0.1f) dt = 0.1f;
    if (!anc.ok || d2 > CUT_UNITS * CUT_UNITS) {
        memcpy(anc.p, raw, sizeof anc.p);
        anc.snaps++, anc.gliding = false;
        if (anc.snaps < 50 || anc.snaps % 100 == 0)
            LOG("[stereo] table anchor at (%.2f %.2f %.2f) (%s; %llu so far)\n", raw[0], raw[1], raw[2], !anc.ok ? "first" : "a cut",
                (unsigned long long)anc.snaps);
    } else {
        if (zchg && cfg.follow == FOLLOW_ROOM) {
            if (!anc.gliding && (anc.glides++ < 50 || anc.glides % 100 == 0))
                LOG("[stereo] table anchor: the camera zone changed, gliding to the new one (%llu so far)\n", (unsigned long long)anc.glides);
            anc.gliding = true, anc.still_ns = now;
        }
        if (anc.gliding && m2 > 0.0025f) anc.still_ns = now;  // (the game's camera still moving: 0.05 units a frame)
        if (anc.gliding && now - anc.still_ns > 400000000ull && d2 < 0.01f) anc.gliding = false;
        if (cfg.follow == FOLLOW_SMOOTH || anc.gliding) {
            float a = 1.0f - expf(-dt / cfg.tau), ay = 1.0f - expf(-dt / (cfg.tau * 2.0f));
            anc.p[0] += (raw[0] - anc.p[0]) * a, anc.p[2] += (raw[2] - anc.p[2]) * a, anc.p[1] += (raw[1] - anc.p[1]) * ay;
        }
    }
    anc.ok = true, anc.t_ns = now, anc.zone = zone;
    memcpy(anc.last, raw, sizeof anc.last);
    if (zone) memcpy(anc.lo, lo, sizeof lo), memcpy(anc.hi, hi, sizeof hi);
    memcpy(out, anc.p, sizeof anc.p);
}

// What the latest latch's eye cameras were built with: the latch (the poses go into the projection layer of the set its
// eye images are shown in: stereo_note_composite / stereo_set_views), the head views, and the per-eye V / P (game units).
static struct { bool ok; uint64_t id; XrEyeView ev[2]; float V[2][16], P[2][16]; } g_rv;
// Per eye output: the latch it was composited with (0: none, or the game's own camera)
static struct { uint64_t id; XrEyeView ev; float V[16], P[16]; } g_out[2];

static void quat_mat(const float q[4], float m[3][3]) {  // x y z w -> rotation (columns: the eye's axes in app space)
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z), m[0][1] = 2 * (x * y - z * w), m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w), m[1][1] = 1 - 2 * (x * x + z * z), m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w), m[2][1] = 2 * (y * z + x * w), m[2][2] = 1 - 2 * (x * x + y * y);
}

static uint64_t g_logged_frame;
static struct { bool ok, room, zone; float anchor[3], lo[3], hi[3]; } g_table;  // (stereo_table: the latest latch's)
bool stereo_table(float anchor[3], bool *room, bool *zone, float lo[3], float hi[3]) {
    if (!g_table.ok) return false;
    memcpy(anchor, g_table.anchor, 12), memcpy(lo, g_table.lo, 12), memcpy(hi, g_table.hi, 12);
    *room = g_table.room, *zone = g_table.zone;
    return true;
}

#ifdef LARECOMP_HAVE_XR
static bool eye_fn(const CamLatch *l, const float pivot_in[3], float V[2][16], float P[2][16]) {
    XrEyeView ev[2];
    uint64_t vid;
    g_rv.ok = false;
    if (!xr_latch_views_id(ev, &vid)) return false;
    if (!cfg.init) cfg_init();
    {
        int req = __atomic_exchange_n(&g_follow_req, -1, __ATOMIC_ACQ_REL);
        if (req >= 0) {
            bool ex = req != 0;  // (VRS_FOLLOW_SMOOTH 0: the rig's own anchoring)
            if (ex && g_table.ok) memcpy(anc.p, g_table.anchor, 12), memcpy(anc.last, pivot_in, 12), anc.ok = true, anc.gliding = false,
                                  anc.t_ns = plat_now_ns(), anc.zone = cam_override_zone(anc.lo, anc.hi);
            else if (ex) anc.ok = false;
            cfg.explicit_follow = ex;
            cfg.follow = req == 1 ? FOLLOW_ROOM : req == 2 ? FOLLOW_FIXED : FOLLOW_SMOOTH;
            LOG("[stereo] follow mode %s (VR settings)\n", req == 1 ? "room" : req == 2 ? "fixed" : "smooth (the rig's area anchoring)");
        }
    }
    float pivot_raw[3] = {pivot_in[0], pivot_in[1], pivot_in[2]}, pivot[3];
    if (cfg.explicit_follow) anchor_update(pivot_raw, pivot);
    else memcpy(pivot, pivot_raw, sizeof pivot);
    XrRigIn ri = {.explicit_follow = cfg.explicit_follow, .frame = l->frame};  // (the rig: area anchoring, the player's controls, the blink)
    memcpy(ri.raw, pivot_raw, 12), memcpy(ri.followed, pivot, 12), memcpy(ri.fwd, l->ref.fwd, 12);
    ri.zone = cam_override_zone(ri.zlo, ri.zhi);
    extern int game_level_overworld(void);  // (widescreen.c)
    ri.overworld = game_level_overworld();
    XrRigPose rp;
    xr_rig_update(&ri, &rp);
    memcpy(pivot, rp.anchor, 12);
    g_table.ok = true, g_table.room = rp.area != XR_RIG_OPEN, g_table.zone = ri.zone;
    memcpy(g_table.anchor, rp.anchor, 12), memcpy(g_table.lo, ri.zlo, 12), memcpy(g_table.hi, ri.zhi, 12);
    cam_override_table_anchor(pivot);  // (perf lane: the culling fit centres the table here)
    const float *Rw = rp.R;  // world -> app (row-major); its transpose takes app to world
    const float *gp = l->ref.P;
    for (int e = 0; e < 2; e++) {
        float m[3][3], w[3][3];
        quat_mat(ev[e].orientation, m);
        for (int r = 0; r < 3; r++)  // world rotation = R^T * m
            for (int c = 0; c < 3; c++) w[r][c] = Rw[0 * 3 + r] * m[0][c] + Rw[1 * 3 + r] * m[1][c] + Rw[2 * 3 + r] * m[2][c];
        float d[3] = {(ev[e].position[0] - rp.A[0]) / rp.s, (ev[e].position[1] - rp.A[1]) / rp.s, (ev[e].position[2] - rp.A[2]) / rp.s};
        float E[3];
        for (int r = 0; r < 3; r++) E[r] = pivot[r] + Rw[0 * 3 + r] * d[0] + Rw[1 * 3 + r] * d[1] + Rw[2 * 3 + r] * d[2];
        float *v = V[e];
        for (int r = 0; r < 3; r++) {  // rows: the eye's right, up, back in world; rigid, game units
            v[r * 4 + 0] = w[0][r], v[r * 4 + 1] = w[1][r], v[r * 4 + 2] = w[2][r];
            v[r * 4 + 3] = -(w[0][r] * E[0] + w[1][r] * E[1] + w[2][r] * E[2]);
        }
        v[12] = v[13] = v[14] = 0, v[15] = 1;
        float tl = tanf(ev[e].fov[0]), tr = tanf(ev[e].fov[1]), tu = tanf(ev[e].fov[2]), td = tanf(ev[e].fov[3]);
        float *p = P[e];
        memset(p, 0, 64);
        float sx = gp[0] < 0 ? -1.0f : 1.0f, syy = gp[5] < 0 ? -1.0f : 1.0f;  // (the game's own axis signs)
        p[0] = sx * 2 / (tr - tl), p[2] = sx * (tr + tl) / (tr - tl);
        p[5] = syy * 2 / (tu - td), p[6] = syy * (tu + td) / (tu - td);
        p[10] = gp[10], p[11] = gp[11];
        if (cfg.near_ > 0) {
            float f = l->far_, n = cfg.near_;
            p[10] = -(f + n) / (f - n), p[11] = -2 * f * n / (f - n);
        }
        p[14] = -1;
        static int test = -1;  // (debug bisect) LARECOMP_STEREO_TEST=gamecam: the game's V and P; gameP: the game's P
        if (test < 0) {
            const char *t = getenv("LARECOMP_STEREO_TEST");
            test = !t ? 0 : !strcmp(t, "gamecam") ? 1 : !strcmp(t, "gameP") ? 2 : !strcmp(t, "gamerot") ? 3 : !strcmp(t, "gamepos") ? 4 : 0;
        }
        if (test == 1) memcpy(v, l->ref.V, 64);
        if (test == 3 || test == 4) {  // (the game's rotation with this eye's position / this rotation at the game's eye)
            const float *gv = l->ref.V, *pos = test == 3 ? E : l->ref.eye;
            if (test == 3) for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) v[r * 4 + c] = gv[r * 4 + c];
            for (int r = 0; r < 3; r++) v[r * 4 + 3] = -(v[r * 4] * pos[0] + v[r * 4 + 1] * pos[1] + v[r * 4 + 2] * pos[2]);
        }
        if (test >= 1 && test <= 2) memcpy(p, l->ref.P, 64);
        if (e == 1 && l->frame == g_logged_frame)
            LOG("[stereo]   eye1 (%.2f %.2f %.2f) units\n", E[0], E[1], E[2]);
        if (e == 0 && l->frame >= g_logged_frame + 600) {
            g_logged_frame = l->frame;
            LOG("[stereo] f%llu pivot (%.2f %.2f %.2f) eye0 (%.2f %.2f %.2f) units; game eye (%.2f %.2f %.2f) near %.3g far %.3g\n",
                (unsigned long long)l->frame, pivot[0], pivot[1], pivot[2], E[0], E[1], E[2], l->ref.eye[0], l->ref.eye[1], l->ref.eye[2],
                l->near_, l->far_);
            float c[4];
            for (int i = 0; i < 4; i++) {
                float vw[4];
                for (int k = 0; k < 4; k++) vw[k] = v[k * 4 + 0] * pivot[0] + v[k * 4 + 1] * pivot[1] + v[k * 4 + 2] * pivot[2] + v[k * 4 + 3];
                c[i] = p[i * 4 + 0] * vw[0] + p[i * 4 + 1] * vw[1] + p[i * 4 + 2] * vw[2] + p[i * 4 + 3] * vw[3];
            }
            LOG("[stereo]   game P00 %.3f P11 %.3f P02 %.3f P12 %.3f V up row (%.2f %.2f %.2f); eye0 fwd (%.2f %.2f %.2f); pivot -> ndc (%.3f %.3f %.3f)\n",
                gp[0], gp[5], gp[2], gp[6], l->ref.V[4], l->ref.V[5], l->ref.V[6], -v[8], -v[9], -v[10], c[0] / c[3], c[1] / c[3], c[2] / c[3]);
        }
    }
    g_rv.ok = true, g_rv.id = vid;
    memcpy(g_rv.ev, ev, sizeof ev);
    fov_note_views((const float[2][4]){{ev[0].orientation[0], ev[0].orientation[1], ev[0].orientation[2], ev[0].orientation[3]},
                                      {ev[1].orientation[0], ev[1].orientation[1], ev[1].orientation[2], ev[1].orientation[3]}},
                   (const float[2][4]){{ev[0].fov[0], ev[0].fov[1], ev[0].fov[2], ev[0].fov[3]},
                                      {ev[1].fov[0], ev[1].fov[1], ev[1].fov[2], ev[1].fov[3]}}, vid);  // (foveation.c)
    memcpy(g_rv.V, V, sizeof g_rv.V), memcpy(g_rv.P, P, sizeof g_rv.P);
    return true;
}
#endif

void stereo_note_composite(int eye) {
    if (eye < 0 || eye > 1) return;
#ifdef LARECOMP_HAVE_XR
    if (g_rv.ok && cam_override_eye_ready()) {
        g_out[eye].id = g_rv.id, g_out[eye].ev = g_rv.ev[eye];
        memcpy(g_out[eye].V, g_rv.V[eye], 64), memcpy(g_out[eye].P, g_rv.P[eye], 64);
        return;
    }
#endif
    g_out[eye].id = 0;
}

bool stereo_eye_matrices(int eye, float V[16], float P[16]) {
    if (eye < 0 || eye > 1 || !g_out[eye].id) return false;
    memcpy(V, g_out[eye].V, 64), memcpy(P, g_out[eye].P, 64);
    return true;
}

int stereo_set_views(void) {
#ifdef LARECOMP_HAVE_XR
    static uint64_t n, bad, split, gone, repeats, prev_id, t_log;
    uint64_t id[2] = {g_out[0].id, g_out[1].id};
    if (id[0] && id[0] == prev_id) repeats++;  // (an older output again: the poses go with it)
    prev_id = id[0];
    uint64_t now = plat_now_ns();
    if (now - t_log > 10000000000ull) {
        if (t_log) LOG("[stereo] poses: %llu sets in 10 s, %llu showed an older eye output again (with its own poses)\n",
                       (unsigned long long)n, (unsigned long long)repeats);
        t_log = now, n = repeats = 0;
    }
    XrEyeView ev[2] = {g_out[0].ev, g_out[1].ev};
    int r = id[0] && id[1] ? xr_set_views(id, ev) : -1;
    n++;
    if (r < 0) gone++;
    else if (r & 3) bad++;
    if (r >= 0 && (r & 4)) split++;
    if ((r < 0 || (r & 7)) && (gone + bad + split) <= 20)
        LOG("[stereo] set %llu: eye images' poses %s (latches %llu / %llu)\n", (unsigned long long)n,
            r < 0 ? "unknown (no latch: the layer gets a fresh pose)" : (r & 3) ? "differ from their latch" : "from two frames",
            (unsigned long long)id[0], (unsigned long long)id[1]);
    return r;
#else
    return -1;
#endif
}

void stereo_cameras_init(void) {
#ifdef LARECOMP_HAVE_XR
    const char *t = getenv("LARECOMP_STEREO_TEST");
    cam_override_stereo(eye_fn, t && !strcmp(t, "identity"));
#endif
}

// ---------------------------------------------------------------- stats
void stereo_note_gpu(uint64_t frame, double ms0, double ms1, int lists) {
    (void)frame;
#ifdef LARECOMP_HAVE_XR
    xr_note_eye_gpu((float)ms0, (float)ms1);
#endif
    static double s0, s1, mx, t_last;
    static int n, nl;
    s0 += ms0, s1 += ms1, n++, nl += lists;
    if (ms0 + ms1 > mx) mx = ms0 + ms1;
    double now = plat_now_ns() / 1e9;
    if (t_last == 0) t_last = now;
    if (now - t_last >= 2.0 && n) {
        LOG("[stereo] GPU per game frame: eye 0 %.2f ms, eye 1 %.2f ms, both %.2f ms (max %.2f) over %d frames; %.1f lists per frame\n",
            s0 / n, s1 / n, (s0 + s1) / n, mx, n, (double)nl / n);
        s0 = s1 = mx = 0, n = nl = 0, t_last = now;
    }
}
