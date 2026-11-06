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
    float env_ex, env_ez, env_tmax, env_ring, rooms;  // (scene) the environment's values; the reach of Low / Full in rooms
    bool given_ex, given_tmax, given_ring;
} C;
static struct { bool have; float quad[4][2], floor_y, hlo, hhi, shrink[4]; } g_rect;  // the latest fit's floor quadrilateral (vr_cull_rect)
static struct { bool have; float quad[4][2]; } g_drawn;  // where the fitted frustum meets the floor (vr_cull_drawn)
static _Atomic float g_aspect, g_fs;   // the fitted culling aspect (absolute) and fovy scale; 0 = none yet
static _Atomic float g_fovy_orig = 0;  // the game's own fovy field (radians or degrees, as it is)
static _Atomic float g_aspect_orig = 0;

static void scene_apply(int m);
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
    // (streaming) LARECOMP_VR_CULL_EXTENT=<x>,<z>: the culled-to rectangle is the world-axis box of these half-extents about
    // the table centre instead of the scaled stock footprint, e.g. 52,42 for the overworld's resident 7x7 rooms around Link
    // (rooms are 15 x 12; docs/af/STREAM.md). LARECOMP_VR_CULL_TMAX=<tan>: the widest half-angle tangent (default 4, 76 deg).
    C.ex = C.ez = 0, C.tmax = 4.0f;
    if ((e = getenv("LARECOMP_VR_CULL_EXTENT"))) sscanf(e, "%f,%f", &C.ex, &C.ez);
    if ((e = getenv("LARECOMP_VR_CULL_TMAX"))) C.tmax = (float)atof(e);
    if (!(C.tmax > 0.5f)) C.tmax = 4.0f;
    // (ring draws) LARECOMP_VR_RING=<margin>: draws whose position is outside the table fit's footprint grown by this are
    // "ring" draws (exec.c: colour passes only, no shadow casters, no depth pre-pass). LARECOMP_VR_RING_CAST=<margin>:
    // ring draws within this of the footprint still cast shadows.
    C.ring = C.ring_cast = 0;
    if ((e = getenv("LARECOMP_VR_RING"))) C.ring = (float)atof(e);
    if ((e = getenv("LARECOMP_VR_RING_CAST"))) C.ring_cast = (float)atof(e);
    if (!(C.ring > 0)) C.ring = 0;
    if (!(C.ring_cast > 0)) C.ring_cast = 0;
    C.env_ex = C.ex, C.env_ez = C.ez, C.env_tmax = C.tmax, C.env_ring = C.ring;
    C.given_ex = getenv("LARECOMP_VR_CULL_EXTENT") != NULL, C.given_tmax = getenv("LARECOMP_VR_CULL_TMAX") != NULL;
    C.given_ring = getenv("LARECOMP_VR_RING") != NULL;
    // (scene) LARECOMP_VR_SCENE=off | low | backdrop | full, or a number of rooms (full with that reach); the VR tab's row
    // sets it live through vr_scene_set_mode. LARECOMP_VR_SCENE_ROOMS=<n>: the reach of low / full (default 2).
    C.rooms = 2.0f;
    if ((e = getenv("LARECOMP_VR_SCENE_ROOMS")) && atof(e) > 0) C.rooms = fminf((float)atof(e), 3.0f);
    int m = VR_SCENE_OFF;
    if ((e = getenv("LARECOMP_VR_SCENE"))) {
        if (!strcmp(e, "low")) m = VR_SCENE_LOW;
        else if (!strcmp(e, "backdrop")) m = VR_SCENE_BACKDROP;
        else if (!strcmp(e, "full")) m = VR_SCENE_FULL;
        else if (atof(e) > 0) m = VR_SCENE_FULL, C.rooms = fminf((float)atof(e), 3.0f);
    }
    scene_apply(m);
}

// ---- (scene) drawing the overworld past the table (docs/af/SCENE.md). The game keeps the 7 x 7 rooms around Link's room
// resident, so a reach of up to 3 rooms needs nothing loaded: Low and Full widen the culling to the EXTENT box of (rooms +
// 0.5) rooms about the table centre (rooms are 15 x 12 units), clipped as always by both eyes' frusta and their margin, with
// the widest tangent raised to 20 (the box reaches under and behind the game camera, whose frustum can't pass 90 degrees:
// tan 4 left the south rows out) and ring draws (margin 4) so the extra rooms stay out of the shadow and depth pre-passes.
// The values the environment gives (LARECOMP_VR_CULL_EXTENT / _TMAX / LARECOMP_VR_RING) win in every mode.
// Read live: the fit (render thread) and the ring routing (executor) take them per frame.
static _Atomic int g_scene = VR_SCENE_OFF;
static _Atomic float g_ex, g_ez, g_tmax, g_ring;
static void scene_params(bool wide) {  // the box, tangent cap and ring margin: wide (Low, Full, Backdrop capturing) or not
    float r = C.rooms;
    atomic_store(&g_ex, C.given_ex || !wide ? C.env_ex : 15.0f * (r + 0.5f));
    atomic_store(&g_ez, C.given_ex || !wide ? C.env_ez : 12.0f * (r + 0.5f));
    atomic_store(&g_tmax, C.given_tmax || !wide ? C.env_tmax : 20.0f);
    atomic_store(&g_ring, C.given_ring || !wide ? C.env_ring : 4.0f);
}
static void scene_apply(int m) {
    scene_params(m == VR_SCENE_LOW || m == VR_SCENE_FULL);  // (Backdrop: narrow until scene.c asks for a capture)
    atomic_store(&g_scene, m);
}
void vr_scene_set_mode(int m) {
    if (!C.init) init();
    if (m < 0 || m >= VR_SCENE_N || m == atomic_load(&g_scene)) return;
    static const char *const nm[VR_SCENE_N] = {"off", "low", "backdrop", "full"};
    LOG("[scene] mode %s (VR settings; was %s)\n", nm[m], nm[atomic_load(&g_scene)]);
    scene_apply(m);
}
// (scene.c, Backdrop) the frames it captures the surroundings from are culled and drawn as Full
void vr_scene_backdrop_wide(bool on) {
    if (!C.init) init();
    if (atomic_load(&g_scene) == VR_SCENE_BACKDROP) scene_params(on);
}
int vr_scene_mode(void) {
    if (!C.init) init();
    return atomic_load(&g_scene);
}
// the reach's smaller half-extent in game units, 0 when off (the edge fade's clear radius follows it): the box Low, Full and
// Backdrop draw (Backdrop between its captures too), else LARECOMP_VR_CULL_EXTENT's
float vr_cull_scene_reach(void) {
    if (!C.init) init();
    float r = C.rooms;
    if (atomic_load(&g_scene) != VR_SCENE_OFF && !C.given_ex) return fminf(15.0f * (r + 0.5f), 12.0f * (r + 0.5f));
    float ex = atomic_load(&g_ex), ez = atomic_load(&g_ez);
    return ex > 0 && ez > 0 ? fminf(ex, ez) : 0.0f;
}

// the table fit's footprint as a world-axis box (x, z), before the EXTENT override: written by vr_cull_fit (the render
// thread at each latch), read by the executor. A sequence count keeps the four values together.
static _Atomic unsigned g_core_seq;
static _Atomic float g_core[4];  // lo x, lo z, hi x, hi z
static _Atomic int g_core_ok;
static _Atomic float g_ext[4];  // (lane scene) the EXTENT box about the anchor (lo x, lo z, hi x, hi z); unset when EXTENT is off
static void core_set(const float corner[4][3], bool ok, const float anchor[3]) {
    atomic_fetch_add(&g_core_seq, 1);
    float ex = atomic_load(&g_ex), ez = atomic_load(&g_ez);
    if (ok && anchor && ex > 0 && ez > 0)
        atomic_store(&g_ext[0], anchor[0] - ex), atomic_store(&g_ext[1], anchor[2] - ez),
        atomic_store(&g_ext[2], anchor[0] + ex), atomic_store(&g_ext[3], anchor[2] + ez);
    if (ok) {
        float lx = corner[0][0], hx = lx, lz = corner[0][2], hz = lz;
        for (int c = 1; c < 4; c++)
            lx = fminf(lx, corner[c][0]), hx = fmaxf(hx, corner[c][0]), lz = fminf(lz, corner[c][2]), hz = fmaxf(hz, corner[c][2]);
        atomic_store(&g_core[0], lx), atomic_store(&g_core[1], lz), atomic_store(&g_core[2], hx), atomic_store(&g_core[3], hz);
    }
    atomic_store(&g_core_ok, ok);
    atomic_fetch_add(&g_core_seq, 1);
}

static bool core_box(float lo[3], float hi[3], float elo[2], float ehi[2]) {
    for (int tries = 0; tries < 8; tries++) {
        unsigned s0 = atomic_load(&g_core_seq);
        if (s0 & 1) continue;
        int ok = atomic_load(&g_core_ok);
        float b[4], x[4];
        for (int i = 0; i < 4; i++) b[i] = atomic_load(&g_core[i]), x[i] = atomic_load(&g_ext[i]);
        if (atomic_load(&g_core_seq) != s0) continue;
        if (!ok) return false;
        lo[0] = b[0], lo[1] = -1e30f, lo[2] = b[1], hi[0] = b[2], hi[1] = 1e30f, hi[2] = b[3];
        if (elo) elo[0] = x[0], elo[1] = x[1], ehi[0] = x[2], ehi[1] = x[3];
        return true;
    }
    return false;
}
bool vr_cull_core_box(float lo[3], float hi[3]) { return core_box(lo, hi, NULL, NULL); }

bool vr_ring_on(void) {
    if (!C.init) init();
    return atomic_load(&g_ring) > 0;
}

int vr_ring_class(const float blo[3], const float bhi[3], uint64_t frame) {
    if (!C.init) init();
    // one footprint per frame (the executor's thread only): a refit between a frame's depth pre-pass and its colour
    // pass must not turn a draw the pre-pass left out into one that tests EQUAL against it
    static float lo[3], hi[3], elo[2], ehi[2];
    static bool ok;
    static uint64_t at = ~0ull, at_m = ~0ull;
    if (frame != at) at = frame, ok = core_box(lo, hi, elo, ehi);
    static float m, ex, ez;  // (the frame's margin and box: the same for all of its draws)
    if (frame != at_m) at_m = frame, m = atomic_load(&g_ring), ex = atomic_load(&g_ex), ez = atomic_load(&g_ez);
    if (!(m > 0) || !ok) return 0;
    float c = C.ring_cast;  // (the draw's box against the footprint grown by the margin, in x and z)
    if (bhi[0] >= lo[0] - m && blo[0] <= hi[0] + m && bhi[2] >= lo[2] - m && blo[2] <= hi[2] + m) return 0;
    if (c > 0 && bhi[0] >= lo[0] - c && blo[0] <= hi[0] + c && bhi[2] >= lo[2] - c && blo[2] <= hi[2] + c) return 2;
    // (lane scene) the game culls with a frustum from its own camera, so a box that reaches behind that camera opens the
    // frustum to tan 20 and every resident room in front of it comes through: past the EXTENT box, draw nothing
    if (ex > 0 && ez > 0 && !(bhi[0] >= elo[0] && blo[0] <= ehi[0] && bhi[2] >= elo[1] && blo[2] <= ehi[1])) return 3;
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

bool vr_cull_drawn(float quad[4][2]) {
    if (!g_rect.have || !g_drawn.have) return false;  // (render thread, like vr_cull_fit)
    memcpy(quad, g_drawn.quad, sizeof g_drawn.quad);
    return true;
}

bool vr_cull_rect(float quad[4][2], float *floor_y, float *hlo, float *hhi) {
    if (!g_rect.have) return false;  // (render thread, like vr_cull_fit)
    memcpy(quad, g_rect.quad, sizeof g_rect.quad), *floor_y = g_rect.floor_y, *hlo = g_rect.hlo, *hhi = g_rect.hhi;
    return true;
}

// Per edge of that quadrilateral (the same order): how far the stock screen's own edge lies inside it, as a share of
// the distance from the quadrilateral's middle to that edge (1 - 1 / k of LARECOMP_VR_CULL_RECT; 0 at k = 1)
bool vr_cull_rect_shrink(float shrink[4]) {
    if (!g_rect.have) return false;
    memcpy(shrink, g_rect.shrink, sizeof g_rect.shrink);
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

static unsigned g_swing[2];  // (vr_cull_fit, LARECOMP_LOG_CULL) swings since the last log line: fitted, used
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
        core_set(NULL, false, NULL);  // (no footprint: every draw is core)
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
    core_set(corner, true, anchor);  // (ring draws: the table fit's footprint, before the EXTENT box replaces it)
    float ex = atomic_load(&g_ex), ez = atomic_load(&g_ez);
    for (int c = 0; c < 4 && ex > 0 && ez > 0; c++)  // (the world box)
        corner[c][0] = anchor[0] + (c & 1 ? 1.0f : -1.0f) * ex, corner[c][2] = anchor[2] + (c & 2 ? 1.0f : -1.0f) * ez;
    {   // the floor quadrilateral, counter-clockwise seen from above in (x, z) (the edge pass fades to it)
        static const int rg[4] = {0, 1, 3, 2};
        float q[4][2], area = 0;
        for (int i = 0; i < 4; i++) q[i][0] = corner[rg[i]][0], q[i][1] = corner[rg[i]][2];
        for (int i = 0; i < 4; i++) area += q[i][0] * q[(i + 1) % 4][1] - q[(i + 1) % 4][0] * q[i][1];
        for (int i = 0; i < 4; i++) memcpy(g_rect.quad[i], q[area >= 0 ? i : 3 - i], 8);
        float so[4] = {1 - 1 / fmaxf(C.kz, 1.0f), 1 - 1 / fmaxf(C.kx, 1.0f), 1 - 1 / fmaxf(C.kz, 1.0f), 1 - 1 / fmaxf(C.kx, 1.0f)};
        for (int i = 0; i < 4; i++) g_rect.shrink[i] = area >= 0 ? so[i] : so[(2 - i + 4) % 4];
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
    const float TMAX = atomic_load(&g_tmax);  // (4: 76 degrees; 20 with the scene's box)
    if (behind) tx = ty = TMAX;
    if (!npts) tx = tx0 * C.kx, ty = ty0 * C.kz;  // (the eyes see none of the table: keep about the table)
    tx = fminf(tx * 1.05f, TMAX), ty = fminf(ty * 1.05f, TMAX);
    if (tx < tx0) tx = tx0;
    if (ty < ty0) ty = ty0;
    // (lane scene) hold: with the Surroundings box the fit swings from frame to frame as the head moves (a corner that
    // dips behind the game camera takes it from about 2 to 20 and back), and the game's list of drawn objects with it:
    // flicker. So the widening grows at once and shrinks only after LARECOMP_VR_CULL_HOLD frames (60) without growing,
    // by 3% a frame. Only while the box is on (Low, Full, Backdrop's capture frames); the table fit alone keeps the plain
    // fit, and the held values start again from it (LARECOMP_VR_CULL_HOLD_ALL=1: always).
    {
        static int hold = -1, all;
        if (hold < 0) {
            const char *e = getenv("LARECOMP_VR_CULL_HOLD");
            hold = e && *e ? atoi(e) : 60, all = getenv("LARECOMP_VR_CULL_HOLD_ALL") != NULL;
        }
        static float htx, hty;
        static uint64_t gx, gy;
        {   // (LARECOMP_LOG_CULL) fits that moved the vertical tangent by more than a third from the previous one: as fitted, as used
            static float rp, up;
            float used = hold > 0 && (all || atomic_load(&g_ex) > 0) ? fmaxf(ty, hty) : ty;
            if (rp > 0 && fabsf(ty - rp) > 0.33f * fminf(ty, rp)) g_swing[0]++;
            if (up > 0 && fabsf(used - up) > 0.33f * fminf(used, up)) g_swing[1]++;
            rp = ty, up = used;
        }
        if (hold > 0 && (all || atomic_load(&g_ex) > 0)) {
            uint64_t f = l->frame;
            if (tx >= htx || f < gx) htx = tx, gx = f;
            else if (f - gx > (uint64_t)hold) htx = fmaxf(tx, htx * 0.97f);
            if (ty >= hty || f < gy) hty = ty, gy = f;
            else if (f - gy > (uint64_t)hold) hty = fmaxf(ty, hty * 0.97f);
            tx = htx, ty = hty;
        } else htx = tx, hty = ty, gx = gy = l->frame;
    }
    float fs = atanf(ty) / atanf(ty0);
    atomic_store(&g_aspect, tx / ty), atomic_store(&g_fs, fs);
    {   // (vr_cull_drawn) the floor the culling frustum takes in: its four corner rays to the floor (or the far plane)
        static const int rg[4] = {0, 1, 3, 2};
        float q[4][2], area = 0;
        for (int i = 0; i < 4; i++) {
            int c = rg[i];
            float sx = c & 1 ? 1.0f : -1.0f, sy = c & 2 ? 1.0f : -1.0f, d[3];
            for (int k = 0; k < 3; k++) d[k] = g->fwd[k] + sx * tx * g->right[k] + sy * ty * g->up[k];
            float t = d[1] < -1e-3f ? (pivot[1] - g->eye[1]) / d[1] : tmax;
            if (!(t > 0) || t > tmax) t = tmax;
            q[i][0] = g->eye[0] + t * d[0], q[i][1] = g->eye[2] + t * d[2];
        }
        for (int i = 0; i < 4; i++) area += q[i][0] * q[(i + 1) % 4][1] - q[(i + 1) % 4][0] * q[i][1];
        for (int i = 0; i < 4; i++) memcpy(g_drawn.quad[i], q[area >= 0 ? i : 3 - i], 8);
        g_drawn.have = !behind;
    }
    static uint64_t logged;
    if (getenv("LARECOMP_LOG_CULL") && l->frame >= logged + 120) {
        logged = l->frame;
        float lo[3], hi[3];
        bool core = vr_cull_core_box(lo, hi);
        LOG("[vrcull] f%llu fitted: tan x %.3f y %.3f (stock %.3f %.3f): aspect %.3f, fovy x%.3f; %d points%s; core x %.1f..%.1f z %.1f..%.1f; swings %u fitted, %u used\n",
            (unsigned long long)l->frame, tx, ty, tx0, ty0, tx / ty, fs, npts, behind ? ", some behind the camera" : "",
            core ? lo[0] : 0, core ? hi[0] : 0, core ? lo[2] : 0, core ? hi[2] : 0, g_swing[0], g_swing[1]);
        g_swing[0] = g_swing[1] = 0;
    }
}
