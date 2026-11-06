// Edge treatment of the VR diorama: edge.h, docs/af/EDGE.md.
//
// Passes per eye (all in the frame's command buffer; images live in GENERAL, a full barrier between passes):
//   cov    1/8 size: the share of each 8x8 block of the eye that has geometry (scene depth < far)       -> mask A
//   blurH  mask A, a wide soft kernel along x                                                           -> mask B
//   blurV  mask B, the same along y                                                                     -> mask A
//   tint   eye 0 only, a 2x1 image: the average colour of the geometry in the top and the bottom band of the
//          eye, moved a little toward each frame's value (reads last frame's from the other of two images)
//   main   the XR eye image: colour, depth and mask read once each; backdrop, fog, edge fade, dither
// Mode fade is the main pass alone: each pixel's point on the table (its depth, or where its ray meets the floor) is
// darkened by its distance across the floor from the table's centre (a round fade to black), dithered.
// The mask, tint and everything else are device local; nothing is read back.
#include "edge.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../platform.h"
#include "../../rt.h"
#include "../screen_filter.h"
#include "../vr_cull.h"
#include "foveation.h"
#include "record.h"
#include "stereo.h"
#include "vk.h"

// ------------------------------------------------------------------------------------------------ settings
static struct {
    bool init;
    EdgeMode mode;
    float fog_start, fog_end;  // multiples of the pivot distance
    float soft;                // the edge fade's radius, as a share of the image height
    float strength;            // 0..1 of the edge fade
    float rect_soft, rect_strength;  // the floor quadrilateral fade: share of its band (1), 0..1
    float sky[3], skymix;
    float top[3], bot[3];      // the colours the backdrop starts with (and LARECOMP_VR_EDGE_TOP / _BOTTOM: kept)
    bool fixed_colours;
    float desat, bright;       // how the scene's average is turned into haze
    float tau;                 // seconds the backdrop colours take to follow the scene
    int debug;
    bool indoors;              // also in interiors and dungeons (default: the overworld only)
    float radius, width;       // fade: clear within radius of the table's centre, black past radius + width (game units)
    float room_margin;         // fade in a room: the radius reaches at least this past the room's farthest corner
    float pivot_default;
    bool smooth_off;           // LARECOMP_VR_EDGE_SMOOTH=0: the foveated passes' coarse blocks are left as they are
    bool fit;                  // fade, overworld: fitted inside the floor the culling draws (LARECOMP_VR_EDGE_FIT=0: not)
} cfg;

// fade's defaults (game units; a screen is about 15 x 12, a tile 1.5; docs/af/EDGE.md "Fade")
#define FADE_RADIUS 13.0f
#define FADE_WIDTH 12.0f
#define FADE_ROOM_MARGIN 1.5f

static bool hex3(const char *s, float out[3]) {
    unsigned v;
    if (!s || sscanf(s[0] == '#' ? s + 1 : s, "%x", &v) != 1) return false;
    out[0] = ((v >> 16) & 255) / 255.0f, out[1] = ((v >> 8) & 255) / 255.0f, out[2] = (v & 255) / 255.0f;
    return true;
}

static void cfg_init(void) {
    const char *e;
    cfg.init = true;
    cfg.mode = EDGE_BLACK;  // (off until asked: LARECOMP_VR_EDGE=fog)
    if ((e = getenv("LARECOMP_VR_EDGE"))) {
        if (!strcmp(e, "black") || !strcmp(e, "off") || !strcmp(e, "0") || !strcmp(e, "none")) cfg.mode = EDGE_BLACK;
        else if (!strcmp(e, "fog") || !strcmp(e, "1")) cfg.mode = EDGE_FOG;
        else if (!strcmp(e, "gradient")) cfg.mode = EDGE_GRADIENT;
        else if (!strcmp(e, "passthrough")) cfg.mode = EDGE_PASSTHROUGH;
        else if (!strcmp(e, "fade")) cfg.mode = EDGE_FADE;
        else if (*e) LOG("[edge] LARECOMP_VR_EDGE=%s unknown (fade, passthrough, black, fog, gradient): off\n", e);
    }
    cfg.fog_start = 1e6f, cfg.fog_end = 2e6f;  // (distance fog is off unless LARECOMP_VR_EDGE_FOG asks: only the void band fades)
    if ((e = getenv("LARECOMP_VR_EDGE_FOG"))) {
        float a, b;
        if (sscanf(e, "%f,%f", &a, &b) == 2 && a >= 0 && b > a) cfg.fog_start = a, cfg.fog_end = b;
    }
    cfg.soft = (e = getenv("LARECOMP_VR_EDGE_SOFT")) && atof(e) >= 0 ? (float)atof(e) : 0.025f;
    cfg.strength = (e = getenv("LARECOMP_VR_EDGE_STRENGTH")) ? fminf(fmaxf((float)atof(e), 0.0f), 1.0f) : 1.0f;
    cfg.rect_soft = (e = getenv("LARECOMP_VR_EDGE_RECT")) && atof(e) >= 0 ? (float)atof(e) : 0.0f;  // (the culling quadrilateral fade: opt in)
    cfg.sky[0] = 0.80f, cfg.sky[1] = 0.89f, cfg.sky[2] = 0.96f, cfg.skymix = (e = getenv("LARECOMP_VR_EDGE_SKYMIX")) ? fminf(fmaxf((float)atof(e), 0.0f), 1.0f) : 0.85f;
    hex3(getenv("LARECOMP_VR_EDGE_SKY"), cfg.sky);
    cfg.rect_strength = cfg.rect_soft > 0 ? 1.0f : 0.0f;
    cfg.top[0] = 0.36f, cfg.top[1] = 0.46f, cfg.top[2] = 0.40f;
    cfg.bot[0] = 0.16f, cfg.bot[1] = 0.24f, cfg.bot[2] = 0.18f;
    bool t = hex3(getenv("LARECOMP_VR_EDGE_TOP"), cfg.top), b = hex3(getenv("LARECOMP_VR_EDGE_BOTTOM"), cfg.bot);
    cfg.fixed_colours = t || b;
    cfg.desat = 0.30f, cfg.bright = 0.92f;
    if ((e = getenv("LARECOMP_VR_EDGE_HAZE"))) {
        float a, c;
        if (sscanf(e, "%f,%f", &a, &c) == 2) cfg.desat = a, cfg.bright = c;
    }
    cfg.tau = (e = getenv("LARECOMP_VR_EDGE_TAU")) && atof(e) > 0 ? (float)atof(e) : 0.8f;
    cfg.indoors = (e = getenv("LARECOMP_VR_EDGE_INDOORS")) && atoi(e) > 0;
    cfg.debug = (e = getenv("LARECOMP_VR_EDGE_DEBUG")) ? atoi(e) : 0;
    cfg.radius = (e = getenv("LARECOMP_VR_EDGE_RADIUS")) && atof(e) > 0 ? (float)atof(e) : FADE_RADIUS;
    cfg.width = (e = getenv("LARECOMP_VR_EDGE_WIDTH")) && atof(e) > 0 ? (float)atof(e) : FADE_WIDTH;
    cfg.room_margin = FADE_ROOM_MARGIN;
    cfg.smooth_off = (e = getenv("LARECOMP_VR_EDGE_SMOOTH")) && atoi(e) == 0;
    cfg.fit = !((e = getenv("LARECOMP_VR_EDGE_FIT")) && atoi(e) == 0);
    float s = (e = getenv("LARECOMP_VR_SCALE")) && atof(e) > 0 ? (float)atof(e) : 0.067f;
    float d = (e = getenv("LARECOMP_VR_DIST")) && atof(e) > 0 ? (float)atof(e) : 1.0f;
    cfg.pivot_default = d / s;
    if (cfg.mode == EDGE_FADE)
        LOG("[edge] mode fade: clear within %.1f units of the table's centre, black past %.1f (rooms: their farthest corner + %.1f at least)\n",
            cfg.radius, cfg.radius + cfg.width, cfg.room_margin);
    else
    LOG("[edge] mode %s, fog %.2f..%.2f x pivot distance, edge fade radius %.3f of the height x %.2f, haze desat %.2f x%.2f%s\n",
        cfg.mode == EDGE_BLACK ? "black" : cfg.mode == EDGE_FOG ? "fog" : cfg.mode == EDGE_GRADIENT ? "gradient" : cfg.mode == EDGE_FADE ? "fade" : "passthrough",
        cfg.fog_start, cfg.fog_end, cfg.soft, cfg.strength, cfg.desat, cfg.bright, cfg.fixed_colours ? ", colours fixed" : "");
}

// The mode is read by three threads (the present: stereo_eye_post / edge_eye; the XR thread: edge_passthrough_wanted)
// and changed by the VR tab (edge_set_mode): atomic. The passes' pipelines are one shader for every mode (the mode is a
// uniform) and made at the first treated eye, so a switch only changes what the next eye is drawn with.
static int g_mode = -1;
EdgeMode edge_mode(void) {
    if (!cfg.init) cfg_init();
    int m = __atomic_load_n(&g_mode, __ATOMIC_ACQUIRE);
    if (m < 0) {
        int want = -1;
        __atomic_compare_exchange_n(&g_mode, &want, (int)cfg.mode, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        m = __atomic_load_n(&g_mode, __ATOMIC_ACQUIRE);
    }
    return (EdgeMode)m;
}
bool edge_passthrough_wanted(void) { return edge_mode() == EDGE_PASSTHROUGH; }
bool edge_woods_fog_hidden(void) {
    static int keep = -1;
    if (keep < 0) keep = getenv("LARECOMP_VR_WOODS_FOG") && atoi(getenv("LARECOMP_VR_WOODS_FOG")) > 0;
    return !keep;
}

// The VR tab's Table Edge (runtime/vr_settings.c; VRS_EDGE_OFF / FADE / PASSTHROUGH = black, fade, passthrough).
static _Atomic bool g_mode_changed;
void edge_set_mode(int vrs) {
    static const EdgeMode k[3] = {EDGE_BLACK, EDGE_FADE, EDGE_PASSTHROUGH};
    if (vrs < 0 || vrs > 2) return;
    EdgeMode old = edge_mode();
    if (old == k[vrs]) return;
    __atomic_store_n(&g_mode, (int)k[vrs], __ATOMIC_RELEASE), g_mode_changed = true;
    LOG("[edge] mode %s (VR settings; was %s)\n", vrs == 0 ? "black" : vrs == 1 ? "fade" : "passthrough",
        old == EDGE_BLACK ? "black" : old == EDGE_FADE ? "fade" : old == EDGE_PASSTHROUGH ? "passthrough" : old == EDGE_FOG ? "fog" : "gradient");
}

// ------------------------------------------------------------------------------------------------ shaders
// One descriptor set for every pass: 0 the frame's values, 1 depth, 2 colour, 3 mask, 4 backdrop colours.
#define PRELUDE                                                                                                        \
    "#version 450\n"                                                                                                   \
    "layout(std140, set = 0, binding = 0) uniform U {\n"                                                               \
    "  vec4 proj;  // P00 P11 P02 P12\n"                                                                               \
    "  vec4 zt;    // A = P[2][2], B = P[2][3], zn, zf - zn\n"                                                         \
    "  vec4 fl;    // depth in -1..1, flip, mode, frame seed\n"                                                        \
    "  vec4 up;    // the world's up in eye space, pivot distance\n"                                                   \
    "  vec4 cc;    // colour crop xy, depth crop zw\n"                                                                 \
    "  vec4 sz;    // output w h, mask w h\n"                                                                          \
    "  vec4 fog;   // start, end (units), edge fade radius (mask texels), strength\n"                                  \
    "  vec4 top;   // starting top colour, 1: the first frame\n"                                                       \
    "  vec4 bot;   // starting bottom colour, the smoothing step\n"                                                    \
    "  vec4 mi;    // desaturation, brightness, debug, fixed colours\n"                                               \
    "  vec4 vr[3]; // the view's rows (world -> eye), translation in w\n"                                             \
    "  vec4 rq[2]; // the table's floor quadrilateral: x0 z0 x1 z1 / x2 z2 x3 z3 (counter-clockwise)\n"               \
    "  vec4 rc;    // quadrilateral on, floor y, unused, strength\n"                                                  \
    "  vec4 rb;    // the fade band at each edge of the quadrilateral (game units)\n"                                  \
    "  vec4 sky;   // the sky tone, how much of the backdrop it takes\n"                                               \
    "  vec4 fd;    // fade: the table's centre (world), radius (units)\n"                                               \
    "  vec4 fw;    // fade: width (units), -, -, -\n"                                                                   \
    "  vec4 pf;    // the present's screen filter: mode, the rig's blink (screen_filter.h)\n"                         \
    "  vec4 va;    // foveation (fov_shade): on, foci, r0, r1\n"                                                      \
    "  vec4 vc;    // the foci in the colour image's pixels: x0 y0 x1 y1\n"                                            \
    "  vec4 vb;    // the filled part w h, the middle ring's rate (log2 w, h)\n"                                       \
    "  vec4 vd;    // the outer rate (log2 w, h), the colour image's size\n"                                           \
    "  vec4 dq[2]; // fade: the floor the culling draws: x0 z0 x1 z1 / x2 z2 x3 z3 (counter-clockwise)\n"              \
    "  vec4 dk;    // fade: that floor on, the fade band inside its edges, the margin (units)\n"                        \
    "  vec4 ls;    // fade, foveated: the block-centre images' sizes (middle ring w h, outer w h)\n"                    \
    "} u;\n"                                                                                                           \
    "layout(set = 0, binding = 1) uniform sampler2D dt;\n"                                                             \
    "layout(set = 0, binding = 2) uniform sampler2D ct;\n"                                                             \
    "layout(set = 0, binding = 3) uniform sampler2D mk;\n"                                                             \
    "layout(set = 0, binding = 4) uniform sampler2D tn;\n"                                                             \
    "layout(location = 0) out vec4 o;\n"                                                                               \
    "// the present's mapping from the output to the eye image (exec.c vk_stereo_eyes, present.vert)\n"                \
    "vec2 qmap(vec2 p) { return vec2(p.x, u.fl.y > 0.5 ? 1.0 - p.y : p.y); }\n"                                        \
    "float dsample(vec2 q) { return texture(dt, q * u.cc.zw).r; }\n"                                                   \
    "bool is_geo(float d) { return d < 0.9999; }\n"                                                                  \
    "// the shading rate (log2 w, h) the foveated passes gave the 8 px texel holding colour pixel sp (foveation.c)\n" \
    "ivec2 vrs_rate(vec2 sp) {\n"                                                                                     \
    "  vec2 x = (floor(sp / 8.0) + 0.5) * 8.0;\n"                                                                     \
    "  if (x.x > u.vb.x + 8.0 || x.y > u.vb.y + 8.0) return ivec2(2);\n"                                              \
    "  ivec2 r = ivec2(2);\n"                                                                                         \
    "  for (int f = 0; f < 2; f++) {\n"                                                                               \
    "    if (float(f) >= u.va.y) break;\n"                                                                            \
    "    float d = length((x - (f == 0 ? u.vc.xy : u.vc.zw)) / (0.5 * u.vb.xy));\n"                                   \
    "    r = min(r, d < u.va.z ? ivec2(0) : d < u.va.w ? ivec2(u.vb.zw) : ivec2(u.vd.xy));\n"                         \
    "  }\n"                                                                                                           \
    "  return r;\n"                                                                                                   \
    "}\n"                                                                                                             \
    "// the eye's colour at q, with the foveated passes' coarse blocks smoothed: where a block of b pixels was shaded\n" \
    "// once, the colour is interpolated between the block centres (bilinear), so no block edges show\n"            \
    "vec3 colour(vec2 q) {\n"                                                                                         \
    "  vec2 uv = q * u.cc.xy;\n"                                                                                      \
    "  if (u.va.x < 0.5) return texture(ct, uv).rgb;\n"                                                               \
    "  vec2 sp = uv * u.vd.zw;\n"                                                                                     \
    "  ivec2 lr = vrs_rate(sp);\n"                                                                                    \
    "  if (lr.x + lr.y == 0) return texture(ct, uv).rgb;\n"                                                           \
    "  vec2 b = vec2(float(1 << lr.x), float(1 << lr.y));\n"                                                          \
    "  vec2 f = sp / b - 0.5, i0 = floor(f), t = f - i0;\n"                                                           \
    "  vec2 c0 = (i0 + 0.5) * b / u.vd.zw, c1 = (i0 + 1.5) * b / u.vd.zw;\n"                                          \
    "  vec3 a = mix(texture(ct, c0).rgb, texture(ct, vec2(c1.x, c0.y)).rgb, t.x);\n"                                   \
    "  vec3 e = mix(texture(ct, vec2(c0.x, c1.y)).rgb, texture(ct, c1).rgb, t.x);\n"                                   \
    "  return mix(a, e, t.y);\n"                                                                                      \
    "}\n"

static const char *vs_src =
    "#version 450\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// void share: each mask texel is an 8x8 block of the eye (all 64 pixels read, none skipped: a thin tree line or a gap in
// the terrain keeps its true share), turned into a void weight (thin slivers and lone pixels count for nothing)
static const char *cov_fs =
    PRELUDE
    "void main() {\n"
    "  vec2 blk = floor(gl_FragCoord.xy);\n"
    "  float n = 0.0;\n"
    "  for (int j = 0; j < 8; j++) for (int i = 0; i < 8; i++) {\n"
    "    vec2 p = (blk * 8.0 + vec2(0.5 + float(i), 0.5 + float(j))) / u.sz.xy;\n"
    "    n += is_geo(dsample(qmap(p))) ? 0.0 : 1.0;\n"
    "  }\n"
    "  o = vec4(smoothstep(0.35, 0.65, n / 64.0), 0.0, 0.0, 1.0);\n"
    "}\n";

// how near the void is: a separable gaussian-weighted MAX of the void weight (the product of the two passes' weights is
// the radial gaussian, so this is a soft distance to the nearest void: 1 at the void, 0.6 one sigma away, 0.14 at two
// and a half; it does not depend on how big the void is, unlike an average). 19 taps over +-radius texels (fog.z)
// (direction in fl.w: 0 x, 1 y)
static const char *blur_fs =
    PRELUDE
    "void main() {\n"
    "  vec2 p = gl_FragCoord.xy / u.sz.zw;\n"
    "  vec2 dir = (u.fl.w < 0.5 ? vec2(1.0, 0.0) : vec2(0.0, 1.0)) / u.sz.zw;\n"
    "  float sg = max(u.fog.z, 0.5) / 2.5, best = 0.0;\n"
    "  for (int k = -9; k <= 9; k++) {\n"
    "    float d = float(k) * u.fog.z / 9.0;\n"
    "    float w = exp(-d * d / (2.0 * sg * sg));\n"
    "    best = max(best, w * texture(mk, clamp(p + dir * d, vec2(0.0), vec2(1.0))).r);\n"
    "  }\n"
    "  o = vec4(best, 0.0, 0.0, 1.0);\n"
    "}\n";

// the backdrop colours: texel 0 the top band's average colour, texel 1 the bottom's, over the geometry only
static const char *tint_fs =
    PRELUDE
    "void main() {\n"
    "  float band = floor(gl_FragCoord.x);\n"
    "  vec4 prev = band < 0.5 ? vec4(u.top.rgb, 1.0) : vec4(u.bot.rgb, 1.0);\n"
    "  if (u.top.w < 0.5) prev = texelFetch(tn, ivec2(int(band), 0), 0);\n"
    "  if (u.mi.w > 0.5) { o = prev; return; }\n"
    "  vec3 sum = vec3(0.0);\n"
    "  float n = 0.0;\n"
    "  float y0 = band < 0.5 ? 0.0 : 0.86, y1 = band < 0.5 ? 0.14 : 1.0;\n"
    "  for (int j = 0; j < 8; j++) for (int i = 0; i < 12; i++) {\n"
    "    vec2 p = vec2((float(i) + 0.5) / 12.0, mix(y0, y1, (float(j) + 0.5) / 8.0));\n"
    "    vec2 q = qmap(p);\n"
    "    if (is_geo(dsample(q))) { sum += texture(ct, q * u.cc.xy).rgb; n += 1.0; }\n"
    "  }\n"
    "  if (n < 6.0) { o = prev; return; }\n"  // (too little geometry up there: keep what it was)
    "  vec3 c = sum / n;\n"
    "  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "  c = mix(vec3(l), c, 1.0 - u.mi.x) * u.mi.y;\n"
    "  o = vec4(u.top.w > 0.5 ? c : mix(prev.rgb, c, u.bot.w), 1.0);\n"
    "}\n";

// (fade) how much of the pixel at output position p is kept: its point on the table (its depth, else where its ray
// meets the floor) against the round fade, and against the edges of the floor the culling draws (nothing past them
// is drawn, so the fade has ended before them: a cut never shows)
// (fade) the treated pixel at output position p (a black pixel stays black: its depth is not read, and the void is
// most of the eye)
#define FADE_FUNCS \
    "float fade_k(vec2 p, float d, bool geo) {\n" \
    "  vec2 q = qmap(p);\n" \
    "  vec2 nd = vec2(p.x * 2.0 - 1.0, 1.0 - 2.0 * q.y);\n" \
    "  vec2 xy = vec2((nd.x + u.proj.z) / u.proj.x, (nd.y + u.proj.w) / u.proj.y);\n" \
    "  float z = (d - u.zt.z) / u.zt.w;\n" \
    "  if (u.fl.x > 0.5) z = z * 2.0 - 1.0;\n" \
    "  float pd = u.zt.y / (z + u.zt.x);\n" \
    "  vec3 E = -(u.vr[0].xyz * u.vr[0].w + u.vr[1].xyz * u.vr[1].w + u.vr[2].xyz * u.vr[2].w);\n" \
    "  vec3 dw = u.vr[0].xyz * xy.x + u.vr[1].xyz * xy.y - u.vr[2].xyz;\n" \
    "  float t = geo ? pd : (dw.y < -1e-5 ? (u.fd.y - E.y) / dw.y : -1.0);\n" \
    "  if (t <= 0.0) return 0.0;\n" \
    "  vec2 wp = E.xz + dw.xz * t;\n" \
    "  float x = clamp((length(wp - u.fd.xz) - u.fd.w) / max(u.fw.x, 1e-3), 0.0, 1.0);\n" \
    "  float k = 1.0 - x * x * x * (x * (x * 6.0 - 15.0) + 10.0);\n"  /* (smootherstep: no kink where it starts or ends) */ \
    "  if (u.dk.x > 0.5)\n" \
    "    for (int i = 0; i < 4; i++) {\n" \
    "      vec2 a = i < 2 ? (i == 0 ? u.dq[0].xy : u.dq[0].zw) : (i == 2 ? u.dq[1].xy : u.dq[1].zw);\n" \
    "      vec2 b = i == 3 ? u.dq[0].xy : (i == 0 ? u.dq[0].zw : (i == 1 ? u.dq[1].xy : u.dq[1].zw));\n" \
    "      vec2 e = b - a, f = wp - a;\n" \
    "      float y = clamp(((e.x * f.y - e.y * f.x) / max(length(e), 1e-4) - u.dk.z) / u.dk.y, 0.0, 1.0);\n" \
    "      k *= y * y * (3.0 - 2.0 * y);\n" \
    "    }\n" \
    "  return k;\n" \
    "}\n" \
    "vec3 fade_px(vec2 p, out float k) {\n" \
    "  vec2 q = qmap(p);\n" \
    "  vec3 s = texture(ct, q * u.cc.xy).rgb;\n" \
    "  k = 0.0;\n" \
    "  if (max(s.r, max(s.g, s.b)) <= 0.0) return s;\n" \
    "  float d = dsample(q);\n" \
    "  k = fade_k(p, d, is_geo(d));\n" \
    "  return s * k;\n" \
    "}\n" \
    "vec2 out_pos(vec2 sp) { return qmap(sp / (u.vd.zw * u.cc.xy)); }\n"  /* (colour pixel -> output position) */ \
    ""

// (fade, foveated) the treated colour (rgb) and fade factor (a) at the centre of each block of 2^fw.y x 2^fw.z pixels
// (one texel per block): the main pass reads it bilinearly where the foveated passes shaded such blocks
static const char *low_fs =
    PRELUDE
    FADE_FUNCS
    "void main() {\n"
    "  vec2 b = vec2(float(1 << int(u.fw.y)), float(1 << int(u.fw.z)));\n"
    "  float k;\n"
    "  vec3 c = fade_px(out_pos(gl_FragCoord.xy * b), k);\n"
    "  o = vec4(c, k);\n"
    "}\n";

static const char *main_fs =
    PRELUDE
    "float ign(vec2 p, float s) { return fract(52.9829189 * fract(dot(p + 5.588238 * s, vec2(0.06711056, 0.00583715)))); }\n"
    FADE_FUNCS
    "void main() {\n"
    // fade, no debug view: where the foveated passes shaded blocks of pixels once, the treated colour is interpolated
    // between the block centres, so neither the blocks nor the stairs they leave at a silhouette show
    "  if (u.fl.z > 3.5 && u.mi.z < 0.5) {\n"
    "    vec2 p = gl_FragCoord.xy / u.sz.xy, q = qmap(p);\n"
    "    vec2 sp = q * u.cc.xy * u.vd.zw;\n"
    "    ivec2 lr = u.va.x > 0.5 ? vrs_rate(sp) : ivec2(0);\n"
    "    float k;\n"
    "    vec3 c;\n"
    "    if (lr.x + lr.y == 0) c = fade_px(p, k);\n"
    "    else {\n"  // (the block-centre image of this rate, bilinear: mk the outer ring's, tn the middle ring's)
    "      bool mid = lr == ivec2(u.vb.zw);\n"
    "      vec2 b = vec2(float(1 << lr.x), float(1 << lr.y));\n"
    "      vec4 v = mid ? texture(tn, sp / b / u.ls.xy) : texture(mk, sp / b / u.ls.zw);\n"
    "      c = v.rgb, k = v.a;\n"
    "    }\n"
    // (dither: triangular, one 8-bit step, fixed to the pixel so it cannot crawl under head motion; only where darkened
    // and not black, so the void stays exactly black)
    "    float n2 = ign(gl_FragCoord.xy, 0.0) + ign(gl_FragCoord.yx + vec2(37.0, 11.0), 0.0) - 1.0;\n"
    "    c += n2 / 255.0 * min((1.0 - k) * 8.0, 1.0) * min(max(c.r, max(c.g, c.b)) * 32.0, 1.0);\n"
    "    { vec4 pf = u.pf;\n" SCREEN_FILTER_SRC("vec3") "  }\n"
    "    o = vec4(c, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  vec2 p = gl_FragCoord.xy / u.sz.xy;\n"
    "  int quad = 3;\n"
    "  if (abs(u.mi.z - 9.0) < 0.5) { quad = int(p.x * 2.0) + 2 * int(p.y * 2.0); p = fract(p * 2.0); }\n"  // (debug 9: 2x2 mosaic)
    "  vec2 q = qmap(p);\n"
    "  vec4 s = vec4(colour(q), 1.0);\n"
    // (fade: a black pixel stays black whatever its depth says, so its depth is not read; void pixels are most of the eye)
    "  float d = u.fl.z > 3.5 && max(s.r, max(s.g, s.b)) <= 0.0 ? 1.0 : dsample(q);\n"
    "  bool geo = is_geo(d);\n"
    // the pixel's ray in eye space (x / depth, y / depth), and its distance from the eye in game units
    "  vec2 nd = vec2(p.x * 2.0 - 1.0, 1.0 - 2.0 * q.y);\n"
    "  vec2 xy = vec2((nd.x + u.proj.z) / u.proj.x, (nd.y + u.proj.w) / u.proj.y);\n"
    "  vec3 ray = vec3(xy, -1.0);\n"
    "  float z = (d - u.zt.z) / u.zt.w;\n"
    "  if (u.fl.x > 0.5) z = z * 2.0 - 1.0;\n"
    "  float pd = u.zt.y / (z + u.zt.x);\n"       // planar distance
    "  float r = geo ? pd * length(ray) : 1e9;\n"  // distance from the eye
    // (fade) the pixel's point on the table: the geometry's own (its depth), else where its ray meets the floor (the
    // game's glows and other draws that leave the depth cleared); its distance across the floor from the table's centre
    "  float fk = 1.0;\n"
    "  if (u.fl.z > 3.5) {\n"
    "    vec3 E = -(u.vr[0].xyz * u.vr[0].w + u.vr[1].xyz * u.vr[1].w + u.vr[2].xyz * u.vr[2].w);\n"
    "    vec3 dw = u.vr[0].xyz * ray.x + u.vr[1].xyz * ray.y + u.vr[2].xyz * ray.z;\n"
    "    float t = geo ? pd : (dw.y < -1e-5 ? (u.fd.y - E.y) / dw.y : -1.0);\n"
    "    fk = 0.0;\n"
    "    if (t > 0.0) {\n"
    "      vec2 wp = E.xz + dw.xz * t;\n"
    "      float x = clamp((length(wp - u.fd.xz) - u.fd.w) / max(u.fw.x, 1e-3), 0.0, 1.0);\n"
    "      fk = 1.0 - x * x * x * (x * (x * 6.0 - 15.0) + 10.0);\n"  // (smootherstep: no kink where it starts or ends)
    "    }\n"
    "  }\n"
    "  float elev = dot(normalize(ray), u.up.xyz);\n"
    "  vec3 tc = texelFetch(tn, ivec2(0, 0), 0).rgb, bc = texelFetch(tn, ivec2(1, 0), 0).rgb;\n"
    "  vec3 skyc = mix(tc, u.sky.rgb, u.sky.w);\n"                 // (the scene's top colour, toward a sky tone)
    "  vec3 horc = mix(skyc, vec3(1.0), 0.25);\n"  // (a lighter, hazier band at the horizon)
    "  vec3 belc = mix(horc, bc, 0.2) * 0.96;\n"
    "  float g = smoothstep(-0.55, 0.0, elev), g2 = smoothstep(0.0, 0.75, elev);\n"
    "  vec3 back = mix(mix(belc, horc, g), skyc, g2);\n"
    "  float fog = smoothstep(u.fog.x, u.fog.y, r);\n"
    "  float rect = 0.0;\n"
    "  if (u.rc.x > 0.5 && geo) {\n"   // (the table's floor quadrilateral: the culling's: fade to the backdrop at its edge)
    "    vec3 v = vec3(xy * pd, -pd);\n"
    "    vec3 w = u.vr[0].xyz * (v.x - u.vr[0].w) + u.vr[1].xyz * (v.y - u.vr[1].w) + u.vr[2].xyz * (v.z - u.vr[2].w);\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      vec2 a = i < 2 ? (i == 0 ? u.rq[0].xy : u.rq[0].zw) : (i == 2 ? u.rq[1].xy : u.rq[1].zw);\n"
    "      vec2 b = i == 3 ? u.rq[0].xy : (i == 0 ? u.rq[0].zw : (i == 1 ? u.rq[1].xy : u.rq[1].zw));\n"
    "      vec2 e = b - a, f = w.xz - a;\n"
    "      float d = (e.x * f.y - e.y * f.x) / max(length(e), 1e-4);\n"
    "      float t = clamp(d / max(u.rb[i], 1e-3), 0.0, 1.0);\n"
    "      rect = max(rect, (1.0 - t * t * (3.0 - 2.0 * t)) * u.rc.w);\n"  // (full at the edge, none a band inside)
    "    }\n"
    "  }\n"
    "  float cover = u.fl.z > 3.5 ? 0.0 : texture(mk, p).r;\n"  // (how near the void is, 0..1; fade: no mask)
    "  float edge = smoothstep(0.06, 0.92, cover) * u.fog.w;\n"
    "  float fade = !geo ? 1.0 : (u.fl.z < 1.5 ? max(max(fog, edge), rect) : 0.0);\n"
    "  vec3 c = mix(s.rgb, back, fade);\n"
    "  float a = 1.0;\n"
    "  if (abs(u.fl.z - 3.0) < 0.5) { a = 1.0 - fade; c = s.rgb * a; }\n"  // (passthrough: premultiplied)
    "  float n = ign(gl_FragCoord.xy, u.fl.w) + ign(gl_FragCoord.xy + 17.0, u.fl.w + 3.0) - 1.0;\n"  // (triangular)
    "  if (u.fl.z > 3.5) {\n"  // (fade: toward black; dithered where darkened and not black already, so the void stays black)
    // (the dither is fixed to the pixel, not drawn anew each frame: it cannot crawl or shimmer under head motion)
    "    float n2 = ign(gl_FragCoord.xy, 0.0) + ign(gl_FragCoord.yx + vec2(37.0, 11.0), 0.0) - 1.0;\n"
    "    c = s.rgb * fk;\n"
    "    c += n2 / 255.0 * min((1.0 - fk) * 8.0, 1.0) * min(max(s.r, max(s.g, s.b)) * 32.0, 1.0);\n"
    "  } else\n"
    "  c += n / 255.0 * min(fade * 8.0, 1.0);\n"  // (dither only where something was blended: the rest is untouched)
    "  { vec4 pf = u.pf;\n" SCREEN_FILTER_SRC("vec3") "  }\n"  // (the present's screen filter and the rig's blink)
    "  float dm = u.mi.z;\n"
    "  if (abs(dm - 9.0) < 0.5) {\n"
    "    if (quad == 0) c = s.rgb;\n"
    "    else if (quad == 1) c = geo ? vec3(clamp(r / (4.0 * u.up.w), 0.0, 1.0), fract(d * 64.0), d) : vec3(1.0, 0.0, d);\n"
    "    else if (quad == 2) c = vec3(cover, fog, edge);\n"
    "  } else if (abs(dm - 8.0) < 0.5) {\n"
    "    if (p.x < 0.5) c = s.rgb;\n"  // (debug 8: before | after, in one frame)
    "    if (abs(p.x - 0.5) < 0.0008) c = vec3(1.0);\n"
    "    a = 1.0;\n"
    "  } else if (abs(dm - 10.0) < 0.5) {\n"  // (debug 10: the box 0.55..0.95 x 0.5..0.9 shows the untreated eye inside, the treated outside, one frame)
    "    vec2 bq = abs(p - vec2(0.75, 0.7));\n"
    "    if (bq.x < 0.2 && bq.y < 0.2) c = s.rgb;\n"
    "    if (abs(max(bq.x, bq.y) - 0.2) < 0.0008) c = vec3(1.0);\n"
    "    a = 1.0;\n"
    "  } else if (abs(dm - 11.0) < 0.5) {\n"  // (debug 11: |treated - untreated| x 8 over a dark copy of the eye: white where the pass changed a pixel)
    "    c = abs(c - s.rgb) * 8.0 + 0.1 * s.rgb;\n"
    "    a = 1.0;\n"
    "  } else if (u.mi.z > 0.5) {\n"
    "    int m = int(u.mi.z + 0.5);\n"
    "    if (m == 1) c = vec3(geo ? 0.2 + 0.8 * (1.0 - d) : 0.0, 0.0, geo ? 0.0 : 1.0);\n"
    "    else if (m == 2) c = vec3(cover);\n"
    "    else if (m == 3) c = geo ? vec3(rect, fog, edge) : vec3(1.0, 0.0, 1.0);\n"
    "    else if (m == 4) c = vec3(elev * 0.5 + 0.5, g, 0.0);\n"
    "    else if (m == 5) c = back;\n"
    "    else if (m == 6) c = vec3(fk, geo ? 1.0 : 0.0, 0.0) * (0.25 + 0.75 * dot(s.rgb, vec3(0.333)));\n"
    "    a = 1.0;\n"
    "  }\n"
    "  o = vec4(c, a);\n"
    "}\n";

// ------------------------------------------------------------------------------------------------ GPU objects
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; VkFormat fmt; uint32_t w, h; } Img;
enum { P_COV, P_BLUR, P_TINT, P_LOW, NPIPE };
static struct {
    int ok;  // -1 not tried, 0 failed, 1 ready
    VkShaderModule vs, fs[NPIPE + 1];
    VkDescriptorSetLayout sl;
    VkPipelineLayout pl;
    VkPipeline pipe[NPIPE];
    struct { VkFormat fmt; VkPipeline p; } main_pipe[4];
    Img mask[2], tint[2], low[2];  // (low: fade, foveated: the middle and outer rings' block-centre images)
    int tint_cur;
    bool tint_first;
    double t_last;
    uint64_t frame;
} g = {.ok = -1, .tint_first = true};

static bool img_make(Img *m, VkFormat fmt, uint32_t w, uint32_t h) {
    if (m->img && m->fmt == fmt && m->w == w && m->h == h) return true;
    if (m->img) vk_defer_view(m->view), vk_defer_image(m->img, m->mem), memset(m, 0, sizeof *m);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &m->img, &m->mem, NULL) != VK_SUCCESS) { m->img = VK_NULL_HANDLE; return false; }
    vk_image_init_layout(vk_cmd(), m->img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &m->view) != VK_SUCCESS) {
        vk_defer_image(m->img, m->mem);
        memset(m, 0, sizeof *m);
        return false;
    }
    m->fmt = fmt, m->w = w, m->h = h;
    return true;
}

static VkPipeline make_pipe(VkShaderModule fs, VkFormat fmt) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g.vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = g_vk.vids ? 3u : 2u, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = g.pl};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) {
        LOG("[edge] pipeline failed\n");
        return VK_NULL_HANDLE;
    }
    return p;
}

static VkShaderModule compile(const char *src, int stage, const char *name) {
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) LOG("[edge] shader %s failed to compile\n", name);
    return m;
}

static bool init(void) {
    if (g.ok >= 0) return g.ok;
    g.ok = 0;
    g.vs = compile(vs_src, STAGE_VERTEX, "edge.vert");
    g.fs[P_COV] = compile(cov_fs, STAGE_FRAGMENT, "edge_cov.frag");
    g.fs[P_BLUR] = compile(blur_fs, STAGE_FRAGMENT, "edge_blur.frag");
    g.fs[P_TINT] = compile(tint_fs, STAGE_FRAGMENT, "edge_tint.frag");
    g.fs[P_LOW] = compile(low_fs, STAGE_FRAGMENT, "edge_low.frag");
    g.fs[NPIPE] = compile(main_fs, STAGE_FRAGMENT, "edge_main.frag");
    if (!g.vs || !g.fs[P_COV] || !g.fs[P_BLUR] || !g.fs[P_TINT] || !g.fs[P_LOW] || !g.fs[NPIPE]) return false;
    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t i = 0; i < 5; i++)
        b[i] = (VkDescriptorSetLayoutBinding){i, i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                              VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &g.sl));
    VkPipelineLayoutCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g.sl};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pci, NULL, &g.pl));
    g.pipe[P_COV] = make_pipe(g.fs[P_COV], VK_FORMAT_R16_SFLOAT);
    g.pipe[P_BLUR] = make_pipe(g.fs[P_BLUR], VK_FORMAT_R16_SFLOAT);
    g.pipe[P_TINT] = make_pipe(g.fs[P_TINT], VK_FORMAT_R16G16B16A16_SFLOAT);
    g.pipe[P_LOW] = make_pipe(g.fs[P_LOW], VK_FORMAT_R16G16B16A16_SFLOAT);
    if (!g.pipe[P_COV] || !g.pipe[P_BLUR] || !g.pipe[P_TINT] || !g.pipe[P_LOW]) return false;
    g.ok = 1;
    return true;
}

static VkPipeline main_pipe(VkFormat fmt) {
    for (int i = 0; i < 4; i++) {
        if (g.main_pipe[i].p && g.main_pipe[i].fmt == fmt) return g.main_pipe[i].p;
        if (!g.main_pipe[i].p) {
            g.main_pipe[i].p = make_pipe(g.fs[NPIPE], fmt), g.main_pipe[i].fmt = fmt;
            return g.main_pipe[i].p;
        }
    }
    return VK_NULL_HANDLE;
}

static void begin(VkCommandBuffer cb, VkImageView view, VkImageLayout layout, uint32_t w, uint32_t h) {
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = layout,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    if (g_vk.vids) {
        VkVertexInputBindingDescription2EXT vb = {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
        VkVertexInputAttributeDescription2EXT va = {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
        rec_CmdSetVertexInputEXT(cb, 0, &vb, 0, &va);
    }
}

typedef struct { float proj[4], zt[4], fl[4], up[4], cc[4], sz[4], fog[4], top[4], bot[4], mi[4], vr[3][4], rq[2][4], rc[4], rb[4], sky[4], fd[4], fw[4], pf[4], va[4], vc[4], vb[4], vd[4], dq[2][4], dk[4], ls[4]; } UBlock;

static VkDescriptorSet make_set(const UBlock *ub, VkImageView depth, VkImageView color, VkImageView mask, VkImageView tint) {
    VkSpan s = vk_transient_copy(ub, sizeof *ub, 256);
    VkDescriptorSet set = vk_alloc_set(g.sl);
    VkSampler nearest = vk_default_sampler(false), lin = vk_default_sampler(true);
    VkDescriptorBufferInfo bi = {s.buf, s.off, sizeof *ub};
    VkDescriptorImageInfo im[4] = {{nearest, depth, VK_IMAGE_LAYOUT_GENERAL}, {lin, color, VK_IMAGE_LAYOUT_GENERAL},
                                   {lin, mask, VK_IMAGE_LAYOUT_GENERAL}, {lin, tint, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[5];
    for (uint32_t i = 0; i < 5; i++) {
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1};
        if (i == 0) wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, wr[i].pBufferInfo = &bi;
        else wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[i].pImageInfo = &im[i - 1];
    }
    rec_UpdateDescriptorSets(5, wr);
    return set;
}

static void fullscreen(VkCommandBuffer cb, VkPipeline p, VkDescriptorSet set) {
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl, 0, 1, &set, 0, NULL);
    rec_CmdDraw(cb, 3, 1, 0, 0);
}

// ------------------------------------------------------------------------------------------------ GPU time
// The cost of the passes, per eye: a timestamp pair around them, resolved a few frames later, logged every 2 s.
enum { TSQ = 64 };
static VkQueryPool ts_pool;
static struct { uint64_t seq; uint32_t q; int eye; } ts_ring[TSQ / 2];
static uint32_t ts_head, ts_tail;
static double ts_sum[2];
static int ts_n[2];
static double ts_t0;

static uint32_t ts_begin(VkCommandBuffer cb) {
    if (!ts_pool) {
        if (!g_vk.props.limits.timestampComputeAndGraphics) return ~0u;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = TSQ};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &ts_pool) != VK_SUCCESS) { ts_pool = VK_NULL_HANDLE; return ~0u; }
    }
    if (ts_head - ts_tail >= TSQ / 2) return ~0u;
    uint32_t q = (ts_head % (TSQ / 2)) * 2;
    rec_CmdResetQueryPool(cb, ts_pool, q, 2);
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ts_pool, q);
    return q;
}
static void ts_end(VkCommandBuffer cb, uint32_t q, int eye) {
    if (q == ~0u) return;
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ts_pool, q + 1);
    ts_ring[ts_head % (TSQ / 2)].seq = vk_pending_seq(), ts_ring[ts_head % (TSQ / 2)].q = q, ts_ring[ts_head % (TSQ / 2)].eye = eye;
    ts_head++;
}
static void ts_resolve(void) {
    uint64_t done = vk_completed_seq();
    while (ts_tail != ts_head) {
        __typeof__(ts_ring[0]) *t = &ts_ring[ts_tail % (TSQ / 2)];
        if (t->seq > done) break;
        uint64_t v[2];
        if (vkGetQueryPoolResults(g_vk.dev, ts_pool, t->q, 2, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && v[1] >= v[0]) {
            ts_sum[t->eye] += (double)(v[1] - v[0]) * g_vk.props.limits.timestampPeriod / 1e6;
            ts_n[t->eye]++;
        }
        ts_tail++;
    }
    double now = plat_now_ns() / 1e9;
    if (ts_t0 == 0) ts_t0 = now;
    if (now - ts_t0 >= 2.0 && ts_n[0] + ts_n[1]) {
        LOG("[edge] GPU per eye: %.3f ms (eye 0), %.3f ms (eye 1) over %d / %d frames\n", ts_n[0] ? ts_sum[0] / ts_n[0] : -1.0,
            ts_n[1] ? ts_sum[1] / ts_n[1] : -1.0, ts_n[0], ts_n[1]);
        ts_sum[0] = ts_sum[1] = 0, ts_n[0] = ts_n[1] = 0, ts_t0 = now;
    }
}

// ------------------------------------------------------------------------------------------------ the eye
// What the eye post hook noted for each eye: the latest new output's images and cameras.
static struct {
    bool have;
    TexBackend *color, *depth;
    uint32_t layer, depth_layer;  // (LARECOMP_MV: the eye's layer of each)
    float ax, ay, V[16], P[16], zn, zf;
    bool vrs;       // the passes that made it were foveated (fov_shade): their coarse blocks are smoothed
    FovShade sh;
} noted[2];

void stereo_eye_post(const StereoEyePost *e) {
    if (e->eye < 0 || e->eye > 1) return;
    if (!cfg.init) cfg_init();
    // (noted in every mode, black too: a switch from the VR tab then finds the current images, never stale ones)
    __typeof__(noted[0]) *n = &noted[e->eye];
    n->have = e->depth && e->have_matrices;
    n->color = e->color, n->depth = e->depth, n->layer = e->layer, n->depth_layer = e->depth_layer, n->ax = e->ax, n->ay = e->ay, n->zn = e->zn, n->zf = e->zf;
    memcpy(n->V, e->V, 64), memcpy(n->P, e->P, 64);
    n->vrs = fov_shade(&n->sh) && n->sh.w == e->width && n->sh.h == e->height;
    {
        static bool said_vrs;
        FovShade sh;
        if (!said_vrs && fov_shade(&sh) && !n->vrs)
            LOG("[edge] foveated passes %ux%u, eye output %ux%u: their blocks are not smoothed\n", sh.w, sh.h, e->width, e->height), said_vrs = true;
    }
    static bool said;
    if (!said && edge_mode() != EDGE_BLACK && (!e->depth || !e->have_matrices)) LOG("[edge] eye %d: no %s noted: the edge pass waits for it\n", e->eye, e->depth ? "matrices" : "depth"), said = true;
}

typedef struct {
    int eye;
    VkImageView color, depth;
    float color_crop[2], depth_crop[2], V[16], P[16], zn, zf, pivot_dist;
    bool origin_lower_left, depth_minus1;
} EdgeEye;

extern uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void);

// (fade, overworld) The culling draws only the floor inside `drawn` (vr_cull_drawn: a trapezoid from the game camera,
// widening away from it). With the circle about the table's centre at its old size (black past 25 units) the trapezoid's
// slanted sides cut the table first, 10-14 units out: a straight-sided wedge of trees with black past it ("a triangle off
// the trees"). So the circle is fitted to it: its centre moves (at most 8 units, preferring the table's centre) to where
// it can be largest, and it is black by the sides and the far edge. The near edge (the shortest, under the game camera,
// often only 2 units past the stock screen's) may cut up to FADE_NEAR_ALLOW into it: there the shader also fades to the
// trapezoid's edges (FADE_CUT_BAND), so that edge is soft too. Width 40% of the black radius (3 units at least); black
// at FADE_MIN_BLACK at the least and never past radius + width. Eye 0 fits, eye 1 uses the same; filtered over
// FADE_FIT_TAU, so the edge glides when the culling changes with the head.
#define FADE_CUT_BAND 2.0f
#define FADE_CUT_MARGIN 0.25f
#define FADE_FIT_MARGIN 0.5f
#define FADE_NEAR_ALLOW 3.0f
#define FADE_MIN_BLACK 10.0f
#define FADE_FIT_TAU 0.4f
static float drawn_dist(const float q[4][2], float x, float z, int near, float allow) {  // inside distance (negative: outside)
    float m = 1e9f;
    for (int i = 0; i < 4; i++) {
        float ex = q[(i + 1) % 4][0] - q[i][0], ez = q[(i + 1) % 4][1] - q[i][1], el = sqrtf(ex * ex + ez * ez) + 1e-6f;
        m = fminf(m, (ex * (z - q[i][1]) - ez * (x - q[i][0])) / el + (i == near ? allow : 0.0f));
    }
    return m;
}
static bool fade_fit(int eye, const float q[4][2], float c[3], float *r, float *w, bool *cut) {
    static struct { bool ok; float off[2], rb, anchor[2]; double t; } f;
    int near = 0;
    float ln = 1e9f;
    for (int i = 0; i < 4; i++) {
        float l = hypotf(q[(i + 1) % 4][0] - q[i][0], q[(i + 1) % 4][1] - q[i][1]);
        if (l < ln) ln = l, near = i;
    }
    if (eye == 0) {
        float best = -1e9f, bo[2] = {0, 0}, bin = 0;
        for (int j = -16; j <= 16; j++)
            for (int i = -16; i <= 16; i++) {
                float ox = i * 0.5f, oz = j * 0.5f, m = drawn_dist(q, c[0] + ox, c[2] + oz, near, FADE_NEAR_ALLOW);
                float sc = m - 0.25f * sqrtf(ox * ox + oz * oz);
                if (sc > best) best = sc, bo[0] = ox, bo[1] = oz, bin = m;
            }
        float rb = fmaxf(fminf(cfg.radius + cfg.width, bin - FADE_FIT_MARGIN), FADE_MIN_BLACK);
        double now = plat_now_ns() / 1e9;
        float jump = hypotf(c[0] - f.anchor[0], c[2] - f.anchor[1]);
        if (!f.ok || now - f.t > 0.5 || jump > 3.0f) f.off[0] = bo[0], f.off[1] = bo[1], f.rb = rb;  // (a new place: no glide)
        else {
            float a = 1.0f - expf(-(float)(now - f.t) / FADE_FIT_TAU);
            f.off[0] += (bo[0] - f.off[0]) * a, f.off[1] += (bo[1] - f.off[1]) * a, f.rb += (rb - f.rb) * a;
        }
        f.ok = true, f.t = now, f.anchor[0] = c[0], f.anchor[1] = c[2];
    }
    if (!f.ok) return false;
    c[0] += f.off[0], c[2] += f.off[1];
    *w = fminf(fmaxf(f.rb * 0.4f, 3.0f), cfg.width);
    *r = fmaxf(f.rb - *w, 0.5f);
    *cut = drawn_dist(q, c[0], c[2], -1, 0) < f.rb + 0.5f * FADE_FIT_MARGIN;  // (the circle reaches past the drawn floor: fade to its edges too)
    return true;
}

static bool g_tint_restart;
bool edge_eye(VkCommandBuffer cb, int eye, VkImageView target, VkFormat fmt, uint32_t w, uint32_t h) {
    EdgeMode mode = edge_mode();
    if (g_mode_changed) g_mode_changed = false, g_tint_restart = true;  // (a switch: the backdrop colours start from the scene again)
    if (mode == EDGE_BLACK || eye < 0 || eye > 1 || !noted[eye].have) return false;
    // outdoors only: an interior or a dungeon (or the title) keeps its dark void and the game's own glow, drawn by the present as before
    extern int game_level_overworld(void);  // (widescreen.c) 1 the overworld, 0 another level, -1 not in play
    int ow = game_level_overworld();
    if (mode == EDGE_FADE ? ow < 0 : !cfg.indoors && ow != 1) return false;  // (fade: everywhere in play)
    float fade_c[3] = {0}, fade_r = cfg.radius, fade_w = cfg.width;  // (fade) the table's centre, the clear radius, the width
    float drawn[4][2];
    bool fitted = false, cut = false;
    // (scene: the overworld drawn further out, the VR tab's Surroundings) the fade ends at the drawn box's nearer side
    // instead of radius + width out, unless LARECOMP_VR_EDGE_RADIUS says otherwise; with a reach the culling frustum is
    // opened wide and the box limits what is drawn, so the fit to the frustum's floor is left out
    static int r_given = -1;
    if (r_given < 0) r_given = getenv("LARECOMP_VR_EDGE_RADIUS") != NULL;
    float reach = vr_cull_scene_reach();
    if (!r_given) fade_r = fmaxf(fade_r, reach - cfg.width);
    if (mode == EDGE_FADE) {
        float lo[3], hi[3];
        bool room, zone;
        if (!stereo_table(fade_c, &room, &zone, lo, hi)) return false;
        // (a room or a side view, not the overworld, whose camera zones are areas of 240 x 198: all of it stays clear;
        // the fade only softens what lies past it)
        if (room && zone && ow == 0 && hi[0] - lo[0] < 80 && hi[2] - lo[2] < 80)
            for (int i = 0; i < 4; i++) {
                float dx = (i & 1 ? hi[0] : lo[0]) - fade_c[0], dz = (i & 2 ? hi[2] : lo[2]) - fade_c[2];
                fade_r = fmaxf(fade_r, sqrtf(dx * dx + dz * dz) + cfg.room_margin);
            }
        if (ow == 1 && cfg.fit && !(reach > 0) && vr_cull_drawn(drawn)) fitted = fade_fit(eye, drawn, fade_c, &fade_r, &fade_w, &cut);
    }
    EdgeEye ein = {.eye = eye};
    const EdgeEye *in = &ein;
    {
        __typeof__(noted[0]) *n = &noted[eye];
        if (!n->color->image || !n->depth->image) return false;
        static const VkComponentMapping id = {0};
        VkFormat cf = n->color->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
                    : n->color->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : n->color->format;
        ein.color = vk_image_view(n->color, VK_IMAGE_VIEW_TYPE_2D, cf, 0, 1, n->layer, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
        ein.depth = vk_image_view(n->depth, VK_IMAGE_VIEW_TYPE_2D, n->depth->format, 0, 1, n->depth_layer, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT);
        ein.color_crop[0] = ein.depth_crop[0] = n->ax, ein.color_crop[1] = ein.depth_crop[1] = n->ay;
        memcpy(ein.V, n->V, 64), memcpy(ein.P, n->P, 64);
        ein.zn = n->zn, ein.zf = n->zf, ein.pivot_dist = 0;
        ein.origin_lower_left = renderer_origin_lower_left();
        ein.depth_minus1 = nvn_depth_mode() == 0;
    }
    if (!ein.color || !ein.depth) return false;
    if (!init()) return false;
    VkPipeline mp = main_pipe(fmt);
    uint32_t mw = (w + 7) / 8, mh = (h + 7) / 8;
    if (!mp || !img_make(&g.mask[0], VK_FORMAT_R16_SFLOAT, mw, mh) || !img_make(&g.mask[1], VK_FORMAT_R16_SFLOAT, mw, mh) ||
        !img_make(&g.tint[0], VK_FORMAT_R16G16B16A16_SFLOAT, 2, 1) || !img_make(&g.tint[1], VK_FORMAT_R16G16B16A16_SFLOAT, 2, 1)) {
        static bool said;
        if (!said) LOG("[edge] passes unavailable: the eye is drawn without them\n"), said = true;
        return false;
    }
    if (g_tint_restart) g.tint_first = true, g_tint_restart = false;
    ts_resolve();
    uint32_t tq = ts_begin(cb);

    UBlock u;
    memset(&u, 0, sizeof u);
    const float *P = in->P, *V = in->V;
    u.proj[0] = P[0], u.proj[1] = P[5], u.proj[2] = P[2], u.proj[3] = P[6];
    u.zt[0] = P[10], u.zt[1] = P[11], u.zt[2] = in->zn, u.zt[3] = in->zf - in->zn;
    u.fl[0] = in->depth_minus1 ? 1.0f : 0.0f, u.fl[1] = in->origin_lower_left ? 1.0f : 0.0f;
    u.fl[2] = mode == EDGE_FOG ? 1.0f : mode == EDGE_GRADIENT ? 2.0f : mode == EDGE_PASSTHROUGH ? 3.0f : 4.0f;
    u.fl[3] = (float)(g.frame++ % 64);
    u.up[0] = V[1], u.up[1] = V[5], u.up[2] = V[9];  // the world's up in eye space (the view's second column)
    float pd = in->pivot_dist > 0 ? in->pivot_dist : cfg.pivot_default;
    u.up[3] = pd;
    u.cc[0] = in->color_crop[0], u.cc[1] = in->color_crop[1], u.cc[2] = in->depth_crop[0], u.cc[3] = in->depth_crop[1];
    u.sz[0] = (float)w, u.sz[1] = (float)h, u.sz[2] = (float)mw, u.sz[3] = (float)mh;
    u.fog[0] = cfg.fog_start * pd, u.fog[1] = cfg.fog_end * pd;
    u.fog[2] = cfg.soft * (float)h / 8.0f;  // the fade's radius in mask texels
    u.fog[3] = cfg.strength;
    memcpy(u.top, cfg.top, sizeof cfg.top), u.top[3] = g.tint_first ? 1.0f : 0.0f;
    memcpy(u.bot, cfg.bot, sizeof cfg.bot);
    double now = plat_now_ns() / 1e9, dt = g.t_last > 0 ? now - g.t_last : 0.0;
    if (in->eye == 0) {
        g.t_last = now;
        u.bot[3] = 1.0f - expf(-(float)fminf((float)dt, 0.25f) / cfg.tau);
    }
    for (int r = 0; r < 3; r++) u.vr[r][0] = V[r * 4], u.vr[r][1] = V[r * 4 + 1], u.vr[r][2] = V[r * 4 + 2], u.vr[r][3] = V[r * 4 + 3];
    {
        float quad[4][2], fy, hlo, hhi;
        if (cfg.rect_strength > 0 && vr_cull_rect(quad, &fy, &hlo, &hhi)) {
            memcpy(u.rq, quad, sizeof quad);
            u.rc[0] = 1.0f, u.rc[1] = fy, u.rc[3] = cfg.rect_strength;
            // the eye's world position (-R^T t) and its distance to the quadrilateral's middle: the fog scales with it
            float E[3];
            for (int k = 0; k < 3; k++) E[k] = -(V[k] * V[3] + V[4 + k] * V[7] + V[8 + k] * V[11]);
            float cx = 0, cz = 0;
            for (int i = 0; i < 4; i++) cx += quad[i][0] * 0.25f, cz += quad[i][1] * 0.25f;
            float shrink[4];
            vr_cull_rect_shrink(shrink);
            for (int i = 0; i < 4; i++) {  // each edge's band: the part between the stock screen's edge and the culling's
                float ax = quad[i][0], az = quad[i][1], bx = quad[(i + 1) % 4][0], bz = quad[(i + 1) % 4][1];
                float ex = bx - ax, ez = bz - az, el = sqrtf(ex * ex + ez * ez) + 1e-6f;
                float dc = (ex * (cz - az) - ez * (cx - ax)) / el;  // the middle's distance to the edge
                u.rb[i] = fmaxf(shrink[i] * dc * cfg.rect_soft, 0.4f);
            }
            pd = sqrtf((E[0] - cx) * (E[0] - cx) + (E[1] - fy) * (E[1] - fy) + (E[2] - cz) * (E[2] - cz));
            u.up[3] = pd, u.fog[0] = cfg.fog_start * pd, u.fog[1] = cfg.fog_end * pd;
        }
    }
    memcpy(u.sky, cfg.sky, sizeof cfg.sky), u.sky[3] = cfg.skymix;
    memcpy(u.fd, fade_c, sizeof fade_c), u.fd[3] = fade_r, u.fw[0] = fade_w;
    if (fitted && cut) {
        memcpy(u.dq, drawn, sizeof drawn);
        u.dk[0] = 1, u.dk[1] = FADE_CUT_BAND, u.dk[2] = FADE_CUT_MARGIN;
    }
    screen_filter_params(u.pf);
    if (noted[eye].vrs && !(cfg.smooth_off)) {  // (the foveated passes' shading: edge.c colour())
        const FovShade *sh = &noted[eye].sh;
        u.va[0] = 1, u.va[1] = (float)sh->nfoci, u.va[2] = sh->r0, u.va[3] = sh->r1;
        u.vc[0] = sh->c[0][0], u.vc[1] = sh->c[0][1], u.vc[2] = sh->c[1][0], u.vc[3] = sh->c[1][1];
        u.vb[0] = sh->fw, u.vb[1] = sh->fh, u.vb[2] = (float)sh->mid[0], u.vb[3] = (float)sh->mid[1];
        u.vd[0] = (float)sh->out[0], u.vd[1] = (float)sh->out[1], u.vd[2] = (float)sh->w, u.vd[3] = (float)sh->h;
    }  // (the present's screen filter and the rig's blink, as the present draw would apply them)
    {
        static float said_r = -1;
        static double said_t;
        double tn = plat_now_ns() / 1e9;
        if (mode == EDGE_FADE && fabsf(fade_r - said_r) > 0.5f && tn - said_t > 2.0 && (said_t = tn))
            LOG("[edge] fade: centre (%.1f %.1f %.1f), clear to %.1f units, black past %.1f%s\n", fade_c[0], fade_c[1], fade_c[2], fade_r,
                fade_r + fade_w, fitted ? " (fitted inside the floor the culling draws)" : ""), said_r = fade_r;
    }
    u.mi[0] = cfg.desat, u.mi[1] = cfg.bright, u.mi[2] = (float)cfg.debug, u.mi[3] = cfg.fixed_colours ? 1.0f : 0.0f;

    // the backdrop colours: eye 0 makes this frame's from the last (the other image); eye 1 reads them
    int prev = g.tint_cur, cur = in->eye == 0 ? 1 - g.tint_cur : g.tint_cur;
    vk_full_barrier(cb);  // (the scene and the eye output)
    static int bench = -1;  // (LARECOMP_VR_EDGE_BENCH: 1 no backdrop colours, 2 no mask, 4 no main pass: timing only)
    if (bench < 0) bench = getenv("LARECOMP_VR_EDGE_BENCH") ? atoi(getenv("LARECOMP_VR_EDGE_BENCH")) : 0;
    bool fade = mode == EDGE_FADE;  // (fade: the main pass alone, no backdrop colours or mask)
    if (in->eye == 0 && !(bench & 1) && !fade) {
        VkDescriptorSet set = make_set(&u, in->depth, in->color, g.mask[0].view, g.tint[prev].view);
        begin(cb, g.tint[cur].view, VK_IMAGE_LAYOUT_GENERAL, 2, 1);
        fullscreen(cb, g.pipe[P_TINT], set);
        rec_CmdEndRendering(cb);
        g.tint_cur = cur, g.tint_first = false;
        vk_full_barrier(cb);
    }
    // the coverage of geometry, blurred
    float fl3 = u.fl[3];
    VkDescriptorSet set;
    if (!(bench & 2) && !fade) {
    set = make_set(&u, in->depth, in->color, g.mask[1].view, g.tint[cur].view);
    begin(cb, g.mask[0].view, VK_IMAGE_LAYOUT_GENERAL, mw, mh);
    fullscreen(cb, g.pipe[P_COV], set);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    u.fl[3] = 0;
    set = make_set(&u, in->depth, in->color, g.mask[0].view, g.tint[cur].view);
    begin(cb, g.mask[1].view, VK_IMAGE_LAYOUT_GENERAL, mw, mh);
    fullscreen(cb, g.pipe[P_BLUR], set);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    u.fl[3] = 1;
    set = make_set(&u, in->depth, in->color, g.mask[1].view, g.tint[cur].view);
    begin(cb, g.mask[0].view, VK_IMAGE_LAYOUT_GENERAL, mw, mh);
    fullscreen(cb, g.pipe[P_BLUR], set);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    }
    // (fade, foveated) the block-centre images of the middle and outer rings
    VkImageView mk_view = g.mask[0].view, tn_view = g.tint[cur].view;
    if (fade && u.va[0] > 0) {
        const FovShade *sh = &noted[eye].sh;
        int rl[2][2] = {{sh->mid[0], sh->mid[1]}, {sh->out[0], sh->out[1]}};
        bool ok = true;
        for (int i = 0; i < 2 && ok; i++) {
            uint32_t lw = (sh->w + (1u << rl[i][0]) - 1) >> rl[i][0], lh = (sh->h + (1u << rl[i][1]) - 1) >> rl[i][1];
            ok = img_make(&g.low[i], VK_FORMAT_R16G16B16A16_SFLOAT, lw, lh);
            u.ls[i * 2] = (float)lw, u.ls[i * 2 + 1] = (float)lh;
        }
        if (ok) {
            for (int i = 0; i < 2; i++) {
                UBlock ul = u;
                ul.fw[1] = (float)rl[i][0], ul.fw[2] = (float)rl[i][1];
                VkDescriptorSet ls = make_set(&ul, in->depth, in->color, g.mask[0].view, g.tint[cur].view);
                begin(cb, g.low[i].view, VK_IMAGE_LAYOUT_GENERAL, g.low[i].w, g.low[i].h);
                if (i == 0 && sh->r1 > 0) {  // (the middle ring's image is only read inside the ring: its box about the foci)
                    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, rx = sh->r1 * 0.5f * sh->fw + 8, ry = sh->r1 * 0.5f * sh->fh + 8;
                    for (int f = 0; f < sh->nfoci; f++)
                        x0 = fminf(x0, sh->c[f][0] - rx), x1 = fmaxf(x1, sh->c[f][0] + rx), y0 = fminf(y0, sh->c[f][1] - ry), y1 = fmaxf(y1, sh->c[f][1] + ry);
                    int bx0 = (int)fmaxf(floorf(x0 / (float)(1 << rl[0][0])) - 1, 0), by0 = (int)fmaxf(floorf(y0 / (float)(1 << rl[0][1])) - 1, 0);
                    int bx1 = (int)fminf(ceilf(x1 / (float)(1 << rl[0][0])) + 1, (float)g.low[0].w), by1 = (int)fminf(ceilf(y1 / (float)(1 << rl[0][1])) + 1, (float)g.low[0].h);
                    VkRect2D sc = {{bx0, by0}, {(uint32_t)(bx1 > bx0 ? bx1 - bx0 : 0), (uint32_t)(by1 > by0 ? by1 - by0 : 0)}};
                    rec_CmdSetScissor(cb, 0, 1, &sc);
                }
                fullscreen(cb, g.pipe[P_LOW], ls);
                rec_CmdEndRendering(cb);
            }
            vk_full_barrier(cb);
            tn_view = g.low[0].view, mk_view = g.low[1].view;
        } else u.va[0] = 0;  // (no images: the blocks stay)
    }
    // the eye image
    u.fl[3] = fl3;
    set = make_set(&u, in->depth, in->color, mk_view, tn_view);
    begin(cb, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, w, h);
    if (!(bench & 4)) fullscreen(cb, mp, set);
    rec_CmdEndRendering(cb);
    ts_end(cb, tq, in->eye);
    return true;
}
