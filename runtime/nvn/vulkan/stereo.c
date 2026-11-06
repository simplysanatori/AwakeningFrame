// Stereo executor: switches and the v1 diorama's eye cameras (stereo.h).
#include "stereo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../platform.h"
#include "../../rt.h"
#include "../cam_override.h"
#ifdef LARECOMP_HAVE_XR
#include "../../xr/xr.h"
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

// ---------------------------------------------------------------- the v1 diorama (fixed table; lane 6: the rig)
// World (game units) to the app's LOCAL space (metres; origin = the head at the session's start, y up, -z ahead):
//   app = A + s * Ry(yaw) * (world - pivot)
// pivot = the game camera's screen centre on the floor (latched per game frame: the table follows the game's camera,
// moving only on new game frames). A = the table point the pivot sits on: LARECOMP_VR_DIST metres from the head at
// LARECOMP_VR_PITCH degrees below the horizon (default: the game camera's own pitch at the first frame, so the first
// look at the table is the game's view), or LARECOMP_VR_TABLE=x,y,z. s = LARECOMP_VR_SCALE metres per game unit
// (0.067: a 1.5-unit tile ~10 cm). The eye cameras keep the game's depth terms (near / far: the shaders that linearise
// depth with the game's constants stay right) unless LARECOMP_VR_NEAR=<units>.
static struct {
    bool init, have_pitch, have_table;
    float s, dist, pitch, yaw, near_;
    float A[3];
} cfg;

static void cfg_init(void) {
    const char *e;
    cfg.init = true;
    cfg.s = (e = getenv("LARECOMP_VR_SCALE")) && atof(e) > 0 ? (float)atof(e) : 0.067f;
    cfg.dist = (e = getenv("LARECOMP_VR_DIST")) && atof(e) > 0 ? (float)atof(e) : 1.0f;
    cfg.yaw = (e = getenv("LARECOMP_VR_YAW")) ? (float)atof(e) : 0.0f;
    cfg.near_ = (e = getenv("LARECOMP_VR_NEAR")) ? (float)atof(e) : 0.0f;
    if ((e = getenv("LARECOMP_VR_PITCH"))) cfg.pitch = (float)atof(e), cfg.have_pitch = true;
    if ((e = getenv("LARECOMP_VR_TABLE")) && sscanf(e, "%f,%f,%f", &cfg.A[0], &cfg.A[1], &cfg.A[2]) == 3) cfg.have_table = true;
}

static void quat_mat(const float q[4], float m[3][3]) {  // x y z w -> rotation (columns: the eye's axes in app space)
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z), m[0][1] = 2 * (x * y - z * w), m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w), m[1][1] = 1 - 2 * (x * x + z * z), m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w), m[2][1] = 2 * (y * z + x * w), m[2][2] = 1 - 2 * (x * x + y * y);
}

static uint64_t g_logged_frame;

#ifdef LARECOMP_HAVE_XR
static bool eye_fn(const CamLatch *l, const float pivot[3], float V[2][16], float P[2][16]) {
    XrEyeView ev[2];
    if (!xr_latch_views_early(ev)) return false;
    if (!cfg.init) cfg_init();
    if (!cfg.have_table) {
        if (!cfg.have_pitch) {
            float p = asinf(fminf(fmaxf(-l->ref.fwd[1], -1.0f), 1.0f)) * (float)(180.0 / M_PI);
            cfg.pitch = fminf(fmaxf(p, 25.0f), 65.0f), cfg.have_pitch = true;
        }
        float pr = cfg.pitch * (float)(M_PI / 180.0);
        cfg.A[0] = 0, cfg.A[1] = -cfg.dist * sinf(pr), cfg.A[2] = -cfg.dist * cosf(pr);
        cfg.have_table = true;
        LOG("[stereo] diorama: %.3f m per unit, pivot at (%.2f %.2f %.2f) m in the head's start space (%.0f deg down, %.2f m), yaw %.0f\n",
            cfg.s, cfg.A[0], cfg.A[1], cfg.A[2], cfg.pitch, cfg.dist, cfg.yaw);
    }
    float cy = cosf(-cfg.yaw * (float)(M_PI / 180.0)), sy = sinf(-cfg.yaw * (float)(M_PI / 180.0));  // Ry(-yaw): app -> world
    const float *gp = l->ref.P;
    for (int e = 0; e < 2; e++) {
        float m[3][3], w[3][3];
        quat_mat(ev[e].orientation, m);
        for (int c = 0; c < 3; c++) {  // world rotation = Ry(-yaw) * m
            w[0][c] = cy * m[0][c] + sy * m[2][c];
            w[1][c] = m[1][c];
            w[2][c] = -sy * m[0][c] + cy * m[2][c];
        }
        float d[3] = {(ev[e].position[0] - cfg.A[0]) / cfg.s, (ev[e].position[1] - cfg.A[1]) / cfg.s, (ev[e].position[2] - cfg.A[2]) / cfg.s};
        float E[3] = {pivot[0] + cy * d[0] + sy * d[2], pivot[1] + d[1], pivot[2] - sy * d[0] + cy * d[2]};
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
        if (e == 0 && l->frame >= g_logged_frame + 600) {
            g_logged_frame = l->frame;
            LOG("[stereo] f%llu pivot (%.2f %.2f %.2f) eye0 (%.2f %.2f %.2f) units; game eye (%.2f %.2f %.2f) near %.3g far %.3g\n",
                (unsigned long long)l->frame, pivot[0], pivot[1], pivot[2], E[0], E[1], E[2], l->ref.eye[0], l->ref.eye[1], l->ref.eye[2],
                l->near_, l->far_);
        }
    }
    return true;
}
#endif

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
