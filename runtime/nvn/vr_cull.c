// VR culling fitted to the table (vr_cull.h). The game culls on its game thread with its own camera, aspect (camera field
// +0x570, rt_cull_aspect) and vertical fov (+0x564, rt_fovy). In VR that camera is never shown; what the eyes need is the
// diorama: a floor rectangle around the table centre (the stock screen footprint scaled by LARECOMP_VR_CULL_RECT, about the
// 1.2 x 1 screens the edge fade keeps), from LARECOMP_VR_CULL_HEIGHT below the floor to above it, cut by both eyes'
// frusta (widened by LARECOMP_VR_CULL_MARGIN in tangent, for head motion between the fit and the frame that uses it).
// Its corners, seen from the game camera, give the half-angle tangents the game's frustum needs. Never narrower than the
// game's own. The fit is made on the render thread at each latch and read on the game thread one or two frames later;
// it is camera-relative (the pivot follows the camera), so the lag does not move it.
#include "vr_cull.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rt.h"
#include "cam_override.h"

static struct {
    int init, mode, eyes;   // mode: VR_CULL_FIT / WIDE / STOCK
    float kx, kz, hlo, hhi, margin;
    float ex, ez, tmax;     // LARECOMP_VR_CULL_EXTENT (world half-extents about the table centre, 0 = off), _TMAX
    float ring, ring_cast;  // LARECOMP_VR_RING / _RING_CAST margins (0 = off)
} C;
static _Atomic float g_aspect, g_fs;   // the fitted culling aspect (absolute) and fovy scale; 0 = none yet
static _Atomic float g_fovy_orig = 0;  // the game's own fovy field (radians or degrees, as it is)
static _Atomic float g_aspect_orig = 0;

static void init(void) {
    const char *e;
    C.init = 1;
    C.mode = VR_CULL_FIT, C.eyes = 1;
    if ((e = getenv("LARECOMP_VR_CULL"))) C.mode = !strcmp(e, "wide") ? VR_CULL_WIDE : !strcmp(e, "stock") ? VR_CULL_STOCK : VR_CULL_FIT;
    C.kx = 1.2f, C.kz = 1.0f, C.hlo = 1.5f, C.hhi = 3.0f, C.margin = 0.3f;
    if ((e = getenv("LARECOMP_VR_CULL_RECT"))) sscanf(e, "%f,%f", &C.kx, &C.kz);
    if ((e = getenv("LARECOMP_VR_CULL_HEIGHT"))) sscanf(e, "%f,%f", &C.hlo, &C.hhi);
    if ((e = getenv("LARECOMP_VR_CULL_MARGIN"))) C.margin = (float)atof(e);
    if ((e = getenv("LARECOMP_VR_CULL_EYES"))) C.eyes = atoi(e) != 0;
    // (lane stream) LARECOMP_VR_CULL_EXTENT=<x>,<z>: the culled-to rectangle is the world-axis box of these half-extents about
    // the table centre instead of the scaled stock footprint, e.g. 52,42 for the overworld's resident 7x7 rooms around Link
    // (rooms are 15 x 12; docs/af/STREAM.md). LARECOMP_VR_CULL_TMAX=<tan>: the widest half-angle tangent (default 4, 76 deg).
    C.ex = C.ez = 0, C.tmax = 4.0f;
    if ((e = getenv("LARECOMP_VR_CULL_EXTENT"))) sscanf(e, "%f,%f", &C.ex, &C.ez);
    if ((e = getenv("LARECOMP_VR_CULL_TMAX"))) C.tmax = (float)atof(e);
    if (!(C.tmax > 0.5f)) C.tmax = 4.0f;
    // (lane ring) LARECOMP_VR_RING=<margin>: draws whose position is outside the table fit's footprint grown by this are
    // "ring" draws (exec.c: colour passes only, no shadow casters, no depth pre-pass). LARECOMP_VR_RING_CAST=<margin>:
    // ring draws within this of the footprint still cast shadows.
    C.ring = C.ring_cast = 0;
    if ((e = getenv("LARECOMP_VR_RING"))) C.ring = (float)atof(e);
    if ((e = getenv("LARECOMP_VR_RING_CAST"))) C.ring_cast = (float)atof(e);
    if (!(C.ring > 0)) C.ring = 0;
    if (!(C.ring_cast > 0)) C.ring_cast = 0;
}

// the table fit's footprint as a world-axis box (x, z), before the EXTENT override: written by vr_cull_fit (the render
// thread at each latch), read by the executor. A sequence count keeps the four values together.
static _Atomic unsigned g_core_seq;
static _Atomic float g_core[4];  // lo x, lo z, hi x, hi z
static _Atomic int g_core_ok;
static void core_set(const float corner[4][3], bool ok) {
    atomic_fetch_add(&g_core_seq, 1);
    if (ok) {
        float lx = corner[0][0], hx = lx, lz = corner[0][2], hz = lz;
        for (int c = 1; c < 4; c++)
            lx = fminf(lx, corner[c][0]), hx = fmaxf(hx, corner[c][0]), lz = fminf(lz, corner[c][2]), hz = fmaxf(hz, corner[c][2]);
        atomic_store(&g_core[0], lx), atomic_store(&g_core[1], lz), atomic_store(&g_core[2], hx), atomic_store(&g_core[3], hz);
    }
    atomic_store(&g_core_ok, ok);
    atomic_fetch_add(&g_core_seq, 1);
}

bool vr_cull_core_box(float lo[3], float hi[3]) {
    for (int tries = 0; tries < 8; tries++) {
        unsigned s0 = atomic_load(&g_core_seq);
        if (s0 & 1) continue;
        int ok = atomic_load(&g_core_ok);
        float b[4];
        for (int i = 0; i < 4; i++) b[i] = atomic_load(&g_core[i]);
        if (atomic_load(&g_core_seq) != s0) continue;
        if (!ok) return false;
        lo[0] = b[0], lo[1] = -1e30f, lo[2] = b[1], hi[0] = b[2], hi[1] = 1e30f, hi[2] = b[3];
        return true;
    }
    return false;
}

bool vr_ring_on(void) {
    if (!C.init) init();
    return C.ring > 0;
}

int vr_ring_class(const float blo[3], const float bhi[3], uint64_t frame) {
    if (!C.init) init();
    // one footprint per frame (the executor's thread only): a refit between a frame's depth pre-pass and its colour
    // pass must not turn a draw the pre-pass left out into one that tests EQUAL against it
    static float lo[3], hi[3];
    static bool ok;
    static uint64_t at = ~0ull;
    if (frame != at) at = frame, ok = vr_cull_core_box(lo, hi);
    if (!(C.ring > 0) || !ok) return 0;
    float m = C.ring, c = C.ring_cast;  // (the draw's box against the footprint grown by the margin, in x and z)
    if (bhi[0] >= lo[0] - m && blo[0] <= hi[0] + m && bhi[2] >= lo[2] - m && blo[2] <= hi[2] + m) return 0;
    if (c > 0 && bhi[0] >= lo[0] - c && blo[0] <= hi[0] + c && bhi[2] >= lo[2] - c && blo[2] <= hi[2] + c) return 2;
    return 1;
}

int vr_cull_mode(void) {
    if (!C.init) init();
    return C.mode;
}

void vr_cull_note_game(float fovy_field, float aspect_field) {
    if (fovy_field > 0) atomic_store(&g_fovy_orig, fovy_field);
    if (aspect_field > 0) atomic_store(&g_aspect_orig, aspect_field);
}

bool vr_cull_get(float *aspect, float *fovy_scale) {
    float a = atomic_load(&g_aspect), f = atomic_load(&g_fs);
    if (!(a > 0) || !(f > 0)) return false;
    *aspect = a, *fovy_scale = f;
    return true;
}

// ---- convex clipping: a polygon (<= 32 points) against the plane n.x + d >= 0
typedef struct { float p[32][3]; int n; } Poly;
static void clip(Poly *io, const float pl[4]) {
    Poly o = {.n = 0};
    for (int i = 0; i < io->n; i++) {
        const float *a = io->p[i], *b = io->p[(i + 1) % io->n];
        float da = pl[0] * a[0] + pl[1] * a[1] + pl[2] * a[2] + pl[3], db = pl[0] * b[0] + pl[1] * b[1] + pl[2] * b[2] + pl[3];
        if (da >= 0 && o.n < 32) memcpy(o.p[o.n++], a, 12);
        if ((da >= 0) != (db >= 0) && o.n < 32) {
            float t = da / (da - db);
            for (int k = 0; k < 3; k++) o.p[o.n][k] = a[k] + t * (b[k] - a[k]);
            o.n++;
        }
    }
    *io = o;
}

static void eye_planes(const CamRef *r, float m, float pl[4][4]) {  // left, right, bottom, top of VP, NDC widened by m
    const float *vp = r->VP;  // row-major, clip = VP * x
    for (int i = 0; i < 4; i++) {
        int row = i / 2;
        float s = i & 1 ? -1.0f : 1.0f;
        for (int k = 0; k < 4; k++) pl[i][k] = (1 + m) * vp[12 + k] + s * vp[row * 4 + k];
    }
}

void vr_cull_fit(const CamLatch *l, const float pivot[3], const float anchor[3], const CamRef *eye0, const CamRef *eye1) {
    if (!C.init) init();
    if (C.mode != VR_CULL_FIT || !l || !l->valid) return;
    const CamRef *g = &l->ref;
    float fo = atomic_load(&g_fovy_orig);
    if (!(fo > 0)) return;
    float fr = fo > 3.2f ? fo * (float)(M_PI / 180.0) : fo;  // (degrees or radians, as rt_fovy)
    float ao = atomic_load(&g_aspect_orig);
    if (!(ao > 0.1f && ao < 10.0f)) ao = 16.0f / 9.0f;
    float ty0 = tanf(fr * 0.5f), tx0 = ty0 * ao;
    float wide_a = 3.2f, wide_fs = 1.5f;
    if (fabsf(g->fwd[1]) < 0.26f) {  // side view (or a level look): no floor rectangle; the old fixed widening
        atomic_store(&g_aspect, wide_a), atomic_store(&g_fs, wide_fs);
        core_set(NULL, false);  // (no footprint: every draw is core)
        return;
    }
    // the stock footprint on the floor (y = pivot), scaled about the pivot along the camera's ground axes
    float hr[3] = {g->right[0], 0, g->right[2]}, hf[3] = {g->fwd[0], 0, g->fwd[2]};
    float lr = sqrtf(hr[0] * hr[0] + hr[2] * hr[2]), lf = sqrtf(hf[0] * hf[0] + hf[2] * hf[2]);
    if (lr < 1e-3f) return;
    hr[0] /= lr, hr[2] /= lr;
    if (lf < 1e-3f) hf[0] = -hr[2], hf[2] = hr[0];  // (straight down: forward on the ground is right turned 90 degrees)
    else hf[0] /= lf, hf[2] /= lf;
    float tmax = l->far_ > 1 ? l->far_ : 100.0f;
    float corner[4][3];
    for (int c = 0; c < 4; c++) {
        float sx = c & 1 ? 1.0f : -1.0f, sy = c & 2 ? 1.0f : -1.0f, d[3];
        for (int k = 0; k < 3; k++) d[k] = g->fwd[k] + sx * tx0 * g->right[k] + sy * ty0 * g->up[k];
        float t = d[1] < -1e-3f ? (pivot[1] - g->eye[1]) / d[1] : tmax;
        if (!(t > 0) || t > tmax) t = tmax;
        float p[3] = {g->eye[0] + t * d[0], pivot[1], g->eye[2] + t * d[2]};
        float rx = (p[0] - pivot[0]) * hr[0] + (p[2] - pivot[2]) * hr[2], rz = (p[0] - pivot[0]) * hf[0] + (p[2] - pivot[2]) * hf[2];
        rx *= C.kx, rz *= C.kz;
        corner[c][0] = anchor[0] + rx * hr[0] + rz * hf[0], corner[c][1] = anchor[1], corner[c][2] = anchor[2] + rx * hr[2] + rz * hf[2];
    }
    core_set(corner, true);  // (lane ring: the table fit's footprint, before the EXTENT box replaces it)
    for (int c = 0; c < 4 && C.ex > 0 && C.ez > 0; c++)  // (the world box)
        corner[c][0] = anchor[0] + (c & 1 ? 1.0f : -1.0f) * C.ex, corner[c][2] = anchor[2] + (c & 2 ? 1.0f : -1.0f) * C.ez;
    // the prism: 8 points (bottom 0-3, top 4-7 in the order 0 1 3 2 around), 6 faces
    static const int ring[4] = {0, 1, 3, 2};
    float P[8][3];
    for (int i = 0; i < 4; i++) {
        memcpy(P[i], corner[ring[i]], 12), memcpy(P[4 + i], corner[ring[i]], 12);
        P[i][1] -= C.hlo, P[4 + i][1] += C.hhi;
    }
    static const int face[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    float tx = tx0, ty = ty0;
    int npts = 0;
    bool behind = false;
    const CamRef *eyes[2] = {eye0, eye1};
    for (int e = 0; e < (C.eyes ? 2 : 1); e++) {
        float pl[4][4];
        if (C.eyes) eye_planes(eyes[e], C.margin, pl);
        for (int f = 0; f < 6; f++) {
            Poly q = {.n = 4};
            for (int i = 0; i < 4; i++) memcpy(q.p[i], P[face[f][i]], 12);
            for (int i = 0; C.eyes && i < 4 && q.n; i++) clip(&q, pl[i]);
            for (int i = 0; i < q.n; i++) {
                const float *v = q.p[i];
                const float *V = g->V;
                float x = V[0] * v[0] + V[1] * v[1] + V[2] * v[2] + V[3];
                float y = V[4] * v[0] + V[5] * v[1] + V[6] * v[2] + V[7];
                float z = -(V[8] * v[0] + V[9] * v[1] + V[10] * v[2] + V[11]);
                npts++;
                if (z < 0.5f) { behind = true; continue; }
                if (fabsf(x) / z > tx) tx = fabsf(x) / z;
                if (fabsf(y) / z > ty) ty = fabsf(y) / z;
            }
        }
    }
    const float TMAX = C.tmax;  // (4: 76 degrees)
    if (behind) tx = ty = TMAX;
    if (!npts) tx = tx0 * C.kx, ty = ty0 * C.kz;  // (the eyes see none of the table: keep about the table)
    tx = fminf(tx * 1.05f, TMAX), ty = fminf(ty * 1.05f, TMAX);
    if (tx < tx0) tx = tx0;
    if (ty < ty0) ty = ty0;
    float fs = atanf(ty) / atanf(ty0);
    atomic_store(&g_aspect, tx / ty), atomic_store(&g_fs, fs);
    static uint64_t logged;
    if (getenv("LARECOMP_LOG_CULL") && l->frame >= logged + 120) {
        logged = l->frame;
        float lo[3], hi[3];
        bool core = vr_cull_core_box(lo, hi);
        LOG("[vrcull] f%llu fitted: tan x %.3f y %.3f (stock %.3f %.3f): aspect %.3f, fovy x%.3f; %d points%s; core x %.1f..%.1f z %.1f..%.1f\n",
            (unsigned long long)l->frame, tx, ty, tx0, ty0, tx / ty, fs, npts, behind ? ", some behind the camera" : "",
            core ? lo[0] : 0, core ? hi[0] : 0, core ? lo[2] : 0, core ? hi[2] : 0);
    }
}
