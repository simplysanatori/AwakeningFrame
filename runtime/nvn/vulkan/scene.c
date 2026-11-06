// The Surroundings row's Backdrop (docs/af/SCENE.md): the world around the table drawn from a still picture.
//
// Drawing the rooms past the table every frame costs the game thread about as much as the GPU (the game emits every
// object its frustum lets through), which is what holds 60 FPS back. Backdrop draws them only now and then: on a capture
// it asks vr_cull for the wide culling (as Full) for a few frames, then copies eye 0's finished output and its scene depth
// with that frame's camera. Every frame after, both eyes get that picture back as a mesh: one vertex per grid point of
// the capture, moved to where its depth puts it in the world and drawn with the eye's current camera, so the head can
// move and the table can follow Link. It only fills pixels the scene left empty (scene depth still cleared): whatever
// the game draws live always wins. Cells whose corners lie at very different depths are left out (no rubber sheets
// between a tree and the ground behind it).
// A capture is taken when there is none, when the table's centre moved (a new room comes into view), when the head
// turned away from the captured view, after LARECOMP_VR_BACKDROP_REFRESH seconds, and not more often than every
// LARECOMP_VR_BACKDROP_GAP seconds. One exec.c call (vk_stereo_eyes, before the eye post hook).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../platform.h"
#include "../../rt.h"
#include "../vr_cull.h"
#include "record.h"
#include "stereo.h"
#include "vk.h"

// ------------------------------------------------------------------------------------------------ settings
static struct {
    bool init;
    int grid;          // LARECOMP_VR_BACKDROP_GRID: grid cells across the capture (each way)
    float move;        // LARECOMP_VR_BACKDROP_MOVE: the table's centre moved this far (units) since the capture
    float turn;        // LARECOMP_VR_BACKDROP_TURN: the head turned this far (degrees) from the captured view
    float refresh;     // LARECOMP_VR_BACKDROP_REFRESH: seconds before a capture is taken again anyway (0: never)
    float gap;         // LARECOMP_VR_BACKDROP_GAP: seconds between captures at least
    int delay;         // LARECOMP_VR_BACKDROP_DELAY: new eye outputs between asking for the wide culling and the capture
    float settle;      // LARECOMP_VR_BACKDROP_SETTLE: seconds after a level change, a warp or a blink before a capture
    float jump;        // a cell whose corners' distances from the capture's eye differ more than this share is left out
    bool log;
    int edge;          // LARECOMP_VR_SCENE_EDGE: 1 (default) the void past the drawn world takes the colour next to it, fading out
    float edge_reach;  // LARECOMP_VR_SCENE_EDGE_REACH: how far the colour reaches, share of the eye's height (0.18)
} cfg;

static float envf(const char *n, float d) { const char *e = getenv(n); return e && *e ? (float)atof(e) : d; }
static void cfg_init(void) {
    cfg.init = true;
    cfg.grid = (int)envf("LARECOMP_VR_BACKDROP_GRID", 128);
    if (cfg.grid < 16) cfg.grid = 16;
    if (cfg.grid > 1024) cfg.grid = 1024;
    cfg.move = envf("LARECOMP_VR_BACKDROP_MOVE", 6.0f);
    cfg.turn = envf("LARECOMP_VR_BACKDROP_TURN", 25.0f);
    cfg.refresh = envf("LARECOMP_VR_BACKDROP_REFRESH", 0.0f);
    cfg.gap = envf("LARECOMP_VR_BACKDROP_GAP", 1.0f);
    cfg.delay = (int)envf("LARECOMP_VR_BACKDROP_DELAY", 8);
    cfg.settle = envf("LARECOMP_VR_BACKDROP_SETTLE", 2.5f);
    cfg.jump = envf("LARECOMP_VR_BACKDROP_JUMP", 0.08f);
    cfg.log = getenv("LARECOMP_VR_BACKDROP_LOG") != NULL;
    cfg.edge = (int)envf("LARECOMP_VR_SCENE_EDGE", 1);
    cfg.edge_reach = envf("LARECOMP_VR_SCENE_EDGE_REACH", 0.18f);
}

// ------------------------------------------------------------------------------------------------ shaders
#define UBLOCK                                                                                                         \
    "layout(std140, set = 0, binding = 0) uniform U {\n"                                                               \
    "  layout(row_major) mat4 inv;  // the capture: clip (game convention) -> world\n"                                 \
    "  layout(row_major) mat4 cur;  // this eye: world -> clip (game convention)\n"                                    \
    "  vec4 grid;  // cells x, cells y, the capture's filled part x, y\n"                                              \
    "  vec4 cz;    // the capture's depth range: zn, zf - zn, depth in -1..1, the cell jump share\n"                    \
    "  vec4 ce;    // the capture's eye (world)\n"                                                                     \
    "  vec4 sz;    // this eye's output size w h\n"                                                                    \
    "} u;\n"

static const char *vs_src =
    "#version 450\n" UBLOCK
    "layout(set = 0, binding = 1) uniform sampler2D cd;\n"  // the capture's depth (nearest)
    "layout(location = 0) out vec2 v_uv;\n"
    "vec3 world(vec2 g) {\n"  // a grid point (0..1 over the capture's filled part) -> world; w < 0: no geometry
    "  float d = textureLod(cd, g * u.grid.zw, 0.0).r;\n"
    "  if (d >= 0.9999) return vec3(1e30);\n"
    "  float z = (d - u.cz.x) / u.cz.y;\n"
    "  if (u.cz.z > 0.5) z = z * 2.0 - 1.0;\n"
    "  vec4 w = u.inv * vec4(g.x * 2.0 - 1.0, 1.0 - 2.0 * g.y, z, 1.0);\n"
    "  return w.xyz / w.w;\n"
    "}\n"
    "void main() {\n"
    "  const vec2 k[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));\n"
    "  int cell = gl_VertexIndex / 6, nx = int(u.grid.x);\n"
    "  vec2 c0 = vec2(float(cell % nx), float(cell / nx));\n"
    "  vec2 g = (c0 + k[gl_VertexIndex % 6]) / u.grid.xy;\n"
    // the cell's four corners: any without geometry, or far apart in distance from the capture's eye: leave the cell out
    "  float lo = 1e30, hi = 0.0;\n"
    "  vec3 me = vec3(0.0);\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    vec2 o = vec2(float(i & 1), float(i >> 1));\n"
    "    vec3 p = world((c0 + o) / u.grid.xy);\n"
    "    float r = p.x > 1e29 ? 1e30 : length(p - u.ce.xyz);\n"
    "    lo = min(lo, r), hi = max(hi, r);\n"
    "    if (o == k[gl_VertexIndex % 6]) me = p;\n"
    "  }\n"
    "  v_uv = g * u.grid.zw;\n"
    "  if (hi > 1e29 || hi > lo * (1.0 + u.cz.w)) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }\n"
    "  vec4 c = u.cur * vec4(me, 1.0);\n"
    "  gl_Position = vec4(c.x, -c.y, (c.z + c.w) * 0.5, c.w);\n"  // (into the output's texture space: row 0 is the top, NDC y +1)
    "}\n";

static const char *fs_src =
    "#version 450\n" UBLOCK
    "layout(set = 0, binding = 2) uniform sampler2D cc;\n"  // the capture's colour
    "layout(set = 0, binding = 3) uniform sampler2D sd;\n"  // this eye's scene depth (nearest)
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  if (texture(sd, gl_FragCoord.xy / u.sz.xy).r < 0.9999) discard;\n"  // (the live scene has something here: it wins)
    "  o = vec4(texture(cc, v_uv).rgb, 1.0);\n"
    "}\n";

// The edge fill (the void past the drawn world): seed = each texel of an eighth-size image is an 8x8 block of the eye: the
// average colour of every other pixel each way with something drawn (the live scene's depth, or the backdrop's own) times their share, and
// that share (premultiplied colour); its mips (blits) average that out; the fill reads, for an empty pixel, the levels from
// the coarsest down, each laid over the coarser ones where it has colour (pull-push): the colour of the nearest drawn world,
// smoothly spread; it fades to black with the share of drawn pixels around (a coarse level), so the colour bleeds a little
// way out and the world ends softly instead of at a hard black edge.
#define EBLOCK                                                                                                         \
    "layout(std140, set = 0, binding = 0) uniform E {\n"                                                               \
    "  vec4 sz;   // eye w h, seed w h\n"                                                                              \
    "  vec4 cr;   // filled part x y, levels, the fade's level\n"                                                      \
    "  vec4 fl;   // backdrop drawn this eye (its depth is binding 3)\n"                                               \
    "} u;\n"                                                                                                           \
    "layout(set = 0, binding = 1) uniform sampler2D sd;\n"  /* the scene depth (nearest) */                             \
    "layout(set = 0, binding = 2) uniform sampler2D ec;\n"  /* the eye output (nearest), or the seed's mips */          \
    "layout(set = 0, binding = 3) uniform sampler2D bd;\n"  /* the backdrop's depth (nearest) */                        \
    "layout(location = 0) out vec4 o;\n"                                                                               \
    "bool drawn(vec2 q) { return texture(sd, q).r < 0.9999 || (u.fl.x > 0.5 && texture(bd, q).r < 0.9999); }\n"

static const char *fvs_src =
    "#version 450\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *seed_fs =
    "#version 450\n" EBLOCK
    "void main() {\n"
    "  vec2 b = floor(gl_FragCoord.xy) * 8.0;\n"
    "  vec3 c = vec3(0.0);\n"
    "  float n = 0.0;\n"
    "  for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {\n"
    "    vec2 q = (b + vec2(float(i), float(j)) * 2.0 + 0.5) / u.sz.xy;\n"
    "    if (q.x > u.cr.x || q.y > u.cr.y) continue;\n"
    "    if (drawn(q)) { c += texture(ec, q).rgb; n += 1.0; }\n"
    "  }\n"
    "  o = vec4(c / 16.0, n / 16.0);\n"
    "}\n";

static const char *bleed_fs =
    "#version 450\n" EBLOCK
    "void main() {\n"
    "  vec2 q = gl_FragCoord.xy / u.sz.xy;\n"
    "  if (drawn(q)) discard;\n"
    "  float near = textureLod(ec, q, u.cr.w).a;\n"
    "  if (near < 0.002) discard;\n"  // (past the colour's reach: the void as it was)
    "  int top = int(u.cr.z) - 1;\n"
    "  vec4 r = textureLod(ec, q, float(top));\n"
    "  r = r.a > 1e-4 ? vec4(r.rgb / r.a, 1.0) : vec4(0.0);\n"
    "  for (int k = top - 1; k >= 0; k--) {\n"
    "    vec4 p = textureLod(ec, q, float(k));\n"
    "    r = p + (1.0 - p.a) * r;\n"
    "  }\n"
    "  vec3 c = r.a > 1e-4 ? r.rgb / r.a : vec3(0.0);\n"
    "  float f = smoothstep(0.0, 0.5, near);\n"
    "  o = vec4(c * f * f, 1.0);\n"
    "}\n";

// ------------------------------------------------------------------------------------------------ GPU objects
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; VkFormat fmt; uint32_t w, h; VkImageAspectFlags asp; } Img;
static struct {
    int ok;  // -1 not tried, 0 failed, 1 ready
    VkShaderModule vs, fs;
    VkDescriptorSetLayout sl;
    VkPipelineLayout pl;
    struct { VkFormat fmt; VkPipeline p; } pipe[4];
    Img col, dep, zb;  // the capture's colour and depth; the fill pass's own depth
    // the edge fill
    int eok;
    VkShaderModule fvs, seed_fs, bleed_fs;
    VkDescriptorSetLayout esl;
    VkPipelineLayout epl;
    VkPipeline seed_p;
    struct { VkFormat fmt; VkPipeline p; } bleed_p[4];
    struct { VkImage img; VmaAllocation mem; VkImageView all, lv0; uint32_t w, h, levels; } seed;
} g = {.ok = -1, .eok = -1};

static struct {
    bool valid;
    float inv[16], eye[3], fwd[3], anchor[3], ax, ay, zn, zf;
    bool dm1;
    double t;
    int level;
} cap;

static int state;  // 0 idle, 1 waiting for the wide culling to reach the eyes (cap_wait outputs to go)
static int cap_wait;
static double last_try;
static unsigned n_caps;
// Not while the world is changing: after a level change, a warp (the table's centre jumped) or the rig's blink, and at
// the start, wait cfg.settle seconds (the rooms stream in, the game fades in), then take a capture, and once more a
// settle later (need: captures still owed).
static double settle_until = -1;
static int need;
static int last_level = -2;
static float last_anchor[3];
static bool have_last_anchor;
extern float xr_rig_fade(void) __attribute__((weak));

static void settle_watch(double now) {
    extern int game_level_overworld(void);
    int lv = game_level_overworld();
    float a[3], lo[3], hi[3];
    bool room, zone, moved = false;
    if (stereo_table(a, &room, &zone, lo, hi)) {
        moved = have_last_anchor && hypotf(a[0] - last_anchor[0], a[2] - last_anchor[2]) > 3.0f;  // (one output: a warp, not walking)
        memcpy(last_anchor, a, sizeof a), have_last_anchor = true;
    }
    bool blink = xr_rig_fade && xr_rig_fade() > 0.01f;
    const char *why = settle_until < 0 ? "start" : lv != last_level ? "level" : moved ? "warp" : blink ? "blink" : NULL;
    if (lv != last_level && cap.valid && cap.level != lv) cap.valid = false;  // (another level's picture: gone)
    last_level = lv;
    if (!why) return;
    if (cfg.log && (now >= settle_until || why[0] != 'b')) LOG("[scene] backdrop: settling (%s)\n", why);
    settle_until = fmax(settle_until, now + (blink ? 0.5 : cfg.settle)), need = 2;
    if (state == 1) vr_scene_backdrop_wide(false), state = 0;  // (a capture asked before it: not of this)
}

static bool img_make(Img *m, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags asp) {
    if (m->img && m->fmt == fmt && m->w == w && m->h == h) return true;
    if (m->img) vk_defer_view(m->view), vk_defer_image(m->img, m->mem), memset(m, 0, sizeof *m);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &m->img, &m->mem, NULL) != VK_SUCCESS) { m->img = VK_NULL_HANDLE; return false; }
    vk_image_init_layout(vk_cmd(), m->img, asp);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = {asp & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : asp, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &m->view) != VK_SUCCESS) {
        vk_defer_image(m->img, m->mem);
        memset(m, 0, sizeof *m);
        return false;
    }
    m->fmt = fmt, m->w = w, m->h = h, m->asp = asp;
    return true;
}

static VkPipeline make_pipe(VkFormat fmt) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g.vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g.fs, .pName = "main"}};
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
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                                .depthCompareOp = VK_COMPARE_OP_LESS};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = g_vk.vids ? 3u : 2u, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt, .depthAttachmentFormat = g.zb.fmt};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = g.pl};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) {
        LOG("[scene] backdrop pipeline failed\n");
        return VK_NULL_HANDLE;
    }
    return p;
}

static bool init(void) {
    if (g.ok >= 0) return g.ok;
    g.ok = 0;
    g.vs = vk_compile_glsl(vs_src, STAGE_VERTEX, "scene_backdrop.vert");
    g.fs = vk_compile_glsl(fs_src, STAGE_FRAGMENT, "scene_backdrop.frag");
    if (!g.vs || !g.fs) { LOG("[scene] backdrop shaders failed to compile\n"); return false; }
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b};
    if (vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &g.sl) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g.sl};
    if (vkCreatePipelineLayout(g_vk.dev, &pci, NULL, &g.pl) != VK_SUCCESS) return false;
    g.ok = 1;
    return true;
}

static VkPipeline pipe_for(VkFormat fmt) {
    for (int i = 0; i < 4; i++) {
        if (g.pipe[i].p && g.pipe[i].fmt == fmt) return g.pipe[i].p;
        if (!g.pipe[i].p) return g.pipe[i].fmt = fmt, g.pipe[i].p = make_pipe(fmt);
    }
    return VK_NULL_HANDLE;
}

// ------------------------------------------------------------------------------------------------ maths
static void mul4(const float a[16], const float b[16], float o[16]) {  // row-major
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}
static bool inv4(const float m[16], float o[16]) {  // Gauss-Jordan, row-major
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c && a[r][c] != 0) { double f = a[r][c]; for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[r * 4 + c] = (float)a[r][c + 4];
    return true;
}
static void eye_of(const float V[16], float e[3], float f[3]) {  // the view's eye (-R^T t) and forward (-row 2)
    for (int k = 0; k < 3; k++) e[k] = -(V[k] * V[3] + V[4 + k] * V[7] + V[8 + k] * V[11]), f[k] = -V[8 + k];
}

// ------------------------------------------------------------------------------------------------ capture and fill
static bool want_capture(const StereoEyePost *e, double now) {
    if (now < settle_until) return false;
    if (need || !cap.valid) return true;
    if (now - last_try < cfg.gap) return false;
    float a[3], E[3], F[3], lo[3], hi[3];
    bool room, zone;
    eye_of(e->V, E, F);
    extern int game_level_overworld(void);
    if (game_level_overworld() != cap.level) return true;
    if (stereo_table(a, &room, &zone, lo, hi) && hypotf(a[0] - cap.anchor[0], a[2] - cap.anchor[2]) > cfg.move) return true;
    float c = F[0] * cap.fwd[0] + F[1] * cap.fwd[1] + F[2] * cap.fwd[2];
    if (c < cosf(cfg.turn * (float)M_PI / 180.0f)) return true;
    return cfg.refresh > 0 && now - cap.t > cfg.refresh;
}

static void capture(const StereoEyePost *e) {
    TexBackend *c = e->color, *d = e->depth;
    VkImageAspectFlags dasp = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!img_make(&g.col, c->format, c->width, c->height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !img_make(&g.dep, d->format, d->width, d->height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  d->format == VK_FORMAT_D24_UNORM_S8_UINT || d->format == VK_FORMAT_D32_SFLOAT_S8_UINT ? dasp | VK_IMAGE_ASPECT_STENCIL_BIT : dasp)) {
        LOG("[scene] backdrop: no capture images\n");
        return;
    }
    VkCommandBuffer cb = e->cb;
    vk_full_barrier(cb);
    VkImageCopy rc = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, e->layer, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {c->width, c->height, 1}};
    rec_CmdCopyImage(cb, c->image, VK_IMAGE_LAYOUT_GENERAL, g.col.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
    VkImageCopy rd = {.srcSubresource = {dasp, 0, e->depth_layer, 1}, .dstSubresource = {dasp, 0, 0, 1}, .extent = {d->width, d->height, 1}};
    rec_CmdCopyImage(cb, d->image, VK_IMAGE_LAYOUT_GENERAL, g.dep.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rd);
    vk_full_barrier(cb);
    float vp[16];
    mul4(e->P, e->V, vp);
    if (!inv4(vp, cap.inv)) return;
    eye_of(e->V, cap.eye, cap.fwd);
    float lo[3], hi[3];
    bool room, zone;
    if (!stereo_table(cap.anchor, &room, &zone, lo, hi)) memcpy(cap.anchor, cap.eye, sizeof cap.anchor);
    extern int game_level_overworld(void);
    extern uint32_t nvn_depth_mode(void);
    cap.level = game_level_overworld();
    cap.ax = e->ax, cap.ay = e->ay, cap.zn = e->zn, cap.zf = e->zf, cap.dm1 = nvn_depth_mode() == 0;
    cap.t = plat_now_ns() / 1e9;
    cap.valid = true;
    if (need > 0 && --need > 0) settle_until = cap.t + cfg.settle;
    n_caps++;
    if (cfg.log) LOG("[scene] backdrop capture %u: eye (%.1f %.1f %.1f) table (%.1f %.1f) fwd (%.2f %.2f %.2f), filled %.2f x %.2f\n", n_caps,
                     cap.eye[0], cap.eye[1], cap.eye[2], cap.anchor[0], cap.anchor[2], cap.fwd[0], cap.fwd[1], cap.fwd[2], cap.ax, cap.ay);
}

typedef struct { float inv[16], cur[16], grid[4], cz[4], ce[4], sz[4]; } UBlock;

static bool fill(const StereoEyePost *e) {
    TexBackend *c = e->color, *d = e->depth;
    if (!img_make(&g.zb, VK_FORMAT_D16_UNORM, c->width, c->height, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
    VkPipeline p = pipe_for(c->format);
    if (!p) return false;
    static const VkComponentMapping id = {0};
    VkImageView cv = vk_image_view(c, VK_IMAGE_VIEW_TYPE_2D, c->format, 0, 1, e->layer, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView dv = vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, e->depth_layer, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!cv || !dv) return false;
    UBlock u;
    memset(&u, 0, sizeof u);
    memcpy(u.inv, cap.inv, 64);
    mul4(e->P, e->V, u.cur);
    u.grid[0] = u.grid[1] = (float)cfg.grid, u.grid[2] = cap.ax, u.grid[3] = cap.ay;
    u.cz[0] = cap.zn, u.cz[1] = cap.zf - cap.zn, u.cz[2] = cap.dm1 ? 1.0f : 0.0f, u.cz[3] = cfg.jump;
    memcpy(u.ce, cap.eye, 12);
    u.sz[0] = (float)c->width, u.sz[1] = (float)c->height;
    VkSpan s = vk_transient_copy(&u, sizeof u, 256);
    VkDescriptorSet set = vk_alloc_set(g.sl);
    VkSampler nearest = vk_default_sampler(false), lin = vk_default_sampler(true);
    VkDescriptorBufferInfo bi = {s.buf, s.off, sizeof u};
    VkDescriptorImageInfo im[3] = {{nearest, g.dep.view, VK_IMAGE_LAYOUT_GENERAL}, {lin, g.col.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {nearest, dv, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[4];
    for (uint32_t i = 0; i < 4; i++) {
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1};
        if (i == 0) wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, wr[i].pBufferInfo = &bi;
        else wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[i].pImageInfo = &im[i - 1];
    }
    rec_UpdateDescriptorSets(4, wr);
    VkCommandBuffer cb = e->cb;
    vk_full_barrier(cb);
    uint32_t w = (uint32_t)lroundf(e->ax * (float)c->width), h = (uint32_t)lroundf(e->ay * (float)c->height);
    if (!w || !h) return false;
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = cv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo za = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g.zb.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.depthStencil = {1.0f, 0}}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &za};
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
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl, 0, 1, &set, 0, NULL);
    rec_CmdDraw(cb, (uint32_t)(cfg.grid * cfg.grid * 6), 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    return true;
}

// ------------------------------------------------------------------------------------------------ the edge fill
static VkPipeline efs_pipe(VkShaderModule fs, VkFormat fmt) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g.fvs, .pName = "main"},
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
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = g_vk.vids ? 3u : 2u, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = g.epl};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[scene] edge pipeline failed\n");
    return p;
}

static bool einit(void) {
    if (g.eok >= 0) return g.eok;
    g.eok = 0;
    g.fvs = vk_compile_glsl(fvs_src, STAGE_VERTEX, "scene_edge.vert");
    g.seed_fs = vk_compile_glsl(seed_fs, STAGE_FRAGMENT, "scene_edge_seed.frag");
    g.bleed_fs = vk_compile_glsl(bleed_fs, STAGE_FRAGMENT, "scene_edge_bleed.frag");
    if (!g.fvs || !g.seed_fs || !g.bleed_fs) { LOG("[scene] edge shaders failed to compile\n"); return false; }
    VkDescriptorSetLayoutBinding b[4] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                         {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                         {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b};
    if (vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &g.esl) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g.esl};
    if (vkCreatePipelineLayout(g_vk.dev, &pci, NULL, &g.epl) != VK_SUCCESS) return false;
    g.seed_p = efs_pipe(g.seed_fs, VK_FORMAT_R16G16B16A16_SFLOAT);
    if (!g.seed_p) return false;
    g.eok = 1;
    return true;
}

static bool seed_make(uint32_t w, uint32_t h) {
    if (g.seed.img && g.seed.w == w && g.seed.h == h) return true;
    if (g.seed.img) vk_defer_view(g.seed.all), vk_defer_view(g.seed.lv0), vk_defer_image(g.seed.img, g.seed.mem), memset(&g.seed, 0, sizeof g.seed);
    uint32_t lv = 1;
    while ((w >> lv) >= 2 && (h >> lv) >= 2 && lv < 12) lv++;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                            .extent = {w, h, 1}, .mipLevels = lv, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &g.seed.img, &g.seed.mem, NULL) != VK_SUCCESS) { g.seed.img = VK_NULL_HANDLE; return false; }
    {   // (every level GENERAL: vk_image_init_layout does level 0 only)
        VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = g.seed.img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, lv, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
        rec_CmdPipelineBarrier2(vk_cmd(), &di);
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g.seed.img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R16G16B16A16_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, lv, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &g.seed.all) != VK_SUCCESS) return false;
    vi.subresourceRange.levelCount = 1;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &g.seed.lv0) != VK_SUCCESS) return false;
    g.seed.w = w, g.seed.h = h, g.seed.levels = lv;
    return true;
}

static VkSampler mip_sampler(void) {
    static VkSampler s;
    if (s) return s;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .maxLod = 16.0f};
    if (vkCreateSampler(g_vk.dev, &si, NULL, &s) != VK_SUCCESS) s = VK_NULL_HANDLE;
    return s;
}

typedef struct { float sz[4], cr[4], fl[4]; } EBlock;

static VkDescriptorSet eset(const EBlock *eb, VkImageView sd, VkImageView ec, VkSampler ecs, VkImageView bd) {
    VkSpan s = vk_transient_copy(eb, sizeof *eb, 256);
    VkDescriptorSet set = vk_alloc_set(g.esl);
    VkSampler nearest = vk_default_sampler(false);
    VkDescriptorBufferInfo bi = {s.buf, s.off, sizeof *eb};
    VkDescriptorImageInfo im[3] = {{nearest, sd, VK_IMAGE_LAYOUT_GENERAL}, {ecs, ec, VK_IMAGE_LAYOUT_GENERAL}, {nearest, bd, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[4];
    for (uint32_t i = 0; i < 4; i++) {
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1};
        if (i == 0) wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, wr[i].pBufferInfo = &bi;
        else wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[i].pImageInfo = &im[i - 1];
    }
    rec_UpdateDescriptorSets(4, wr);
    return set;
}

static void epass(VkCommandBuffer cb, VkImageView target, uint32_t w, uint32_t h, VkPipeline p, VkDescriptorSet set) {
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = target, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
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
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.epl, 0, 1, &set, 0, NULL);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
}

static void edge_fill(const StereoEyePost *e, bool backdrop_drawn) {
    TexBackend *c = e->color, *d = e->depth;
    uint32_t sw = (c->width + 7) / 8, sh = (c->height + 7) / 8;
    if (!einit() || !seed_make(sw, sh) || !mip_sampler()) return;
    VkPipeline bp = VK_NULL_HANDLE;
    for (int i = 0; i < 4 && !bp; i++) {
        if (g.bleed_p[i].p && g.bleed_p[i].fmt == c->format) bp = g.bleed_p[i].p;
        else if (!g.bleed_p[i].p) g.bleed_p[i].fmt = c->format, bp = g.bleed_p[i].p = efs_pipe(g.bleed_fs, c->format);
    }
    if (!bp) return;
    if (!backdrop_drawn && !img_make(&g.zb, VK_FORMAT_D16_UNORM, c->width, c->height, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                     VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    static const VkComponentMapping id = {0};
    VkImageView cv = vk_image_view(c, VK_IMAGE_VIEW_TYPE_2D, c->format, 0, 1, e->layer, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView dv = vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, e->depth_layer, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!cv || !dv) return;
    EBlock eb = {.sz = {(float)c->width, (float)c->height, (float)sw, (float)sh}, .cr = {e->ax, e->ay, 0, 0}, .fl = {backdrop_drawn ? 1.0f : 0.0f}};
    // the fade's level: where a seed texel covers about the reach (share of the eye's height, in eighth-size texels)
    float reach = cfg.edge_reach * (float)c->height / 8.0f;
    eb.cr[2] = (float)g.seed.levels, eb.cr[3] = fminf(log2f(fmaxf(reach, 1.0f)), (float)g.seed.levels - 1);
    VkCommandBuffer cb = e->cb;
    vk_full_barrier(cb);
    epass(cb, g.seed.lv0, sw, sh, g.seed_p, eset(&eb, dv, cv, vk_default_sampler(false), g.zb.view));
    for (uint32_t l = 1; l < g.seed.levels; l++) {
        vk_full_barrier(cb);
        int32_t w0 = (int32_t)(sw >> (l - 1)), h0 = (int32_t)(sh >> (l - 1)), w1 = (int32_t)(sw >> l), h1 = (int32_t)(sh >> l);
        VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1}, .srcOffsets = {{0, 0, 0}, {w0 ? w0 : 1, h0 ? h0 : 1, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, .dstOffsets = {{0, 0, 0}, {w1 ? w1 : 1, h1 ? h1 : 1, 1}}};
        rec_CmdBlitImage(cb, g.seed.img, VK_IMAGE_LAYOUT_GENERAL, g.seed.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_LINEAR);
    }
    vk_full_barrier(cb);
    uint32_t w = (uint32_t)lroundf(e->ax * (float)c->width), h = (uint32_t)lroundf(e->ay * (float)c->height);
    if (w && h) epass(cb, cv, w, h, bp, eset(&eb, dv, g.seed.all, mip_sampler(), g.zb.view));
    vk_full_barrier(cb);
}

// (exec.c vk_stereo_eyes, before stereo_eye_post) each new eye output
void scene_eye_post(const StereoEyePost *e) {
    if (!cfg.init) cfg_init();
    int mode = vr_scene_mode();
    if (mode != VR_SCENE_BACKDROP && (state || cap.valid || settle_until >= 0))
        vr_scene_backdrop_wide(false), state = 0, cap.valid = false, settle_until = -1, have_last_anchor = false;
    if (mode == VR_SCENE_OFF || e->eye < 0 || e->eye > 1 || !e->depth || !e->color) return;
    extern int game_level_overworld(void);
    bool edge = cfg.edge && game_level_overworld() == 1;  // (indoors the dark around a room is the game's look)
    if (mode != VR_SCENE_BACKDROP || !e->have_matrices || !init()) {
        if (edge) edge_fill(e, false);
        return;
    }
    if (e->eye == 0) {
        double now = plat_now_ns() / 1e9;
        settle_watch(now);
        if (state == 1 && --cap_wait <= 0) {
            capture(e);
            vr_scene_backdrop_wide(false), state = 0;
        } else if (state == 0 && want_capture(e, now)) {
            vr_scene_backdrop_wide(true), state = 1, cap_wait = cfg.delay, last_try = now;
            if (cfg.log) LOG("[scene] backdrop: capture asked (%s)\n", need ? "settled" : cap.valid ? "moved, turned or due" : "none yet");
        }
    }
    bool drawn = cap.valid && fill(e);  // (also while a capture is pending: until the wide culling reaches the eyes they have holes)
    if (edge) edge_fill(e, drawn);
}
