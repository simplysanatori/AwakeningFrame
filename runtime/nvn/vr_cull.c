// VR culling fitted to the table (vr_cull.h). The game culls on its game thread, once per frame, with a frustum it builds
// from a camera matrix, an aspect (camera field +0x570, rt_cull_aspect) and a vertical fov (+0x564, rt_fovy). In VR the
// game's camera is never shown; what the eyes need is the diorama: the floor around the table's centre, from
// LARECOMP_VR_CULL_HEIGHT below it to above it, cut by both eyes' frusta (widened by LARECOMP_VR_CULL_MARGIN in tangent,
// for head motion between the fit and the frame that uses it).
// - The default: the table's own culling camera (table_fit / vr_cull_camera below), straight above a rectangle about the
//   table's centre. It reaches the same distance on every side, whatever side the table is seen from.
// - With the Surroundings' box (LARECOMP_VR_CULL_EXTENT, Low / Full / Backdrop's captures) or LARECOMP_VR_CULL=camera: the
//   game camera's own frustum, widened. The stock screen's footprint on the floor (scaled by LARECOMP_VR_CULL_RECT), or
//   the box, seen from the game camera gives the half-angle tangents that frustum needs. Never narrower than the game's.
//   These tangents also widen the game camera's own projection in both cases, as they always did.
// The fit is made on the render thread at each latch and read on the game thread one or two frames later; it is
// camera-relative (the pivot follows the camera), so the lag does not move it.
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
    bool table;             // the fit is the table's own culling camera (default); false: the game camera's frustum widened
    float tkx, tkz, tx, tz; // the table's half-extents: the stock screen's widest half-width and far reach times these, or units
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

#define TABLE_KX 1.5f
#define TABLE_KZ 2.0f
static void scene_apply(int m);
static void init(void) {
    const char *e;
    C.init = 1;
    C.mode = VR_CULL_FIT, C.eyes = 1;
    if ((e = getenv("LARECOMP_VR_CULL"))) C.mode = !strcmp(e, "wide") ? VR_CULL_WIDE : !strcmp(e, "stock") ? VR_CULL_STOCK : VR_CULL_FIT;
    // (the table's culling camera, below) LARECOMP_VR_CULL=camera: the fit before it, the game camera's frustum widened.
    // LARECOMP_VR_CULL_TABLE=<kx>,<kz>: the table's half-extents as multiples of the stock screen's widest half-width and
    // far reach on the floor (14.8 and 11.8 units outdoors); LARECOMP_VR_CULL_TABLE_UNITS=<x>,<z>: in game units.
    C.table = !(e && !strcmp(e, "camera"));
    C.tkx = TABLE_KX, C.tkz = TABLE_KZ, C.tx = C.tz = 0;
    if ((e = getenv("LARECOMP_VR_CULL_TABLE"))) sscanf(e, "%f,%f", &C.tkx, &C.tkz);
    if ((e = getenv("LARECOMP_VR_CULL_TABLE_UNITS"))) sscanf(e, "%f,%f", &C.tx, &C.tz);
    if (!(C.tkx > 0.2f)) C.tkx = TABLE_KX;
    if (!(C.tkz > 0.2f)) C.tkz = TABLE_KZ;
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

// The part of a prism both eyes can see: the prism over a floor quadrilateral (corners as vr_cull_fit makes them: bit 0
// right, bit 1 far), from hlo below it to hhi above, each face cut by each eye's frustum widened by the margin. Returns the
// cut faces' points (at most 6 faces of 32 points an eye); without eye clipping, the prism's own.
#define PRISM_PTS (2 * 6 * 32)
static int prism_points(const float corner[4][3], const CamRef *const eyes[2], float out[PRISM_PTS][3]) {
    static const int ring[4] = {0, 1, 3, 2};  // (bottom 0-3, top 4-7 in the order 0 1 3 2 around)
    static const int face[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    float P[8][3];
    for (int i = 0; i < 4; i++) {
        memcpy(P[i], corner[ring[i]], 12), memcpy(P[4 + i], corner[ring[i]], 12);
        P[i][1] -= C.hlo, P[4 + i][1] += C.hhi;
    }
    int n = 0;
    for (int e = 0; e < (C.eyes ? 2 : 1); e++) {
        float pl[4][4];
        if (C.eyes) eye_planes(eyes[e], C.margin, pl);
        for (int f = 0; f < 6; f++) {
            Poly q = {.n = 4};
            for (int i = 0; i < 4; i++) memcpy(q.p[i], P[face[f][i]], 12);
            for (int i = 0; C.eyes && i < 4 && q.n; i++) clip(&q, pl[i]);
            for (int i = 0; i < q.n && n < PRISM_PTS; i++) memcpy(out[n++], q.p[i], 12);
        }
    }
    return n;
}

// ---- The table's culling camera. The game's visibility pass culls with a frustum built from a camera matrix and four
// values (tools/patches.py CULL_FRUSTUM_SITE). A frustum from the game's own camera, however wide, is symmetric about
// that camera's line of sight: south of Link (towards the camera, which stands 17.7 units south of its floor point and as
// high) the floor left it 7 to 10 units from the table's centre while north it reached 23, and reaching as far south
// would have opened it to 37 units north. From the game's own side nothing was missing; seen from the table's other
// sides, or over a flat tilt, the world ended in a straight cut close behind Link (Mabe Village's weathercock plaza, the
// woods' edge at Field_09C). So the default fit hands the pass another camera: 200 units above the table, looking straight
// down, its frustum just wide enough for a rectangle about the table's centre (the anchor) along the game camera's ground
// axes. The rectangle is the table's footprint (C.tkx, C.tkz times the stock screen's widest half-width and far reach)
// cut down to the part both eyes can see, so it reaches the same distance on every side and costs what its area holds.
// Written by vr_cull_fit (the render thread), read by vr_cull_camera (the game thread) a frame or two later: everything
// is kept relative to the game camera's floor point, so the lag does not move it and a camera cut takes it along.
#define TABLE_CAM_H 200.0f     // the culling camera's height over the floor: its sides lean 5 degrees at most
#define TABLE_CAM_DEPTH 100.0f // and how far below the floor it still takes things in
static _Atomic unsigned g_tc_seq;
static _Atomic int g_tc_on;
static _Atomic float g_tc[7];  // the floor point's distance along the camera's forward; the floor's height above it; the
                               // rectangle's centre from it along the ground's right / forward; its half-extents; hhi
static void table_off(void) {
    if (atomic_load(&g_tc_on)) atomic_fetch_add(&g_tc_seq, 1), atomic_store(&g_tc_on, 0), atomic_fetch_add(&g_tc_seq, 1);
}
static float g_table_log[6];  // (LARECOMP_LOG_CULL) the footprint's half-extents; the fitted rectangle about the anchor
static _Atomic unsigned g_tc_taken[3];  // (LARECOMP_LOG_CULL) frustums built from the table's camera: the level's, the main
                                        // camera's; main camera frustums left alone (another camera's)
// the visibility pass's camera as vr_cull_camera last saw it (position, back axis): what the main camera's frustum is told by
static _Atomic unsigned g_gc_seq;
static _Atomic float g_gc[6];
// u, v: the ground's right and forward (unit, y = 0); w0, d0, n0: the stock screen's widest half-width, far and near reach
static void table_fit(const CamLatch *l, const float pivot[3], const float anchor[3], const float u[3], const float v[3],
                      float w0, float d0, float n0, const CamRef *const eyes[2]) {
    float fx = C.tx > 0 ? C.tx : w0 * C.tkx, fz = C.tz > 0 ? C.tz : d0 * C.tkz;
    float corner[4][3];
    for (int c = 0; c < 4; c++) {
        float sx = c & 1 ? fx : -fx, sz = c & 2 ? fz : -fz;
        corner[c][0] = anchor[0] + sx * u[0] + sz * v[0], corner[c][1] = anchor[1], corner[c][2] = anchor[2] + sx * u[2] + sz * v[2];
    }
    static const int rg[4] = {0, 1, 3, 2};
    {   // the footprint is the floor quadrilateral (vr_cull_rect), the stock screen's own edges inside it
        float q[4][2], area = 0;
        for (int i = 0; i < 4; i++) q[i][0] = corner[rg[i]][0], q[i][1] = corner[rg[i]][2];
        for (int i = 0; i < 4; i++) area += q[i][0] * q[(i + 1) % 4][1] - q[(i + 1) % 4][0] * q[i][1];
        for (int i = 0; i < 4; i++) memcpy(g_rect.quad[i], q[area >= 0 ? i : 3 - i], 8);
        float sz = fmaxf(1 - d0 / fz, 0.0f), sx = fmaxf(1 - w0 / fx, 0.0f), so[4] = {sz, sx, sz, sx};
        for (int i = 0; i < 4; i++) g_rect.shrink[i] = area >= 0 ? so[i] : so[(2 - i + 4) % 4];
        g_rect.floor_y = anchor[1], g_rect.hlo = C.hlo, g_rect.hhi = C.hhi, g_rect.have = true;
        // (vr_cull_drawn) and what the culling draws, as far as an eye can tell: the cut below only leaves out what
        // neither eye sees, so the edge fade gets a floor that does not move with the head
        memcpy(g_drawn.quad, g_rect.quad, sizeof g_drawn.quad), g_drawn.have = true;
    }
    static float pts[PRISM_PTS][3];  // (the render thread only)
    int n = prism_points(corner, eyes, pts);
    float lo[2] = {-fx, -fz}, hi[2] = {fx, fz};
    if (n) {  // (the eyes see none of the table: the whole footprint)
        lo[0] = lo[1] = 1e30f, hi[0] = hi[1] = -1e30f;
        for (int i = 0; i < n; i++) {
            float dx = pts[i][0] - anchor[0], dz = pts[i][2] - anchor[2], a = dx * u[0] + dz * u[2], b = dx * v[0] + dz * v[2];
            lo[0] = fminf(lo[0], a), hi[0] = fmaxf(hi[0], a), lo[1] = fminf(lo[1], b), hi[1] = fmaxf(hi[1], b);
        }
        // 5% and half a unit more, as the game camera's fit had 5%; never past the footprint
        float gu = 0.05f * (hi[0] - lo[0]) + 0.5f, gv = 0.05f * (hi[1] - lo[1]) + 0.5f;
        lo[0] = fmaxf(lo[0] - gu, -fx), hi[0] = fminf(hi[0] + gu, fx), lo[1] = fmaxf(lo[1] - gv, -fz), hi[1] = fminf(hi[1] + gv, fz);
    }
    float ax = anchor[0] - pivot[0], az = anchor[2] - pivot[2];  // (the anchor follows the floor point with a lag)
    {   // Never less than the stock screen about the game camera's floor point, as the game camera's fit never was: this
        // frustum also culls characters, effects and whatever the game tests against its view, and none of that may come
        // and go with the head (a close look at a table corner left Link's own rectangle out)
        float pu = -(ax * u[0] + az * u[2]), pv = -(ax * v[0] + az * v[2]);
        lo[0] = fminf(lo[0], pu - w0), hi[0] = fmaxf(hi[0], pu + w0), lo[1] = fminf(lo[1], pv - n0), hi[1] = fmaxf(hi[1], pv + d0);
    }
    float cu = 0.5f * (lo[0] + hi[0]), cv = 0.5f * (lo[1] + hi[1]), hu = 0.5f * (hi[0] - lo[0]), hv = 0.5f * (hi[1] - lo[1]);
    const CamRef *g = &l->ref;
    float vals[7] = {(pivot[0] - g->eye[0]) * g->fwd[0] + (pivot[1] - g->eye[1]) * g->fwd[1] + (pivot[2] - g->eye[2]) * g->fwd[2],
                     anchor[1] - pivot[1], ax * u[0] + az * u[2] + cu, ax * v[0] + az * v[2] + cv, hu, hv, C.hhi};
    atomic_fetch_add(&g_tc_seq, 1);
    for (int i = 0; i < 7; i++) atomic_store(&g_tc[i], vals[i]);
    atomic_store(&g_tc_on, 1);
    atomic_fetch_add(&g_tc_seq, 1);
    g_table_log[0] = fx, g_table_log[1] = fz, g_table_log[2] = lo[0], g_table_log[3] = hi[0], g_table_log[4] = lo[1], g_table_log[5] = hi[1];
}

// (widescreen.c rt_cull_frustum) m: a camera matrix a culling frustum is about to be built from (the right, up and back
// axes and the position, four floats each); p: its fovy (radians), aspect, near and far. main: the engine's main camera
// (which culls everything but the level's own models), else the visibility pass's (the level's models). Replaced by the
// table's camera when the fit made one; false: left as the game has them. The engine builds the main camera's frustum for
// every camera of its kind, so there only a matrix that is the visibility pass's camera (within a frame's movement) is
// taken: a reflection's or a menu's keeps its own.
bool vr_cull_camera(float *m, float *p, bool main) {
    if (!main) {
        atomic_fetch_add(&g_gc_seq, 1);
        for (int i = 0; i < 3; i++) atomic_store(&g_gc[i], m[12 + i]), atomic_store(&g_gc[3 + i], m[8 + i]);
        atomic_fetch_add(&g_gc_seq, 1);
    } else {
        float c[6];
        unsigned s0 = atomic_load(&g_gc_seq);
        for (int i = 0; i < 6; i++) c[i] = atomic_load(&g_gc[i]);
        float lb = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]), lc = sqrtf(c[3] * c[3] + c[4] * c[4] + c[5] * c[5]);
        float dx = m[12] - c[0], dy = m[13] - c[1], dz = m[14] - c[2];
        bool same = !(s0 & 1) && atomic_load(&g_gc_seq) == s0 && lb > 1e-6f && lc > 1e-6f && dx * dx + dy * dy + dz * dz < 4.0f &&
                    (m[8] * c[3] + m[9] * c[4] + m[10] * c[5]) / (lb * lc) > 0.99f;
        if (!same) { atomic_fetch_add(&g_tc_taken[2], 1); return false; }
    }
    float t[7];
    for (int tries = 0;; tries++) {
        if (tries == 8) return false;
        unsigned s0 = atomic_load(&g_tc_seq);
        if (s0 & 1) continue;
        int on = atomic_load(&g_tc_on);
        for (int i = 0; i < 7; i++) t[i] = atomic_load(&g_tc[i]);
        if (atomic_load(&g_tc_seq) != s0) continue;
        if (!on) return false;
        break;
    }
    float lb = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]), lr = sqrtf(m[0] * m[0] + m[2] * m[2]);
    if (!(lb > 1e-6f) || !(lr > 1e-3f)) return false;
    float fwd[3] = {-m[8] / lb, -m[9] / lb, -m[10] / lb};
    if (fabsf(fwd[1]) < 0.26f) return false;  // (a side view since the fit: the game's own, as vr_cull_fit leaves it)
    float u[3] = {m[0] / lr, 0, m[2] / lr}, v[3] = {u[2], 0, -u[0]};  // (the ground's right and forward, as table_fit has them)
    float pv[3] = {m[12] + t[0] * fwd[0], m[13] + t[0] * fwd[1], m[14] + t[0] * fwd[2]};
    float h = TABLE_CAM_H, tu = t[4] / (h - t[6]), tv = t[5] / (h - t[6]);
    m[0] = u[0], m[1] = 0, m[2] = u[2];
    m[4] = v[0], m[5] = 0, m[6] = v[2];
    m[8] = 0, m[9] = 1, m[10] = 0;  // (back: it looks straight down)
    m[12] = pv[0] + t[2] * u[0] + t[3] * v[0], m[13] = pv[1] + t[1] + h, m[14] = pv[2] + t[2] * u[2] + t[3] * v[2];
    p[0] = 2.0f * atanf(tv), p[1] = tu / tv, p[2] = 1.0f, p[3] = h + TABLE_CAM_DEPTH;
    atomic_fetch_add(&g_tc_taken[main], 1);
    return true;
}

// (LARECOMP_LOG_CULL) what a check needs to say where the table's rectangle lies in an eye image: the game camera, the
// pivot and anchor, the floor rectangle, the floor the culling takes in, and each eye's position and view-projection
static void cull_log_geom(const CamLatch *l, const float pivot[3], const float anchor[3], const CamRef *eye0, const CamRef *eye1) {
    const CamRef *g = &l->ref, *eyes[2] = {eye0, eye1};
    LOG("[vrcull] f%llu geom: cam %.3f %.3f %.3f fwd %.4f %.4f %.4f pivot %.3f %.3f %.3f anchor %.3f %.3f %.3f far %.1f\n",
        (unsigned long long)l->frame, g->eye[0], g->eye[1], g->eye[2], g->fwd[0], g->fwd[1], g->fwd[2], pivot[0], pivot[1], pivot[2],
        anchor[0], anchor[1], anchor[2], l->far_);
    if (g_rect.have)
        LOG("[vrcull] f%llu rect: %.3f %.3f  %.3f %.3f  %.3f %.3f  %.3f %.3f height -%.2f +%.2f\n", (unsigned long long)l->frame,
            g_rect.quad[0][0], g_rect.quad[0][1], g_rect.quad[1][0], g_rect.quad[1][1], g_rect.quad[2][0], g_rect.quad[2][1],
            g_rect.quad[3][0], g_rect.quad[3][1], g_rect.hlo, g_rect.hhi);
    if (g_drawn.have)
        LOG("[vrcull] f%llu drawn: %.3f %.3f  %.3f %.3f  %.3f %.3f  %.3f %.3f\n", (unsigned long long)l->frame,
            g_drawn.quad[0][0], g_drawn.quad[0][1], g_drawn.quad[1][0], g_drawn.quad[1][1], g_drawn.quad[2][0], g_drawn.quad[2][1],
            g_drawn.quad[3][0], g_drawn.quad[3][1]);
    for (int e = 0; e < 2; e++) {
        const float *m = eyes[e]->VP;
        LOG("[vrcull] f%llu eye%d: pos %.3f %.3f %.3f fwd %.4f %.4f %.4f vp %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g\n",
            (unsigned long long)l->frame, e, eyes[e]->eye[0], eyes[e]->eye[1], eyes[e]->eye[2], eyes[e]->fwd[0], eyes[e]->fwd[1],
            eyes[e]->fwd[2], m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
}

static unsigned g_swing[2];  // (vr_cull_fit, LARECOMP_LOG_CULL) swings since the last log line: fitted, used
void vr_cull_fit(const CamLatch *l, const float pivot[3], const float anchor[3], const CamRef *eye0, const CamRef *eye1) {
    if (!C.init) init();
    if (C.mode != VR_CULL_FIT || !l || !l->valid) { g_rect.have = false, table_off(); return; }
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
        table_off();
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
    float corner[4][3], w0 = 0, d0 = 0, n0 = 0;  // (the stock screen's widest half-width, far and near reach on the floor)
    for (int c = 0; c < 4; c++) {
        float sx = c & 1 ? 1.0f : -1.0f, sy = c & 2 ? 1.0f : -1.0f, d[3];
        for (int k = 0; k < 3; k++) d[k] = g->fwd[k] + sx * tx0 * g->right[k] + sy * ty0 * g->up[k];
        float t = d[1] < -1e-3f ? (pivot[1] - g->eye[1]) / d[1] : tmax;
        if (!(t > 0) || t > tmax) t = tmax;
        float p[3] = {g->eye[0] + t * d[0], pivot[1], g->eye[2] + t * d[2]};
        float rx = (p[0] - pivot[0]) * hr[0] + (p[2] - pivot[2]) * hr[2], rz = (p[0] - pivot[0]) * hf[0] + (p[2] - pivot[2]) * hf[2];
        w0 = fmaxf(w0, fabsf(rx)), d0 = fmaxf(d0, rz), n0 = fmaxf(n0, -rz);
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
    float tx = tx0, ty = ty0;
    bool behind = false;
    const CamRef *eyes[2] = {eye0, eye1};
    static float pts[PRISM_PTS][3];  // (the render thread only)
    int npts = prism_points(corner, eyes, pts);
    for (int i = 0; i < npts; i++) {
        const float *v = pts[i], *V = g->V;
        float x = V[0] * v[0] + V[1] * v[1] + V[2] * v[2] + V[3];
        float y = V[4] * v[0] + V[5] * v[1] + V[6] * v[2] + V[7];
        float z = -(V[8] * v[0] + V[9] * v[1] + V[10] * v[2] + V[11]);
        if (z < 0.5f) { behind = true; continue; }
        if (fabsf(x) / z > tx) tx = fabsf(x) / z;
        if (fabsf(y) / z > ty) ty = fabsf(y) / z;
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
    // The default: the visibility pass culls with the table's own camera (table_fit, above); the tangents fitted here then
    // only widen the game camera's own projection, as before. With the Surroundings' box (or LARECOMP_VR_CULL=camera) the
    // pass keeps the game camera's frustum with these tangents.
    bool table = C.table && !(ex > 0 && ez > 0);
    if (table) table_fit(l, pivot, anchor, hr, (const float[3]){hr[2], 0, -hr[0]}, w0, d0, n0, eyes);
    else table_off();
    static uint64_t logged;
    static int log_iv = -1;  // LARECOMP_LOG_CULL=<n>: every n frames (1: every 120), with the geometry line
    if (log_iv < 0) { const char *e = getenv("LARECOMP_LOG_CULL"); log_iv = e ? (atoi(e) > 1 ? atoi(e) : 120) : 0; }
    if (log_iv && l->frame >= logged + (uint64_t)log_iv) {
        logged = l->frame;
        cull_log_geom(l, pivot, anchor, eye0, eye1);
        float lo[3], hi[3];
        bool core = vr_cull_core_box(lo, hi);
        LOG("[vrcull] f%llu fitted: tan x %.3f y %.3f (stock %.3f %.3f): aspect %.3f, fovy x%.3f; %d points%s; core x %.1f..%.1f z %.1f..%.1f; swings %u fitted, %u used\n",
            (unsigned long long)l->frame, tx, ty, tx0, ty0, tx / ty, fs, npts, behind ? ", some behind the camera" : "",
            core ? lo[0] : 0, core ? hi[0] : 0, core ? lo[2] : 0, core ? hi[2] : 0, g_swing[0], g_swing[1]);
        g_swing[0] = g_swing[1] = 0;
        if (table)
            LOG("[vrcull] f%llu table: footprint +-%.2f x +-%.2f about the anchor; culled to right %.2f..%.2f forward %.2f..%.2f; "
                "frustums built from it: %u level, %u main camera (%u of another camera left alone)\n", (unsigned long long)l->frame,
                g_table_log[0], g_table_log[1], g_table_log[2], g_table_log[3], g_table_log[4], g_table_log[5],
                atomic_exchange(&g_tc_taken[0], 0), atomic_exchange(&g_tc_taken[1], 0), atomic_exchange(&g_tc_taken[2], 0));
    }
}
