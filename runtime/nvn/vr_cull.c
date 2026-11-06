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
} C;
static struct { bool have; float quad[4][2], floor_y, hlo, hhi; } g_rect;  // the latest fit's floor quadrilateral (vr_cull_rect)
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

bool vr_cull_rect(float quad[4][2], float *floor_y, float *hlo, float *hhi) {
    if (!g_rect.have) return false;  // (render thread, like vr_cull_fit)
    memcpy(quad, g_rect.quad, sizeof g_rect.quad), *floor_y = g_rect.floor_y, *hlo = g_rect.hlo, *hhi = g_rect.hhi;
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
    if (C.mode != VR_CULL_FIT || !l || !l->valid) { g_rect.have = false; return; }
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
        g_rect.have = false;
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
    {   // the floor quadrilateral, counter-clockwise seen from above in (x, z) (the edge pass fades to it)
        static const int rg[4] = {0, 1, 3, 2};
        float q[4][2], area = 0;
        for (int i = 0; i < 4; i++) q[i][0] = corner[rg[i]][0], q[i][1] = corner[rg[i]][2];
        for (int i = 0; i < 4; i++) area += q[i][0] * q[(i + 1) % 4][1] - q[(i + 1) % 4][0] * q[i][1];
        for (int i = 0; i < 4; i++) memcpy(g_rect.quad[i], q[area >= 0 ? i : 3 - i], 8);
        g_rect.floor_y = anchor[1], g_rect.hlo = C.hlo, g_rect.hhi = C.hhi, g_rect.have = true;
    }
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
    const float TMAX = 4.0f;  // (76 degrees)
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
        LOG("[vrcull] f%llu fitted: tan x %.3f y %.3f (stock %.3f %.3f): aspect %.3f, fovy x%.3f; %d points%s\n", (unsigned long long)l->frame,
            tx, ty, tx0, ty0, tx / ty, fs, npts, behind ? ", some behind the camera" : "");
    }
}
