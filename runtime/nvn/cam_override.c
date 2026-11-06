// Free-camera probe: see cam_override.h and docs/af/FREECAM.md.
#include "cam_override.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CAM_STANDALONE  // tools/test_cam_override.c: no runtime
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#include "../rt.h"
#endif

volatile int g_cam_on, g_cam_cull, g_cam_log;

// ---------------------------------------------------------------- matrix maths (row-major, column vectors)
#pragma clang fp contract(off)
void cam_mat_mul(float *out, const float *a, const float *b, bool fused) {
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = a[r * 4] * b[c];
            if (fused) {
                s = fmaf(a[r * 4 + 0], b[0 * 4 + c], 0.0f);
                for (int k = 1; k < 4; k++) s = fmaf(a[r * 4 + k], b[k * 4 + c], s);
            } else {
                for (int k = 1; k < 4; k++) s = s + a[r * 4 + k] * b[k * 4 + c];
            }
            t[r * 4 + c] = s;
        }
    memcpy(out, t, sizeof t);
}
#pragma clang fp contract(on)

bool cam_mat_inv(float *out, const float *mf) {  // general 4x4 (cofactors, in double)
    double m[16], inv[16], det;
    for (int i = 0; i < 16; i++) m[i] = mf[i];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(fabs(det) > 1e-30)) return false;
    for (int i = 0; i < 16; i++) out[i] = (float)(inv[i] / det);
    return true;
}

static void v3sub(float *o, const float *a, const float *b) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static float v3dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v3cross(float *o, const float *a, const float *b) {
    float t[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    memcpy(o, t, sizeof t);
}
static void v3norm(float *v) {
    float l = sqrtf(v3dot(v, v));
    if (l > 1e-20f) v[0] /= l, v[1] /= l, v[2] /= l;
}

// GL lookAt: rows right, up, back; translation -R * eye. The game's view looks down -z (its projection has w = -z).
void cam_look_at(float *v, const float eye[3], const float at[3], const float up[3]) {
    float f[3], s[3], u[3];
    v3sub(f, at, eye);
    v3norm(f);
    v3cross(s, f, up);
    v3norm(s);
    v3cross(u, s, f);
    memset(v, 0, 16 * sizeof *v);
    for (int i = 0; i < 3; i++) v[i] = s[i], v[4 + i] = u[i], v[8 + i] = -f[i];
    v[3] = -v3dot(s, eye), v[7] = -v3dot(u, eye), v[11] = v3dot(f, eye), v[15] = 1;
}

void cam_derive(CamRef *r, const float *v, const float *p, const float *vp) {
    memcpy(r->V, v, sizeof r->V);
    memcpy(r->P, p, sizeof r->P);
    if (vp) memcpy(r->VP, vp, sizeof r->VP);
    else cam_mat_mul(r->VP, p, v, false);
    r->ok = cam_mat_inv(r->iV, r->V) && cam_mat_inv(r->iP, r->P) && cam_mat_inv(r->iVP, r->VP);
    for (int i = 0; i < 3; i++) {
        r->right[i] = v[i], r->up[i] = v[4 + i], r->fwd[i] = -v[8 + i];
        r->eye[i] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
    }
}

// ---------------------------------------------------------------- configuration
static CamCfg g_cfgs[2], g_base;
static atomic_int g_cfg_idx;
static struct { uint64_t frame; CamCfg cfg; } g_sched[256];
static int g_nsched, g_sched_next;
static int g_log_every = 60;

static const CamCfg *cfg_now(void) { return &g_cfgs[atomic_load(&g_cfg_idx)]; }
static void cfg_set(const CamCfg *c) {
    int n = 1 - atomic_load(&g_cfg_idx);
    g_cfgs[n] = *c;
    g_cam_cull = c->cull_aspect > 0 || c->cull_fovy != 1.0f;
    atomic_store(&g_cfg_idx, n);
}

static bool parse_cull(const char *s, float *asp, float *fs) {
    *asp = 0, *fs = 1;
    if (!s || !*s || !strcmp(s, "stock")) return true;
    char *e;
    float a = strtof(s, &e);
    if (e == s || (*e && *e != ':') || a < 0) return false;
    float f = 1;
    if (*e == ':') { f = strtof(e + 1, &e); if (*e || !(f > 0.1f && f < 4)) return false; }
    *asp = a, *fs = f;
    return true;
}

bool cam_parse_mode(const char *s, CamCfg *c) {
    float a[4] = {0};
    int n = 0;
    char kind[16];
    const char *p = strchr(s, ':');
    size_t kl = p ? (size_t)(p - s) : strlen(s);
    if (kl == 0 || kl >= sizeof kind) return false;
    memcpy(kind, s, kl), kind[kl] = 0;
    while (p && *p == ':' && n < 4) {
        char *e;
        a[n] = strtof(p + 1, &e);
        if (e == p + 1) return false;
        n++, p = e;
    }
    if (p && *p) return false;
    if (!strcmp(kind, "off") && n == 0) c->kind = CAM_OFF;
    else if (!strcmp(kind, "identity") && n == 0) c->kind = CAM_IDENTITY;
    else if (!strcmp(kind, "passthrough") && n == 0) c->kind = CAM_PASSTHROUGH;
    else if (!strcmp(kind, "orbit") && (n == 3 || n == 4)) c->kind = CAM_ORBIT, c->yaw = a[0], c->pitch = a[1], c->dist = a[2], c->fovy = n == 4 ? a[3] : 0;
    else if (!strcmp(kind, "spin") && n == 3) c->kind = CAM_SPIN, c->spin = a[0], c->pitch = a[1], c->dist = a[2], c->fovy = 0;
    else return false;
    return true;
}

// <frame>:<mode>[/<cull>],...
static void parse_schedule(const char *s) {
    while (s && *s && g_nsched < 256) {
        char *e;
        unsigned long long f = strtoull(s, &e, 10);
        if (e == s || *e != ':') { LOG("[freecam] LARECOMP_FREECAM_AT: bad entry at '%s'\n", s); return; }
        const char *m = e + 1, *end = strchr(m, ',');
        char buf[96];
        size_t n = end ? (size_t)(end - m) : strlen(m);
        if (n >= sizeof buf) return;
        memcpy(buf, m, n), buf[n] = 0;
        char *cull = strchr(buf, '/');
        if (cull) *cull++ = 0;
        CamCfg c = g_base;
        if (!cam_parse_mode(buf, &c) || (cull && !parse_cull(cull, &c.cull_aspect, &c.cull_fovy))) {
            LOG("[freecam] LARECOMP_FREECAM_AT: bad mode '%s'\n", buf);
            return;
        }
        g_sched[g_nsched].frame = f, g_sched[g_nsched].cfg = c, g_nsched++;
        s = end ? end + 1 : NULL;
    }
}

static const char *kind_name(CamKind k) {
    static const char *n[] = {"off", "identity", "passthrough", "orbit", "spin"};
    return n[k];
}

void cam_override_init(void) {
    memset(&g_base, 0, sizeof g_base);
    g_base.cull_fovy = 1;
    g_nsched = g_sched_next = 0;
    const char *e;
    if ((e = getenv("LARECOMP_FREECAM_PIVOT"))) g_base.pivot_link = !strcmp(e, "link");
    if ((e = getenv("LARECOMP_FREECAM_FAR"))) g_base.far_add = (float)atof(e);
    g_base.fix48 = (e = getenv("LARECOMP_FREECAM_FIX48")) && atoi(e) != 0;
    if ((e = getenv("LARECOMP_FREECAM_CULL")) && !parse_cull(e, &g_base.cull_aspect, &g_base.cull_fovy)) {
        LOG("[freecam] LARECOMP_FREECAM_CULL: bad value '%s' (<aspect>:<fovy scale>)\n", e);
        g_base.cull_aspect = 0, g_base.cull_fovy = 1;
    }
    bool on = false;
    if ((e = getenv("LARECOMP_FREECAM")) && *e) {
        if (cam_parse_mode(e, &g_base)) on = true;
        else LOG("[freecam] LARECOMP_FREECAM: bad value '%s'\n", e);
    }
    if ((e = getenv("LARECOMP_FREECAM_AT")) && *e) parse_schedule(e), on = true;
    g_cam_log = (e = getenv("LARECOMP_FREECAM_LOG")) && atoi(e) != 0;
    if ((e = getenv("LARECOMP_FREECAM_LOG_EVERY")) && atoi(e) > 0) g_log_every = atoi(e);
    cfg_set(&g_base);
    g_cam_on = on || g_cam_log;
    if (g_cam_on || g_cam_cull)
        LOG("[freecam] mode %s yaw %g pitch %g dist %g fovy %g pivot %s far +%g fix48 %d cull %g:%g log %d (%d scheduled)\n",
            kind_name(g_base.kind), g_base.yaw, g_base.pitch, g_base.dist, g_base.fovy, g_base.pivot_link ? "link" : "screen",
            g_base.far_add, g_base.fix48, g_base.cull_aspect, g_base.cull_fovy, g_cam_log, g_nsched);
}
#ifndef CAM_STANDALONE
__attribute__((constructor)) static void cam_override_ctor(void) { cam_override_init(); }
#endif

void cam_override_frame(uint64_t frame) {
    while (g_sched_next < g_nsched && g_sched[g_sched_next].frame <= frame) {
        const CamCfg *c = &g_sched[g_sched_next++].cfg;
        cfg_set(c);
        if (g_cam_log || getenv("LARECOMP_FREECAM_LOG_SCHED"))
            LOG("[freecam] f%llu schedule: %s yaw %g pitch %g dist %g fovy %g cull %g:%g\n", (unsigned long long)frame, kind_name(c->kind),
                c->yaw, c->pitch, c->dist, c->fovy, c->cull_aspect, c->cull_fovy);
    }
}

float cam_override_cull_aspect(void) { return g_cam_cull ? cfg_now()->cull_aspect : 0.0f; }
float cam_override_fovy_scale(void) { return g_cam_cull ? cfg_now()->cull_fovy : 1.0f; }

// ---------------------------------------------------------------- published game state
static float g_link[3], g_zlo[3], g_zhi[3];
static volatile int g_link_ok, g_zone_ok;
void cam_override_set_link(const float pos[3]) { if (pos) memcpy(g_link, pos, sizeof g_link), g_link_ok = 1; else g_link_ok = 0; }
void cam_override_set_zone(const float lo[3], const float hi[3]) {
    if (lo && hi) memcpy(g_zlo, lo, sizeof g_zlo), memcpy(g_zhi, hi, sizeof g_zhi), g_zone_ok = 1;
    else g_zone_ok = 0;
}

// ---------------------------------------------------------------- the latched camera
static CamLatch g_latch;
enum { B_NONE, B_V, B_P, B_VP, B_IV, B_IP, B_IVP, B_T = 8 };
enum { G_NONE, G_EYE, G_FWD, G_RIGHT, G_UP };
static const char *const blk_name[] = {"-", "view", "proj", "view-proj", "inv(view)", "inv(proj)", "inv(view-proj)"};
static const char *const grp_name[] = {"-", "eye position", "forward", "right", "up"};

static inline bool ne(float a, float b) { return fabsf(a - b) <= 1e-4f * (1.0f + fabsf(b)); }
static bool persp(const float *u) { return u[12] == 0 && u[13] == 0 && u[14] == 0 && u[15] == 1 && u[30] == -1 && u[31] == 0; }
static void transpose(float *o, const float *m) { for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[c * 4 + r] = m[r * 4 + c]; }

static const float *ref_mat(const CamRef *r, int kind) {
    switch (kind) {
    case B_V: return r->V; case B_P: return r->P; case B_VP: return r->VP;
    case B_IV: return r->iV; case B_IP: return r->iP; case B_IVP: return r->iVP;
    }
    return NULL;
}
static void ref_cand(float c[12][16], const CamRef *r) {
    for (int k = 1; k <= 6; k++) memcpy(c[k - 1], ref_mat(r, k), 64), transpose(c[6 + k - 1], ref_mat(r, k));
}
static int cand_kind(int i) { return (i % 6 + 1) | (i >= 6 ? B_T : 0); }

static int classify_block(const float *m, const CamRef *r) {
    float c[12][16];
    ref_cand(c, r);
    for (int i = 0; i < 12; i++) {
        int j;
        for (j = 0; j < 16 && ne(m[j], c[i][j]); j++) {}
        if (j == 16) return cand_kind(i);
    }
    return B_NONE;
}
static int classify_group(const float *g, const CamRef *r) {
    const float *v[4] = {NULL, r->eye, r->fwd, r->right};
    for (int k = 1; k < 4; k++) if (ne(g[0], v[k][0]) && ne(g[1], v[k][1]) && ne(g[2], v[k][2])) return k;
    if (ne(g[0], r->up[0]) && ne(g[1], r->up[1]) && ne(g[2], r->up[2])) return G_UP;
    return G_NONE;
}

void cam_latch_from(CamLatch *l, const float *u, uint64_t addr, uint64_t frame) {
    memset(l, 0, sizeof *l);
    memcpy(l->orig, u, sizeof l->orig);
    l->addr = addr, l->frame = frame;
    cam_derive(&l->ref, u, u + 16, u + 32);
    // the game's own product: plain or fused? (identity then rebuilds VP bit-exactly)
    float mu[16], mf[16];
    cam_mat_mul(mu, u + 16, u, false);
    cam_mat_mul(mf, u + 16, u, true);
    l->vp_fused = !memcmp(mu, u + 32, 64) ? 0 : !memcmp(mf, u + 32, 64) ? 1 : -1;
    memcpy(l->id_vp, l->vp_fused == 0 ? mu : l->vp_fused == 1 ? mf : u + 32, 64);
    const float *p = u + 16;
    l->near_ = p[11] / (p[10] - 1), l->far_ = p[11] / (p[10] + 1);
    l->fovy_game = 2 * atanf(1.0f / p[5]);
    for (int b = 0; b < 2; b++) l->blk[b] = (uint8_t)classify_block(u + 48 + 16 * b, &l->ref);
    for (int g = 0; g < 8; g++) l->grp[g] = l->blk[g / 4] ? G_NONE : (uint8_t)classify_group(u + 48 + 4 * g, &l->ref);
    l->valid = l->ref.ok;
}

// the point the orbit turns around: the centre of the screen on the floor (y 0; side-view zones: their middle plane z),
// or Link
bool cam_pivot(const CamRef *r, const float link[3], bool link_ok, bool zone_ok, const float zlo[3], const float zhi[3], bool use_link,
               float out[3]) {
    if (use_link && link_ok) { memcpy(out, link, 3 * sizeof *out); return true; }
    bool side = zone_ok ? zhi[2] - zlo[2] < 6.0f : fabsf(r->fwd[1]) < 0.2f;  // (as widescreen.c: thin in z = side view)
    float t = -1;
    if (side && fabsf(r->fwd[2]) > 1e-3f) t = ((zone_ok ? (zlo[2] + zhi[2]) * 0.5f : 0.0f) - r->eye[2]) / r->fwd[2];
    else if (!side && r->fwd[1] < -0.05f) t = -r->eye[1] / r->fwd[1];
    bool hit = t > 0;
    if (!hit) t = 20.0f;  // (no floor / plane in front: a point 20 units ahead)
    for (int i = 0; i < 3; i++) out[i] = r->eye[i] + t * r->fwd[i];
    return hit;
}

static void block_write(float *dst, int kind, const CamRef *n) {
    const float *m = ref_mat(n, kind & 7);
    if (kind & B_T) transpose(dst, m);
    else memcpy(dst, m, 64);
}

// the substitute for the camera buffer: guest = the live 80 floats, out = the 80 floats to bind
void cam_build(const CamLatch *l, const CamCfg *c, const float *guest, uint64_t frame, const float pivot[3], float *out) {
    memcpy(out, guest, 80 * sizeof *out);
    float V[16], P[16], VP[16];
    const CamRef *o = &l->ref;
    if (c->kind == CAM_IDENTITY || c->kind == CAM_PASSTHROUGH) {
        memcpy(V, o->V, 64), memcpy(P, o->P, 64);
        memcpy(VP, c->kind == CAM_IDENTITY ? l->id_vp : o->VP, 64);
    } else {
        float d[3] = {o->eye[0] - pivot[0], o->eye[1] - pivot[1], o->eye[2] - pivot[2]};
        float dist = c->dist > 0 ? c->dist : sqrtf(v3dot(d, d));
        float yaw = (c->kind == CAM_SPIN ? c->spin * (float)frame : c->yaw) * (float)(M_PI / 180.0);
        float pitch = fminf(fmaxf(c->pitch, -89.0f), 89.0f) * (float)(M_PI / 180.0);
        float eye[3] = {pivot[0] + dist * cosf(pitch) * sinf(yaw), pivot[1] + dist * sinf(pitch), pivot[2] + dist * cosf(pitch) * cosf(yaw)};
        static const float up[3] = {0, 1, 0};
        cam_look_at(V, eye, pivot, up);
        memcpy(P, o->P, 64);
        float fs = c->cull_aspect > 0 || c->cull_fovy != 1.0f ? c->cull_fovy : 1.0f;
        float fov = c->fovy > 0 ? c->fovy * (float)(M_PI / 180.0) : l->fovy_game / fs;  // (the game's own angle, before the culling scale)
        if (fabsf(fov - l->fovy_game) > 1e-6f) {
            float aspect = o->P[5] / o->P[0];
            P[5] = 1.0f / tanf(fov * 0.5f), P[0] = P[5] / aspect;
        }
        if (c->far_add != 0) {  // GL depth: P22 = -(f+n)/(f-n), P23 = -2fn/(f-n)
            float f = l->far_ + c->far_add, n = l->near_;
            P[10] = -(f + n) / (f - n), P[11] = -2 * f * n / (f - n);
        }
        cam_mat_mul(VP, P, V, l->vp_fused == 1);
    }
    memcpy(out, V, 64), memcpy(out + 16, P, 64), memcpy(out + 32, VP, 64);
    if (c->fix48 && c->kind != CAM_IDENTITY && c->kind != CAM_PASSTHROUGH) {
        CamRef n;
        cam_derive(&n, V, P, VP);
        for (int b = 0; b < 2; b++) if (l->blk[b]) block_write(out + 48 + 16 * b, l->blk[b], &n);
        for (int g = 0; g < 8; g++) {
            const float *v = l->grp[g] == G_EYE ? n.eye : l->grp[g] == G_FWD ? n.fwd : l->grp[g] == G_RIGHT ? n.right : l->grp[g] == G_UP ? n.up : NULL;
            if (v) memcpy(out + 48 + 4 * g, v, 3 * sizeof *out);
        }
    }
}

// ---------------------------------------------------------------- probe log
static uint64_t g_draws, g_draw_frame = ~0ull;
static bool g_scan;                         // this frame is a full-scan frame
static unsigned g_id_n[3];     // identity: VP rebuilt plain / fused / neither
static unsigned g_nsusp[3];                 // matrices / other cameras / screen-space
static uint64_t g_seen[8192];

static bool seen_new(uint64_t key) {  // (true the first time)
    key |= 1;
    for (unsigned i = (unsigned)(key * 0x9E3779B97F4A7C15ull >> 51); ; i = (i + 1) & 8191) {
        if (g_seen[i] == key) return false;
        if (!g_seen[i]) { g_seen[i] = key; return true; }
    }
}
static uint64_t mix(uint64_t h, uint64_t v) { h = (h ^ v) * 0x9E3779B97F4A7C15ull; return h ^ (h >> 29); }

static void dump48(const CamLatch *l) {
    const float *u = l->orig;
    LOG("[freecam] f%llu camera @%llx: eye (%.3f %.3f %.3f) fwd (%.3f %.3f %.3f) near %.4g far %.4g fovy %.2f deg aspect %.4f; floats 48-79:\n",
        (unsigned long long)l->frame, (unsigned long long)l->addr, l->ref.eye[0], l->ref.eye[1], l->ref.eye[2], l->ref.fwd[0], l->ref.fwd[1],
        l->ref.fwd[2], l->near_, l->far_, l->fovy_game * (180.0f / (float)M_PI), l->ref.P[5] / l->ref.P[0]);
    for (int r = 0; r < 8; r++)
        LOG("[freecam]   %2d: %12.5g %12.5g %12.5g %12.5g\n", 48 + 4 * r, u[48 + 4 * r], u[49 + 4 * r], u[50 + 4 * r], u[51 + 4 * r]);
    for (int b = 0; b < 2; b++) {
        bool zero = true;
        for (int i = 0; i < 16; i++) zero &= u[48 + 16 * b + i] == 0;
        LOG("[freecam]   floats %d-%d: %s%s\n", 48 + 16 * b, 63 + 16 * b,
            l->blk[b] ? blk_name[l->blk[b] & 7] : zero ? "all zero" : "unknown", l->blk[b] & B_T ? " (transposed)" : "");
    }
    for (int g = 0; g < 8; g++) if (l->grp[g]) LOG("[freecam]   floats %d-%d: %s (xyz)\n", 48 + 4 * g, 51 + 4 * g, grp_name[l->grp[g]]);
    g_id_n[l->vp_fused < 0 ? 2 : l->vp_fused]++;
    float md = 0;
    for (int i = 0; i < 16; i++) md = fmaxf(md, fabsf(l->orig[32 + i] - l->id_vp[i]));
    LOG("[freecam]   rebuilt P*V %s the game's VP (%s; frames so far: plain %u, fused %u, neither %u; max delta this frame %g)\n",
        l->vp_fused >= 0 ? "equals" : "DIFFERS from", l->vp_fused == 0 ? "plain product" : l->vp_fused == 1 ? "fused product" : "no rounding mode matched",
        g_id_n[0], g_id_n[1], g_id_n[2], md);
}

void cam_override_latch(const float *u, uint64_t addr, uint64_t size, uint64_t frame) {
    if (size != 320 || (g_latch.valid && g_latch.frame == frame)) return;
    if (!persp(u)) return;
    cam_latch_from(&g_latch, u, addr, frame);
    if (!g_cam_log) return;
    g_scan = frame % (uint64_t)g_log_every == 0;
    if (g_scan) {
        LOG("[freecam] f%llu previous frame: %llu draws\n", (unsigned long long)frame, (unsigned long long)g_draws);
        dump48(&g_latch);
        if (frame % ((uint64_t)g_log_every * 10) == 0)
            LOG("[freecam] f%llu suspects so far: %u matrix copies, %u other cameras, %u screen-space draws\n", (unsigned long long)frame, g_nsusp[0],
                g_nsusp[1], g_nsusp[2]);
    }
}

bool cam_override_ubo(int stage, uint64_t addr, uint64_t size, void *out320) {
    (void)stage;
    const CamCfg *c = cfg_now();
    if (c->kind == CAM_OFF || !g_latch.valid || addr != g_latch.addr || size != 320) return false;
    const float *g = (const float *)(uintptr_t)addr;
    if (memcmp(g, g_latch.orig, 128)) return false;  // (not this frame's camera: the address was rewritten with another one)
    float pv[3];
    cam_pivot(&g_latch.ref, g_link, g_link_ok, g_zone_ok, g_zlo, g_zhi, c->pivot_link, pv);
    cam_build(&g_latch, c, g, g_latch.frame, pv, (float *)out320);
    return true;
}

// Any other bound buffer (<= 4 KB): the game's passes also keep the camera's matrices in their own scene blocks (a 1 KB
// block at slot 1 holds the view-projection at 0, the view / inverse view as 3x4 at 144 / 192, inverse projection, inverse
// view-projection and projection after them: the level's vertex shaders read the view-projection there, not from the
// 320-byte camera), so the substitute camera has to be written there too. Every 16-byte-aligned block equal to one of the
// latched camera's matrices (or its transpose; a 3x4 copy: rows 0-2 only) is replaced by the substitute camera's, and
// every 4-float group whose xyz is the eye position gets the substitute eye. The copy is made only when something matched.
bool cam_override_patch(const void *guest, uint64_t size, void *out) {
    const CamCfg *c = cfg_now();
    if (c->kind == CAM_OFF || c->kind == CAM_IDENTITY || c->kind == CAM_PASSTHROUGH || !g_latch.valid || size < 48 || size > 4096) return false;
    static const CamLatch *sub_for;
    static uint64_t sub_frame = ~0ull;
    static const CamCfg *sub_cfg;
    static CamRef sub;  // the substitute camera's matrices (per frame and configuration)
    float pv[3];
    if (sub_for != &g_latch || sub_frame != g_latch.frame || sub_cfg != c) {
        float built[80];
        cam_pivot(&g_latch.ref, g_link, g_link_ok, g_zone_ok, g_zlo, g_zhi, c->pivot_link, pv);
        cam_build(&g_latch, c, g_latch.orig, g_latch.frame, pv, built);
        cam_derive(&sub, built, built + 16, built + 32);
        sub_for = &g_latch, sub_frame = g_latch.frame, sub_cfg = c;
    }
    if (!sub.ok) return false;
    const float *u = guest;
    float cand[12][16];
    ref_cand(cand, &g_latch.ref);
    bool did = false;
    float *o = out;
    unsigned n = (unsigned)size;
    for (unsigned off = 0; off + 16 <= n; off += 16) {
        const float *f = u + off / 4;
        if (off + 48 <= n) {
            for (int i = 0; i < 12; i++) {
                int j;
                for (j = 0; j < 12 && ne(f[j], cand[i][j]); j++) {}
                if (j < 12) continue;
                bool full = off + 64 <= n;
                for (int k = 12; full && k < 16; k++) full = ne(f[k], cand[i][k]);
                if (!did) memcpy(out, guest, size), did = true;
                float m[16];
                block_write(m, cand_kind(i), &sub);
                memcpy(o + off / 4, m, (full ? 16 : 12) * sizeof *m);
                break;
            }
        }
        if (ne(f[0], g_latch.ref.eye[0]) && ne(f[1], g_latch.ref.eye[1]) && ne(f[2], g_latch.ref.eye[2]) && !(f[0] == 0 && f[1] == 0 && f[2] == 0)) {
            if (!did) memcpy(out, guest, size), did = true;
            memcpy(o + off / 4, sub.eye, 3 * sizeof *o);
        }
    }
    return did;
}

// ---- matrix scan
int cam_scan_matrices(const float *u, unsigned size, const CamRef *r, CamMatchFn cb, void *user) {
    float c[12][16];
    ref_cand(c, r);
    int n = 0;
    for (unsigned off = 0; off + 48 <= size; off += 16) {
        const float *f = u + off / 4;
        for (int i = 0; i < 12; i++) {
            int j;
            for (j = 0; j < 12 && ne(f[j], c[i][j]); j++) {}
            if (j < 12) continue;
            bool full = off + 64 <= size;
            for (int k = 12; full && k < 16; k++) full = ne(f[k], c[i][k]);
            char w[64];
            snprintf(w, sizeof w, "%s%s%s", blk_name[cand_kind(i) & 7], i >= 6 ? "^T" : "", full ? "" : " (rows 0-2 only)");
            cb(user, off, w);
            n++;
        }
    }
    return n;
}

static uint64_t fnv(const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h ^= b[i], h *= 0x100000001B3ull;
    return h;
}

typedef struct { const CamDrawInfo *d; int stage, idx; } ScanCtx;
static void scan_hit(void *user, unsigned off, const char *what) {
    ScanCtx *s = user;
    uint64_t k = mix(mix(mix(mix(1, s->d->fs_hash), s->d->vs_hash), (uint64_t)s->stage << 8 | (uint64_t)s->idx), ((uint64_t)off << 8 | (fnv(what, strlen(what)) & 0xff)));
    if (!seen_new(k)) return;
    g_nsusp[0]++;
    LOG("[freecam] SUSPECT matrix: fs %016llx vs %016llx stage %d cbuf %d (slot %d) offset %u: %s\n", (unsigned long long)s->d->fs_hash,
        (unsigned long long)s->d->vs_hash, s->stage, s->idx + 3, s->idx, off, what);
}

void cam_override_draw(const CamDrawInfo *d) {
    if (d->frame != g_draw_frame) g_draw_frame = d->frame, g_draws = 0;
    g_draws++;
    if (!g_scan || !g_latch.valid || g_latch.frame != d->frame) return;
    bool main_bound = false;
    for (int stage = 0; stage < 2; stage++)
        for (int i = 0; i < 18; i++) {
            uint64_t addr = d->ubo[stage][2 * i], size = d->ubo[stage][2 * i + 1];
            if (!addr || (addr & 3) || size < 48) continue;
            if (addr == g_latch.addr && size == 320) {
                main_bound = true;
                if (!(stage == 0 && i == 5) && seen_new(mix(mix(2, d->fs_hash), (uint64_t)stage << 8 | (uint64_t)i)))
                    LOG("[freecam] main camera bound at stage %d cbuf %d (slot %d): fs %016llx vs %016llx\n", stage, i + 3, i, (unsigned long long)d->fs_hash,
                        (unsigned long long)d->vs_hash);
                continue;
            }
            const float *u = (const float *)(uintptr_t)addr;
            if (size == 320 && persp(u) && seen_new(mix(mix(mix(3, d->fs_hash), d->vs_hash), (uint64_t)stage << 8 | (uint64_t)i))) {
                g_nsusp[1]++;
                LOG("[freecam] SUSPECT other camera: fs %016llx vs %016llx stage %d cbuf %d (slot %d): 320-byte perspective buffer %s, eye-forward z %.3f\n",
                    (unsigned long long)d->fs_hash, (unsigned long long)d->vs_hash, stage, i + 3, i,
                    addr == g_latch.addr ? "at the main camera's address but other contents" : "elsewhere", -u[10]);
            }
            ScanCtx s = {d, stage, i};
            cam_scan_matrices(u, size > 4096 ? 4096 : (unsigned)size, &g_latch.ref, scan_hit, &s);
        }
    if (d->into_scalable && d->samples_depth && !main_bound && seen_new(mix(mix(4, d->fs_hash), d->vs_hash))) {
        g_nsusp[2]++;
        LOG("[freecam] SUSPECT screen-space: fs %016llx vs %016llx samples depth into a scalable target, no main camera bound\n",
            (unsigned long long)d->fs_hash, (unsigned long long)d->vs_hash);
    }
}
