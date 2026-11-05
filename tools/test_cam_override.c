// Free-camera probe maths and rewrite (runtime/nvn/cam_override.c) against synthetic game cameras: no game files, no
// window. Build + run: tools/test_cam_override.sh
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/cam_override.h"

static int fails;
#define CHECK(c) ((c) ? (void)0 : (void)(fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), fails++))
#define NEAR(a, b, e) CHECK(fabsf((float)(a) - (float)(b)) <= (e))
#define RAD (float)(M_PI / 180.0)

// a Link's Awakening-like game camera: looks along -z from +z, pitched down, 15 units above the floor
static void game_camera(float *u, float pitch_deg, float height, float fovy_deg, float aspect, float eye_x, float eye_z) {
    float p = pitch_deg * RAD, eye[3] = {eye_x, height, eye_z};
    float at[3] = {eye_x, height - sinf(p), eye_z - cosf(p)}, up[3] = {0, 1, 0};
    cam_look_at(u, eye, at, up);
    memset(u + 16, 0, 64);
    float f = 1 / tanf(fovy_deg * RAD / 2), n = 0.5f, fa = 400;
    u[16] = f / aspect, u[21] = f, u[26] = -(fa + n) / (fa - n), u[27] = -2 * fa * n / (fa - n), u[30] = -1;
    cam_mat_mul(u + 32, u + 16, u, false);
    for (int i = 48; i < 80; i++) u[i] = 0.25f * (float)(i - 40);  // (unknown junk)
}

static void ndc(const float *vp, const float p[3], float o[3]) {
    float c[4];
    for (int r = 0; r < 4; r++) c[r] = vp[r * 4] * p[0] + vp[r * 4 + 1] * p[1] + vp[r * 4 + 2] * p[2] + vp[r * 4 + 3];
    o[0] = c[0] / c[3], o[1] = c[1] / c[3], o[2] = c[2] / c[3];
}

static int hits;
static char hitwhat[8][64];
static unsigned hitoff[8];
static void on_hit(void *u, unsigned off, const char *w) { (void)u; if (hits < 8) snprintf(hitwhat[hits], 64, "%s", w), hitoff[hits] = off;
    hits++; }

int main(void) {
    // ---- matrix maths
    float a[16], b[16], c[16], i4[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int i = 0; i < 16; i++) a[i] = (float)((i * 7 + 3) % 11) - 4.5f + (i % 5 == 0 ? 6 : 0);
    CHECK(cam_mat_inv(b, a));
    cam_mat_mul(c, a, b, false);
    for (int i = 0; i < 16; i++) NEAR(c[i], i4[i], 1e-4f);
    float sing[16] = {0};
    CHECK(!cam_mat_inv(c, sing));
    {   // fused and plain products are both well-formed and agree to rounding
        float f[16], g[16];
        cam_mat_mul(f, a, b, true), cam_mat_mul(g, a, b, false);
        for (int i = 0; i < 16; i++) NEAR(f[i], g[i], 1e-5f);
    }

    // ---- lookAt: rigid, looks down -z, maps the eye to the origin and the target to (0, 0, -d)
    {
        float v[16], eye[3] = {3, 12, 9}, at[3] = {1, 0, -2}, up[3] = {0, 1, 0}, o[3], d;
        cam_look_at(v, eye, at, up);
        for (int r = 0; r < 3; r++) NEAR(v[r * 4] * v[r * 4] + v[r * 4 + 1] * v[r * 4 + 1] + v[r * 4 + 2] * v[r * 4 + 2], 1, 1e-5f);
        d = sqrtf(2 * 2 + 12 * 12 + 11 * 11);
        for (int k = 0; k < 3; k++) o[k] = v[k * 4] * at[0] + v[k * 4 + 1] * at[1] + v[k * 4 + 2] * at[2] + v[k * 4 + 3];
        NEAR(o[0], 0, 1e-4f), NEAR(o[1], 0, 1e-4f), NEAR(o[2], -d, 1e-3f);
        CamRef r;
        float p[16] = {0};
        p[0] = p[5] = 1, p[10] = -1.01f, p[11] = -1, p[14] = -1;
        cam_derive(&r, v, p, NULL);
        NEAR(r.eye[0], 3, 1e-4f), NEAR(r.eye[1], 12, 1e-4f), NEAR(r.eye[2], 9, 1e-4f);
        CHECK(r.ok);
    }

    // ---- the latch of a synthetic game camera: pivot on the floor, near / far / fovy / aspect recovered
    float g[80], out[80];
    game_camera(g, 55, 15, 40, 16.0f / 9.0f, 10, 20);
    CamLatch l;
    cam_latch_from(&l, g, 0x1000, 7);
    CHECK(l.valid);
    NEAR(l.near_, 0.5f, 1e-3f), NEAR(l.far_, 400, 0.5f), NEAR(l.fovy_game, 40 * RAD, 1e-5f);
    CHECK(l.vp_fused == 0);  // (built with the plain product)
    CHECK(l.blk[0] == 0 && l.blk[1] == 0);
    float pv[3];
    CHECK(cam_pivot(&l.ref, NULL, false, false, NULL, NULL, false, pv));
    NEAR(pv[1], 0, 1e-3f);
    float n0[3];
    ndc(g + 32, pv, n0);
    NEAR(n0[0], 0, 1e-4f), NEAR(n0[1], 0, 1e-4f);  // the pivot is the screen centre
    // a side-view zone (thin in z): the middle plane
    {
        float side[80], lo[3] = {0, 0, 2}, hi[3] = {40, 10, 6}, p2[3];
        game_camera(side, 0, 5, 40, 16.0f / 9.0f, 20, 30);
        CamLatch ls;
        cam_latch_from(&ls, side, 0x1000, 1);
        CHECK(cam_pivot(&ls.ref, NULL, false, true, lo, hi, false, p2));
        NEAR(p2[2], 4, 1e-3f), NEAR(p2[0], 20, 1e-3f);
    }
    {   // Link as the pivot, with a fallback to the screen when not published
        float lk[3] = {1, 2, 3}, p2[3];
        cam_pivot(&l.ref, lk, true, false, NULL, NULL, true, p2);
        NEAR(p2[0], 1, 0), NEAR(p2[1], 2, 0), NEAR(p2[2], 3, 0);
        cam_pivot(&l.ref, lk, false, false, NULL, NULL, true, p2);
        NEAR(p2[1], 0, 1e-3f);
    }

    // ---- modes
    CamCfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.cull_fovy = 1;
    CHECK(cam_parse_mode("identity", &cfg) && cfg.kind == CAM_IDENTITY);
    CHECK(cam_parse_mode("passthrough", &cfg) && cfg.kind == CAM_PASSTHROUGH);
    CHECK(cam_parse_mode("off", &cfg) && cfg.kind == CAM_OFF);
    CHECK(cam_parse_mode("orbit:45:60:30", &cfg) && cfg.kind == CAM_ORBIT && cfg.yaw == 45 && cfg.pitch == 60 && cfg.dist == 30 && cfg.fovy == 0);
    CHECK(cam_parse_mode("orbit:-90:35.5:25:60", &cfg) && cfg.yaw == -90 && cfg.pitch == 35.5f && cfg.fovy == 60);
    CHECK(cam_parse_mode("spin:0.5:40:30", &cfg) && cfg.kind == CAM_SPIN && cfg.spin == 0.5f);
    CHECK(!cam_parse_mode("orbit:1:2", &cfg) && !cam_parse_mode("orbit", &cfg) && !cam_parse_mode("bogus", &cfg) && !cam_parse_mode("identity:1", &cfg));
    CHECK(!cam_parse_mode("orbit:1:2:x", &cfg) && !cam_parse_mode("spin:1:2:3:4", &cfg) && !cam_parse_mode("", &cfg));

    // identity: floats 0-31 verbatim, VP bit-equal (plain product); the junk in 48-79 untouched
    cam_parse_mode("identity", &cfg);
    cam_build(&l, &cfg, g, 7, pv, out);
    CHECK(!memcmp(out, g, 80 * sizeof *g));
    // identity with a game VP the rebuild cannot reproduce: the game's bits stay (and the log says DIFFERS)
    {
        float g2[80];
        memcpy(g2, g, sizeof g2);
        g2[33] = nextafterf(g2[33], 1e9f);
        CamLatch l2;
        cam_latch_from(&l2, g2, 0x1000, 8);
        CHECK(l2.vp_fused == -1);
        cam_build(&l2, &cfg, g2, 8, pv, out);
        CHECK(!memcmp(out, g2, 80 * sizeof *g));
    }
    // a game that multiplies with fmaf: detected, identity bit-equal, orbit uses the same product
    {
        float g3[80];
        memcpy(g3, g, sizeof g3);
        cam_mat_mul(g3 + 32, g3 + 16, g3, true);
        CamLatch l3;
        cam_latch_from(&l3, g3, 0x1000, 9);
        CHECK(l3.vp_fused >= 0);
        cam_build(&l3, &cfg, g3, 9, pv, out);
        CHECK(!memcmp(out, g3, 80 * sizeof *g));
    }
    // orbit yaw 0 at the game's own pitch and distance reproduces the game's view (conventions: +z side, +y up, GL lookAt)
    {
        float d[3] = {l.ref.eye[0] - pv[0], l.ref.eye[1] - pv[1], l.ref.eye[2] - pv[2]};
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        CamCfg o;
        memset(&o, 0, sizeof o), o.cull_fovy = 1;
        o.kind = CAM_ORBIT, o.yaw = 0, o.pitch = 55, o.dist = dist;
        cam_build(&l, &o, g, 7, pv, out);
        // (the game camera also sits at x = 10, z = 20 over the floor point it looks at: yaw 0 only when the camera is straight
        // south of it, which it is by construction - but the pitch comes from the geometry, not the 55 degrees given)
        float pitch = asinf(d[1] / dist) / RAD;
        o.pitch = pitch;
        cam_build(&l, &o, g, 7, pv, out);
        for (int k = 0; k < 16; k++) NEAR(out[k], g[k], 2e-4f);
        for (int k = 16; k < 32; k++) NEAR(out[k], g[k], 0);       // projection untouched
        for (int k = 32; k < 48; k++) NEAR(out[k], g[32 + k - 32], 1e-3f * (1 + fabsf(g[k])));
        for (int k = 48; k < 80; k++) NEAR(out[k], g[k], 0);        // 48-79 untouched without FIX48
    }
    // orbit angles: the pivot stays at the screen centre, the eye is on the sphere, yaw 90 looks from +x
    {
        const float angs[][3] = {{0, 35, 30}, {45, 60, 25}, {90, 45, 40}, {180, 35, 30}, {-135, 60, 20}};
        for (unsigned k = 0; k < sizeof angs / sizeof *angs; k++) {
            CamCfg o;
            memset(&o, 0, sizeof o), o.cull_fovy = 1;
            o.kind = CAM_ORBIT, o.yaw = angs[k][0], o.pitch = angs[k][1], o.dist = angs[k][2];
            cam_build(&l, &o, g, 7, pv, out);
            float nd[3];
            ndc(out + 32, pv, nd);
            NEAR(nd[0], 0, 1e-4f), NEAR(nd[1], 0, 1e-4f);
            CamRef r;
            cam_derive(&r, out, out + 16, out + 32);
            float d[3] = {r.eye[0] - pv[0], r.eye[1] - pv[1], r.eye[2] - pv[2]};
            NEAR(sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), angs[k][2], 1e-3f);
            NEAR(d[1], angs[k][2] * sinf(angs[k][1] * RAD), 1e-3f);
            NEAR(d[0], angs[k][2] * cosf(angs[k][1] * RAD) * sinf(angs[k][0] * RAD), 1e-3f);
            NEAR(d[2], angs[k][2] * cosf(angs[k][1] * RAD) * cosf(angs[k][0] * RAD), 1e-3f);
            CHECK(r.up[1] > 0);  // (never upside down)
            // VP is P * V
            float vp[16];
            cam_mat_mul(vp, out + 16, out, false);
            for (int m = 0; m < 16; m++) NEAR(vp[m], out[32 + m], 1e-4f * (1 + fabsf(vp[m])));
        }
    }
    // fovy: explicit replaces the angle and keeps the aspect; far: depth terms recomputed, near kept
    {
        CamCfg o;
        memset(&o, 0, sizeof o), o.cull_fovy = 1;
        cam_parse_mode("orbit:0:50:30:70", &o);
        o.far_add = 200;
        cam_build(&l, &o, g, 7, pv, out);
        NEAR(2 * atanf(1 / out[21]), 70 * RAD, 1e-5f);
        NEAR(out[21] / out[16], 16.0f / 9.0f, 1e-4f);
        float n = out[27] / (out[26] - 1), f = out[27] / (out[26] + 1);
        NEAR(n, 0.5f, 1e-3f), NEAR(f, 600, 0.5f);
        NEAR(out[30], -1, 0), NEAR(out[31], 0, 0);
        o.far_add = 0, o.fovy = 0;  // (nothing asked: the game's projection bit for bit)
        cam_build(&l, &o, g, 7, pv, out);
        CHECK(!memcmp(out + 16, g + 16, 64));
        // the culling fovy scale widens the game's own angle: the default orbit angle is the unscaled one
        float gs[80];
        game_camera(gs, 55, 15, 40 * 1.5f, 16.0f / 9.0f, 10, 20);
        CamLatch ls;
        cam_latch_from(&ls, gs, 0x1000, 3);
        o.cull_aspect = 3.2f, o.cull_fovy = 1.5f;
        cam_build(&ls, &o, gs, 3, pv, out);
        NEAR(2 * atanf(1 / out[21]), 40 * RAD, 1e-5f);
    }
    // spin: the yaw follows the frame number
    {
        CamCfg o;
        memset(&o, 0, sizeof o), o.cull_fovy = 1;
        cam_parse_mode("spin:2:45:30", &o);
        float oa[80], ob[80];
        cam_build(&l, &o, g, 0, pv, oa);
        cam_build(&l, &o, g, 45, pv, ob);   // 90 degrees later
        CamRef ra, rb;
        cam_derive(&ra, oa, oa + 16, oa + 32), cam_derive(&rb, ob, ob + 16, ob + 32);
        NEAR(ra.eye[2] - pv[2], 30 * cosf(45 * RAD), 1e-3f);
        NEAR(rb.eye[0] - pv[0], 30 * cosf(45 * RAD), 1e-3f);
    }

    // ---- floats 48-79: block / group classification and the FIX48 rewrite
    {
        float gx[80];
        memcpy(gx, g, sizeof gx);
        CamRef r0;
        cam_derive(&r0, g, g + 16, g + 32);
        memcpy(gx + 48, r0.iVP, 64);                     // 48-63 = inverse view-projection
        gx[64] = r0.eye[0], gx[65] = r0.eye[1], gx[66] = r0.eye[2], gx[67] = 1;  // 64-67 = eye position, w 1
        gx[68] = r0.fwd[0], gx[69] = r0.fwd[1], gx[70] = r0.fwd[2], gx[71] = 0;
        for (int i = 72; i < 80; i++) gx[i] = 123.0f + (float)i;
        CamLatch lx;
        cam_latch_from(&lx, gx, 0x2000, 11);
        CHECK(lx.blk[0] == 6 && lx.blk[1] == 0);        // inv(view-proj)
        CHECK(lx.grp[4] == 1 && lx.grp[5] == 2 && lx.grp[6] == 0 && lx.grp[7] == 0);
        CamCfg o;
        memset(&o, 0, sizeof o), o.cull_fovy = 1;
        cam_parse_mode("orbit:60:45:25", &o);
        cam_build(&lx, &o, gx, 11, pv, out);
        for (int k = 48; k < 80; k++) NEAR(out[k], gx[k], 0);  // default: left as the game has them
        o.fix48 = 1;
        cam_build(&lx, &o, gx, 11, pv, out);
        CamRef rn;
        cam_derive(&rn, out, out + 16, out + 32);
        for (int k = 0; k < 16; k++) NEAR(out[48 + k], rn.iVP[k], 1e-4f * (1 + fabsf(rn.iVP[k])));
        for (int k = 0; k < 3; k++) NEAR(out[64 + k], rn.eye[k], 1e-4f), NEAR(out[68 + k], rn.fwd[k], 1e-5f);
        NEAR(out[67], 1, 0), NEAR(out[71], 0, 0);       // (w kept)
        for (int k = 72; k < 80; k++) NEAR(out[k], gx[k], 0);
        // the inverse of the new VP really inverts it
        float chk[16];
        cam_mat_mul(chk, out + 32, out + 48, false);
        for (int k = 0; k < 16; k++) NEAR(chk[k], i4[k], 2e-3f);
        // identity never touches 48-79, FIX48 or not
        cam_parse_mode("identity", &o);
        cam_build(&lx, &o, gx, 11, pv, out);
        CHECK(!memcmp(out, gx, sizeof gx));
    }

    // ---- the matrix scan: a view-projection copy and an inverse view hidden in an ordinary uniform buffer
    {
        float ub[64];
        for (int i = 0; i < 64; i++) ub[i] = 0.1f * (float)i - 2;
        memcpy(ub + 16, g + 32, 64);                     // offset 64: VP
        CamRef r0;
        cam_derive(&r0, g, g + 16, g + 32);
        float t[16];
        for (int rr = 0; rr < 4; rr++) for (int cc = 0; cc < 4; cc++) t[cc * 4 + rr] = r0.iV[rr * 4 + cc];
        memcpy(ub + 32, t, 64);                          // offset 128: inv(view) transposed
        memcpy(ub + 48, g, 48);                          // offset 192: rows 0-2 of the view only (the buffer ends after 12 floats of it)
        hits = 0;
        int n = cam_scan_matrices(ub, 192 + 48, &r0, on_hit, NULL);
        CHECK(n == 3 && hits == 3);
        CHECK(hitoff[0] == 64 && !strcmp(hitwhat[0], "view-proj"));
        CHECK(hitoff[1] == 128 && !strcmp(hitwhat[1], "inv(view)^T"));
        CHECK(hitoff[2] == 192 && strstr(hitwhat[2], "view (rows 0-2 only)"));
        // a buffer of other data, an identity matrix and a zero buffer match nothing
        float other[64] = {0};
        memcpy(other, i4, 64), memcpy(other + 16, g + 16, 0);
        hits = 0;
        CHECK(cam_scan_matrices(other, 256, &r0, on_hit, NULL) == 0);
        CHECK(cam_scan_matrices(ub, 32, &r0, on_hit, NULL) == 0);  // (too small for a matrix)
    }

    // ---- the substitute through the guest-memory entry point (cbuf_info's view): env-configured, bound range checked
    {
        setenv("LARECOMP_FREECAM", "orbit:90:45:25", 1);
        setenv("LARECOMP_FREECAM_CULL", "3.2:1.4", 1);
        setenv("LARECOMP_FREECAM_AT", "100:identity/stock,200:orbit:10:35:20:55/2.4:1.2,300:off", 1);
        cam_override_init();
        CHECK(g_cam_on && g_cam_cull);
        NEAR(cam_override_cull_aspect(), 3.2f, 0), NEAR(cam_override_fovy_scale(), 1.4f, 0);
        static float guest[80] __attribute__((aligned(16)));
        game_camera(guest, 55, 15, 40, 16.0f / 9.0f, 10, 20);
        cam_override_latch(guest, (uintptr_t)guest, 320, 50);
        uint8_t buf[320];
        CHECK(cam_override_ubo(0, (uintptr_t)guest, 320, buf));                  // the main camera: substituted
        float *f = (float *)buf;
        CamRef rr;
        cam_derive(&rr, f, f + 16, f + 32);
        CHECK(rr.ok);
        CHECK(memcmp(buf, guest, 128) != 0);
        CHECK(!memcmp(buf + 192, guest + 48, 128));                              // 48-79 as the game has them
        CHECK(!cam_override_ubo(0, (uintptr_t)guest + 64, 320, buf));            // another address
        CHECK(!cam_override_ubo(0, (uintptr_t)guest, 144, buf));                 // another size
        static float other[80] __attribute__((aligned(16)));
        memcpy(other, guest, sizeof other), other[0] += 0.5f;
        cam_override_latch(other, (uintptr_t)other, 320, 50);                    // (same frame: the first stays the latch)
        CHECK(!cam_override_ubo(0, (uintptr_t)other, 320, buf));
        guest[5] += 0.25f;                                                       // the address now holds another camera
        CHECK(!cam_override_ubo(0, (uintptr_t)guest, 320, buf));
        guest[5] -= 0.25f;
        // the schedule: identity at 100 (bit-exact), orbit at 200, off at 300
        cam_override_frame(99);
        CHECK(cam_override_ubo(0, (uintptr_t)guest, 320, buf) && memcmp(buf, guest, 128) != 0);
        cam_override_frame(100);
        CHECK(cam_override_ubo(0, (uintptr_t)guest, 320, buf) && !memcmp(buf, guest, 320));
        CHECK(!g_cam_cull);                                                      // "/stock"
        cam_override_frame(250);
        CHECK(g_cam_cull);
        NEAR(cam_override_cull_aspect(), 2.4f, 0), NEAR(cam_override_fovy_scale(), 1.2f, 0);
        CHECK(cam_override_ubo(0, (uintptr_t)guest, 320, buf));
        NEAR(2 * atanf(1 / ((float *)buf)[21]), 55 * RAD, 1e-5f);
        cam_override_frame(300);
        CHECK(!cam_override_ubo(0, (uintptr_t)guest, 320, buf));                 // off: the guest's own
        // nothing set: off everywhere
        unsetenv("LARECOMP_FREECAM"), unsetenv("LARECOMP_FREECAM_CULL"), unsetenv("LARECOMP_FREECAM_AT");
        cam_override_init();
        CHECK(!g_cam_on && !g_cam_cull && !g_cam_log);
        NEAR(cam_override_cull_aspect(), 0, 0), NEAR(cam_override_fovy_scale(), 1, 0);
        // bad values are refused, not half-applied
        setenv("LARECOMP_FREECAM", "orbit:1:2", 1);
        cam_override_init();
        CHECK(!g_cam_on);
        setenv("LARECOMP_FREECAM_LOG", "1", 1);
        cam_override_init();
        CHECK(g_cam_on && g_cam_log);
    }

    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    printf("cam_override: all checks passed\n");
    return 0;
}
