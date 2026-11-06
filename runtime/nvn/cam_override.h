// Free-camera probe (AwakeningFrame lane 1): replaces the game's main camera in the vertex uniform buffer the
// renderer binds (guest cbuf slot 5 = hardware cbuf 8 = GLSL binding 8, 320 bytes: floats 0-15 view, 16-31
// projection, 32-47 view-projection, row-major, column vectors: clip = P * V * x, GL-style depth [-1, 1]).
// Backend neutral; the stereo executor (lane 4) reuses the latch / rewrite entry points.
//
// Everything is off unless one of these is set (the hot path is one test of g_cam_on / g_cam_cull / g_cam_log):
//   LARECOMP_FREECAM=identity | passthrough | orbit:<yaw>:<pitch>:<dist>[:<fovy>] | spin:<deg/frame>:<pitch>:<dist>
//   LARECOMP_FREECAM_PIVOT=screen | link      (what the orbit turns around; default screen)
//   LARECOMP_FREECAM_CULL=<aspect>:<fovy scale>   (widened culling; "stock" or unset: the game's own)
//   LARECOMP_FREECAM_AT=<frame>:<mode>[/<cull>][,...]   schedule: <mode> is a FREECAM value or "off", <cull> as above
//   LARECOMP_FREECAM_FAR=<units>   push the far plane out by this much (default 0: the game's depth terms kept)
//   LARECOMP_FREECAM_FIX48=1       also rewrite camera-derived blocks found in floats 48-79 (see docs/af/FREECAM.md)
//   LARECOMP_FREECAM_LOG=1         probe log; LARECOMP_FREECAM_LOG_EVERY=<n> frames between full scans (default 60)
// yaw / pitch / fovy in degrees; dist in game units. Docs: docs/af/FREECAM.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct CamLatch;

extern volatile int g_cam_on;    // a mode, a schedule or the log is set: the latch / rewrite hooks run
extern volatile int g_cam_cull;  // a culling override is set
extern volatile int g_cam_log;   // LARECOMP_FREECAM_LOG

static inline bool cam_override_active(void) { return __builtin_expect(g_cam_on != 0, 0); }
static inline bool cam_override_logging(void) { return __builtin_expect(g_cam_log != 0, 0); }

void cam_override_init(void);  // reads the environment (also run at startup)
// Once per presented frame (widescreen_frame_end): advances the schedule.
void cam_override_frame(uint64_t frame);
// Published by widescreen.c each frame while the probe is on: Link's position (game units), the camera zone's box.
void cam_override_set_link(const float pos[3]);
void cam_override_set_zone(const float lo[3], const float hi[3]);
bool cam_override_zone(float lo[3], float hi[3]);  // the latest zone box (false: none)
// The frame's main camera: called per draw with the vertex stage's slot-5 range (exec.c, next to
// widescreen_scene_ubo); the first perspective 320-byte buffer of a frame is latched (address, original matrices).
void cam_override_latch(const float *u, uint64_t addr, uint64_t size, uint64_t frame);
// The substitute for a bound range (any stage, any slot): true when the range is the latched main camera, with the
// 320 bytes in out320 (the guest bytes with floats 0-47 replaced; 48-79 as the guest has them unless FIX48).
bool cam_override_ubo(int stage, uint64_t addr, uint64_t size, void *out320);
// Any other bound buffer (48..4096 bytes): a copy with the camera's matrices (and eye position) found in it replaced by the
// substitute camera's; false (nothing written) when nothing matched or the probe is off / identity / passthrough.
bool cam_override_patch(const void *guest, uint64_t size, void *out);
// Changes whenever the substitutes may change (a new latch, a new configuration): a substitute made for a buffer stays
// right while this, the eye and the buffer's bytes stay the same.
uint32_t cam_override_epoch(void);

// Per-draw probe info (LARECOMP_FREECAM_LOG): filled by exec.c on every draw while logging.
typedef struct {
    uint64_t fs_hash, vs_hash;      // shader code hashes (fragment: as g_cur_fs_hash)
    const uint64_t *ubo[2];         // per stage (vertex, fragment): 18 { addr, size } pairs, as st.ubuf[stage]
    bool into_scalable;             // draws into a 16:9 scalable colour target
    bool samples_depth;             // the fragment shader samples a depth texture
    uint64_t frame;                 // renderer_exec_frame()
} CamDrawInfo;
void cam_override_draw(const CamDrawInfo *d);

// ---- stereo (AwakeningFrame lane 4, runtime/nvn/vulkan/stereo.c): one substitute camera per eye
// fn is called once per latched frame (the render thread, at the frame's first main-camera draw) with the game's camera
// and the screen-centre pivot on the floor; it fills each eye's view and projection (game units, the game's conventions:
// row-major, clip = P * V * x, view down -z) and returns false when there are none (no head pose yet: the game's own
// camera is kept). identity: both eyes get the game's own bytes (LARECOMP_STEREO_TEST=identity: eye 0 == eye 1 == flat).
// Turns the probe's hooks on; widened culling (3.2:1.5) unless LARECOMP_FREECAM_CULL says otherwise.
typedef bool (*CamEyeFn)(const struct CamLatch *l, const float pivot[3], float V[2][16], float P[2][16]);
void cam_override_stereo(CamEyeFn fn, bool identity);
// (stereo.c, once per latch) where the table's centre sits in the world (the followed anchor, not the raw pivot)
void cam_override_table_anchor(const float a[3]);
void cam_override_set_eye(int eye);         // the eye being executed (0 / 1)
bool cam_override_eye_ready(void);          // the latched frame has eye cameras (substitution is on for it)
// Per eye the 320 bytes get floats 0-47 (view, projection, view-projection), 48-63 (the rotation-only block: rows
// right, back, down as the game has them), 64-67 (back) and 68-71 (eye position) rewritten: every 4-float group of
// 48-79 whose xyz is +-right / +-up / +-back / the eye position of the game's camera at the latch.

// widescreen.c culling hooks: 0 / 1 when no override.
float cam_override_cull_aspect(void);   // absolute culling aspect, or 0
float cam_override_fovy_scale(void);    // multiplier for rt_fovy's angle, or 1

// ---- internals, exposed for tools/test_cam_override.c
typedef struct {
    float V[16], P[16], VP[16], iV[16], iP[16], iVP[16];
    float eye[3], fwd[3], right[3], up[3];
    bool ok;
} CamRef;
typedef enum { CAM_OFF, CAM_IDENTITY, CAM_PASSTHROUGH, CAM_ORBIT, CAM_SPIN, CAM_STEREO } CamKind;
typedef struct {
    CamKind kind;
    float yaw, pitch, dist, fovy, spin;  // degrees, units; fovy 0 = the game's; spin: degrees / frame
    int pivot_link;
    float far_add;
    int fix48;
    float cull_aspect, cull_fovy;        // cull_aspect 0 = stock
} CamCfg;
typedef struct CamLatch {               // the frame's main camera
    bool valid;
    uint64_t addr, frame;
    float orig[80];
    CamRef ref;
    float id_vp[16];                    // identity: the rebuilt view-projection when bit-equal to the game's, else the game's
    int vp_fused;                       // the game's VP product matched fmaf() (1), plain (0), neither (-1)
    uint8_t blk[2], grp[8];             // what floats 48-63 / 64-79 (16-float blocks) and each 4-float group look like
    float near_, far_, fovy_game;
    int8_t axis[8];                     // (stereo) floats 48-79 by 4-float group: +-1 right, +-2 up, +-3 back (-forward), 4 eye
} CamLatch;
void cam_mat_mul(float *out, const float *a, const float *b, bool fused);
bool cam_mat_inv(float *out, const float *m);
void cam_look_at(float *v, const float eye[3], const float at[3], const float up[3]);
void cam_derive(CamRef *r, const float *v, const float *p, const float *vp);
bool cam_parse_mode(const char *s, CamCfg *c);
bool cam_pivot(const CamRef *r, const float link[3], bool link_ok, bool zone_ok, const float zlo[3], const float zhi[3],
               bool use_link, float out[3]);
void cam_latch_from(CamLatch *l, const float *u, uint64_t addr, uint64_t frame);
void cam_build(const CamLatch *l, const CamCfg *c, const float *guest, uint64_t frame, const float pivot[3], float *out);
// matrix-likeness scan of a buffer (floats, 16-byte steps): calls cb for every match; returns the count
typedef void (*CamMatchFn)(void *user, unsigned off, const char *what);
int cam_scan_matrices(const float *u, unsigned size, const CamRef *r, CamMatchFn cb, void *user);
