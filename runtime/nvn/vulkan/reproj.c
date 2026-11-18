// The refreshes that show a set again, drawn for their own head pose (LARECOMP_XR_REPROJ; runtime/xr/xr.h,
// docs/af/STEREO.md "Re-shown refreshes").
//
// At 60 FPS on a 120 Hz display each set of eye images is shown on two refreshes (three at 90 Hz and four at 120 Hz in
// the 30 FPS hold). Every refresh's layer carries the pose the images were rendered with. SteamVR sees an app that
// submits on time at every refresh: if it puts such a layer up as it is, the second refresh shows the same image at
// the same place while the head has moved, so the world jumps with the head every other refresh (the doubling). Here
// each refresh after the set's first gets eye images of its own, drawn from the set's finished XR eye images (so the
// Table Edge, anti-aliasing, the upscale, the blink and Passthrough's alpha come along), for the head pose located for
// that refresh's display time, and its layer carries that pose. The XR thread only picks, by display time, among
// images whose submission has finished (xr_loop.c end_frame): it never waits for or records GPU work.
// The views are located and the passes planned at the present, inside the set's own commands (reproj_eyes), but
// recorded after the set's submission, each refresh in a submission of its own (reproj_submit, from device.c
// vk_frame_end before the set goes to the XR thread): the set's own images finish as early as without them, and each
// refresh's images are released (and shown) once their own submission is done; one not done by its refresh leaves the
// newest finished image up. They read the set's XR images after the set's submission: the XR thread releases the set
// only once they are done (xr_loop.c xr_release_completed), so the runtime never has an image they still touch; with
// LARECOMP_XR_REPROJ_JOINT=0 (a test) already once the driver has those reads (rec_submitted).
// Drawing them later, nearer each refresh, is not possible: the set's images go to the runtime at its first
// refresh and the scene depth is drawn over by the next frame, and on the one queue a later submission waits behind
// the next frame's queued work (docs/af/STEREO.md).
//
//   rot     rotation only: per pixel one colour read (the image as a plane at infinity)
//   pos     rotation and position from the scene's depth (the eye post hook's, full size where the frame had it): a
//           fixed-point search for the source pixel whose point lands on this one (ITER depth reads), then one colour
//           read. Pixels without depth (the void, glows, the Backdrop) are taken on the table's floor plane, as the
//           fade takes them, and infinitely far above its horizon. Where a nearer surface uncovers a farther one the
//           search settles on one side of the edge: the uncovered strip repeats the pixels beside it (stretch). A
//           set that shows an older output again (no new composite) is drawn by rotation only: the depth is no
//           longer its own.
//   follow  pos, with the table's follow motion carried on for the refresh (stereo.c rig_ahead): the world does not
//           step at 60 Hz while Link walks outdoors; Link and other moving things step with it instead (only real
//           in-between frames would fix those).
// The HUD is a layer of its own and is not touched. Past the source image's edge the edge pixels repeat.
// A refresh whose change of pose moves no pixel more than LARECOMP_XR_REPROJ_SKIP (0.25 px) gets no images: it shows
// the set's own (or the last drawn), with their poses. LARECOMP_XR_REPROJ_ITER: the search's depth reads (3).
// LARECOMP_XR_REPROJ_MV=1: both eyes in one pass (multiview, into one swapchain image of two layers) instead of one
// pass an eye. LARECOMP_XR_REPROJ_RECT=1: only the part of the image the eye's rendered size fills (dynamic resolution
// under the image's size: the layer then names that part and the compositor scales it). Both off by default.
// LARECOMP_XR_REPROJ_LOG=1: a [reproj] line every 5 s with the passes, reads and GPU time.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../platform.h"
#include "../../rt.h"
#include "record.h"
#include "stereo.h"
#include "vk.h"
#ifdef LARECOMP_HAVE_XR
#include "../../xr/xr.h"
#endif

void reproj_eye_post(const StereoEyePost *e);
void reproj_eyes(bool new_outputs, const uint32_t filled[2][2]);
void reproj_submit(void);

#ifndef LARECOMP_HAVE_XR
void reproj_eye_post(const StereoEyePost *e) { (void)e; }
void reproj_eyes(bool new_outputs, const uint32_t filled[2][2]) { (void)new_outputs, (void)filled; }
void reproj_submit(void) {}
#else

static struct {
    bool init;
    float skip_px;
    int iter, log, mv, rect;
} cfg;

static void cfg_init(void) {
    const char *e;
    cfg.init = true;
    cfg.skip_px = (e = getenv("LARECOMP_XR_REPROJ_SKIP")) && atof(e) >= 0 ? (float)atof(e) : 0.25f;
    cfg.iter = (e = getenv("LARECOMP_XR_REPROJ_ITER")) && atoi(e) > 0 ? atoi(e) : 3;
    if (cfg.iter > 8) cfg.iter = 8;
    cfg.log = (e = getenv("LARECOMP_XR_REPROJ_LOG")) && atoi(e) > 0;
    cfg.mv = (e = getenv("LARECOMP_XR_REPROJ_MV")) && atoi(e) > 0;
    cfg.rect = (e = getenv("LARECOMP_XR_REPROJ_RECT")) && atoi(e) > 0;
    if (cfg.mv && !g_vk.multiview) LOG("[reproj] LARECOMP_XR_REPROJ_MV: the device has no multiview on: one pass an eye\n"), cfg.mv = 0;
    if (cfg.mv || cfg.rect)
        LOG("[reproj] passes:%s%s\n", cfg.mv ? " both eyes in one (multiview)" : "", cfg.rect ? " the part the eye's rendered size fills" : "");
}

// ------------------------------------------------------------------------------------------------ the shader
// p: a position in the target XR image (0..1, top down: the compositor's, as its fov gives it); its ray in the eye,
// the source position first by rotation alone, then (u.fl.z > 0) moved by the miss of its point's projection into the
// target, u.fl.z times. The depth is the game's image: read where the present took the colour from (flipped with a
// lower-left origin, as the edge pass reads it). Two eyes (LARECOMP_XR_REPROJ_MV): one block, colour and depth each,
// picked by the view.
static const char *vs_src =
    "#version 450\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *fs_one =
    "#version 450\n"
    "#define NE 1\n"
    "#define VI 0\n"
    "#define SRC(p) texture(src0, p)\n"
    "#define DEP(p) texture(dep0, p)\n";
static const char *fs_two =
    "#version 450\n"
    "#extension GL_EXT_multiview : require\n"
    "#define NE 2\n"
    "#define VI gl_ViewIndex\n"
    "#define SRC(p) (gl_ViewIndex != 0 ? textureLod(src1, p, 0.0) : textureLod(src0, p, 0.0))\n"
    "#define DEP(p) (gl_ViewIndex != 0 ? textureLod(dep1, p, 0.0) : textureLod(dep0, p, 0.0))\n";
static const char *fs_src =
    "struct Eye {\n"
    "  vec4 q[3];  // target eye -> source eye rotation (rows)\n"
    "  mat4 X;     // source eye -> target eye (game units)\n"
    "  vec4 ts;    // source fov: tan left, right, up, down\n"
    "  vec4 tt;    // target fov\n"
    "  vec4 zt;    // the source's depth: A = P[2][2], B = P[2][3], zn, zf - zn\n"
    "  vec4 fl;    // flip, depth in -1..1, search steps (0: rotation only), -\n"
    "  vec4 fi;    // the depth's filled part (x y), the target's drawn size (z w)\n"
    "  vec4 pl;    // the table's floor in the source eye: normal xyz, w = its offset (n . v = w on it)\n"
    "};\n"
    "layout(std140, set = 0, binding = 0) uniform U { Eye e[NE]; } ub;\n"
    "layout(set = 0, binding = 1) uniform sampler2D src0;\n"
    "layout(set = 0, binding = 2) uniform sampler2D dep0;\n"
    "#if NE > 1\n"
    "layout(set = 0, binding = 3) uniform sampler2D src1;\n"
    "layout(set = 0, binding = 4) uniform sampler2D dep1;\n"
    "#endif\n"
    "layout(location = 0) out vec4 o;\n"
    "Eye u;\n"
    "vec3 ray_at(vec2 p, vec4 t) { return vec3(t.x + p.x * (t.y - t.x), t.z - p.y * (t.z - t.w), -1.0); }\n"
    "vec2 pos_of(vec3 v, vec4 t) { vec2 a = v.xy / max(-v.z, 1e-6); return vec2((a.x - t.x) / (t.y - t.x), (t.z - a.y) / (t.z - t.w)); }\n"
    "vec4 point(vec2 ps) {\n"  // the source's point at ps, in its eye (w 0: a direction)
    "  vec3 ray = ray_at(ps, u.ts);\n"
    "  vec2 q = clamp(vec2(ps.x, u.fl.x > 0.5 ? 1.0 - ps.y : ps.y), 0.0, 1.0);\n"
    "  float d = DEP(q * u.fi.xy).r;\n"
    "  if (d < 0.9999) {\n"
    "    float z = (d - u.zt.z) / u.zt.w;\n"
    "    if (u.fl.y > 0.5) z = z * 2.0 - 1.0;\n"
    "    return vec4(ray * (u.zt.y / (z + u.zt.x)), 1.0);\n"
    "  }\n"
    "  float dn = dot(u.pl.xyz, ray), t = abs(dn) > 1e-6 ? u.pl.w / dn : -1.0;\n"
    "  return t > 0.0 ? vec4(ray * t, 1.0) : vec4(ray, 0.0);\n"
    "}\n"
    "void main() {\n"
    "  u = ub.e[VI];\n"
    "  vec2 p = gl_FragCoord.xy / u.fi.zw;\n"
    "  vec3 r = ray_at(p, u.tt);\n"
    "  vec2 ps = pos_of(vec3(dot(u.q[0].xyz, r), dot(u.q[1].xyz, r), dot(u.q[2].xyz, r)), u.ts);\n"
    "  for (int i = 0; i < int(u.fl.z); i++) ps += p - pos_of((u.X * point(ps)).xyz, u.tt);\n"
    "  o = SRC(ps);\n"
    "}\n";

typedef struct { float q[3][4], X[16], ts[4], tt[4], zt[4], fl[4], fi[4], pl[4]; } UBlock;

// ------------------------------------------------------------------------------------------------ GPU objects
// [v]: 0 one eye a pass, 1 both eyes in one (multiview: a view mask of both, the target's two layers)
static struct {
    int ok;  // -1 not tried, 0 failed, 1 ready
    VkShaderModule vs, fs[2];
    VkDescriptorSetLayout sl[2];
    VkPipelineLayout pl[2];
    struct { VkFormat fmt; VkPipeline p; } pipe[2][2];
} g = {.ok = -1};

static VkShaderModule fs_module(int v) {
    size_t n = strlen(fs_two) + strlen(fs_src) + 1;
    char *t = malloc(n);
    if (!t) return VK_NULL_HANDLE;
    snprintf(t, n, "%s%s", v ? fs_two : fs_one, fs_src);
    VkShaderModule m = vk_compile_glsl(t, STAGE_FRAGMENT, v ? "reproj_mv.frag" : "reproj.frag");
    free(t);
    return m;
}

static bool init(void) {
    if (g.ok >= 0) return g.ok;
    g.ok = 0;
    g.vs = vk_compile_glsl(vs_src, STAGE_VERTEX, "reproj.vert");
    g.fs[0] = fs_module(0);
    if (cfg.mv && !(g.fs[1] = fs_module(1))) LOG("[reproj] the multiview shader failed to compile: one pass an eye\n"), cfg.mv = 0;
    if (!g.vs || !g.fs[0]) { LOG("[reproj] shaders failed to compile: re-shown refreshes keep the set's images\n"); return false; }
    for (int v = 0; v < (cfg.mv ? 2 : 1); v++) {
        uint32_t nb = v ? 5 : 3;
        VkDescriptorSetLayoutBinding b[5];
        for (uint32_t i = 0; i < nb; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                  VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &g.sl[v]));
        VkPipelineLayoutCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g.sl[v]};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pci, NULL, &g.pl[v]));
    }
    g.ok = 1;
    return true;
}

static VkPipeline pipe_for(VkFormat fmt, int v) {
    for (int i = 0; i < 2; i++) {
        if (g.pipe[v][i].p && g.pipe[v][i].fmt == fmt) return g.pipe[v][i].p;
        if (g.pipe[v][i].p) continue;
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g.vs, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g.fs[v], .pName = "main"}};
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
        VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .viewMask = v ? 3u : 0u,
                                            .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
        VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                           .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                           .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                           .pColorBlendState = &cb, .pDynamicState = &dy, .layout = g.pl[v]};
        if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &g.pipe[v][i].p) != VK_SUCCESS) {
            LOG("[reproj] pipeline failed\n");
            g.pipe[v][i].p = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        g.pipe[v][i].fmt = fmt;
        return g.pipe[v][i].p;
    }
    return VK_NULL_HANDLE;
}

static void layout_to(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &di);
}

// ------------------------------------------------------------------------------------------------ GPU time
// A timestamp pair around each refresh's passes, resolved a few frames later (LARECOMP_XR_REPROJ_LOG)
enum { TSQ = 32 };
static VkQueryPool ts_pool;
static struct { uint64_t seq; uint32_t q; } ts_ring[TSQ / 2];
static uint32_t ts_head, ts_tail;
static uint32_t ts_begin(VkCommandBuffer cb) {
    if (!cfg.log) return ~0u;
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
static void ts_end(VkCommandBuffer cb, uint32_t q) {
    if (q == ~0u) return;
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ts_pool, q + 1);
    ts_ring[ts_head % (TSQ / 2)].seq = vk_pending_seq(), ts_ring[ts_head % (TSQ / 2)].q = q;
    ts_head++;
}

static struct {
    uint64_t sets, due, drawn, skipped, failed, followed, rot_only, passes, px, subs, sets_drawn, rps;
    double gpu_sum, gpu_max, part;  // (part: the drawn share of the images' pixels, summed over the refreshes drawn)
    int gpu_n;
    double t0;
} st;

static void stats_tick(void) {
    uint64_t done = vk_completed_seq();
    while (ts_tail != ts_head) {
        __typeof__(ts_ring[0]) *t = &ts_ring[ts_tail % (TSQ / 2)];
        if (t->seq > done) break;
        uint64_t v[2];
        if (vkGetQueryPoolResults(g_vk.dev, ts_pool, t->q, 2, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && v[1] >= v[0]) {
            double ms = (double)(v[1] - v[0]) * g_vk.props.limits.timestampPeriod / 1e6;
            st.gpu_sum += ms, st.gpu_n++;
            if (ms > st.gpu_max) st.gpu_max = ms;
        }
        ts_tail++;
    }
    double now = plat_now_ns() / 1e9;
    if (st.t0 == 0) st.t0 = now;
    if (now - st.t0 < 5.0) return;
    if (cfg.log && st.sets)
        LOG("[reproj] %.0f s: %llu sets, %llu re-shown refreshes due: %llu drawn (%llu eye images in %llu passes, %.0f %% of their "
            "pixels, %.1f reads a pixel; %llu with the "
            "table's follow carried on, %llu rotation only: an older output), %llu skipped (no pixel moved %.2f px), %llu without "
            "images; %llu submissions after the sets'; GPU %.3f ms a refresh's images (max %.3f) over %d, %.3f ms a set\n",
            now - st.t0, (unsigned long long)st.sets, (unsigned long long)st.due, (unsigned long long)st.drawn,
            (unsigned long long)st.passes, (unsigned long long)st.rps, st.drawn ? 100.0 * st.part / st.drawn : 0.0,
            st.passes ? (double)st.px / st.passes : 0.0, (unsigned long long)st.followed,
            (unsigned long long)st.rot_only, (unsigned long long)st.skipped, cfg.skip_px, (unsigned long long)st.failed,
            (unsigned long long)st.subs, st.gpu_n ? st.gpu_sum / st.gpu_n : -1.0, st.gpu_max, st.gpu_n,
            st.gpu_n && st.subs && st.sets_drawn ? st.gpu_sum / st.gpu_n * st.subs / st.sets_drawn : -1.0);
    double t0 = now;
    memset(&st, 0, sizeof st);
    st.t0 = t0;
}

// ------------------------------------------------------------------------------------------------ the eyes
// What the eye post hook noted for each eye: the latest new output's depth (the scene's own at full size when the
// frame had it, else the game's quarter-size copy; each through its own filled part)
static struct { bool have; TexBackend *depth; uint32_t layer; float fx, fy, zn, zf; } noted[2];

void reproj_eye_post(const StereoEyePost *e) {
    if (e->eye < 0 || e->eye > 1) return;
    __typeof__(noted[0]) *n = &noted[e->eye];
    if (e->depth_full) n->depth = e->depth_full, n->layer = e->depth_full_layer, n->fx = e->fax, n->fy = e->fay, n->zn = e->fzn, n->zf = e->fzf;
    else n->depth = e->depth, n->layer = e->depth_layer, n->fx = e->dax, n->fy = e->day, n->zn = e->zn, n->zf = e->zf;
    n->have = n->depth && e->have_matrices;
}

extern uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void);

// The fov's tangents (left, right, up, down) back from a projection eye_proj built (stereo.c: P00 = sx 2 / (r - l),
// P02 = sx (r + l) / (r - l), the same in y; sx, sy the game's axis signs)
static void fov_tans(const float P[16], float t[4]) {
    float w = 2.0f / fabsf(P[0]), c = P[2] / P[0], h = 2.0f / fabsf(P[5]), m = P[6] / P[5];
    t[0] = c - 0.5f * w, t[1] = c + 0.5f * w, t[2] = m + 0.5f * h, t[3] = m - 0.5f * h;
}

static void inverse_rigid(const float V[16], double I[16]) {  // V = [R t; 0 1] -> [R^T -R^T t; 0 1]
    memset(I, 0, 16 * sizeof(double));
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) I[r * 4 + c] = V[c * 4 + r];
        I[r * 4 + 3] = -(V[0 * 4 + r] * V[3] + V[1 * 4 + r] * V[7] + V[2 * 4 + r] * V[11]);
    }
    I[15] = 1;
}

// One eye's block: source XR image seen from Vs / Ps, the target for Vt / Pt, drawn w x h. Returns the depth's view
// (VK_NULL_HANDLE: rotation only, u->fl[2] 0).
static VkImageView eye_block(UBlock *u, const float Vs[16], const float Ps[16], const float Vt[16], const float Pt[16], int eye, bool depth,
                             float floor_y, uint32_t w, uint32_t h) {
    memset(u, 0, sizeof *u);
    double Vi[16], X[16];
    inverse_rigid(Vs, Vi);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double a = 0;
            for (int k = 0; k < 4; k++) a += Vt[r * 4 + k] * Vi[k * 4 + c];
            X[r * 4 + c] = a;
        }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) u->X[c * 4 + r] = (float)X[r * 4 + c];  // (GLSL: columns)
    for (int r = 0; r < 3; r++)  // Rs * Rt^T
        for (int c = 0; c < 3; c++) u->q[r][c] = Vs[r * 4 + 0] * Vt[c * 4 + 0] + Vs[r * 4 + 1] * Vt[c * 4 + 1] + Vs[r * 4 + 2] * Vt[c * 4 + 2];
    fov_tans(Ps, u->ts), fov_tans(Pt, u->tt);
    const __typeof__(noted[0]) *n = &noted[eye];
    u->zt[0] = Ps[10], u->zt[1] = Ps[11], u->zt[2] = n->zn, u->zt[3] = n->zf - n->zn;
    u->fl[0] = renderer_origin_lower_left() ? 1.0f : 0.0f, u->fl[1] = nvn_depth_mode() == 0 ? 1.0f : 0.0f, u->fl[2] = depth ? (float)cfg.iter : 0.0f;
    u->fi[0] = n->fx, u->fi[1] = n->fy, u->fi[2] = (float)w, u->fi[3] = (float)h;
    // the floor (world y = floor_y) in the source eye: n = R's second column, v = R w + t, so n . v = floor_y + n . t
    u->pl[0] = Vs[1], u->pl[1] = Vs[5], u->pl[2] = Vs[9], u->pl[3] = floor_y + Vs[1] * Vs[3] + Vs[5] * Vs[7] + Vs[9] * Vs[11];
    VkImageView dv = VK_NULL_HANDLE;
    if (depth) {
        static const VkComponentMapping id = {0};
        dv = vk_image_view(n->depth, VK_IMAGE_VIEW_TYPE_2D, n->depth->format, 0, 1, n->layer, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    if (!dv) u->fl[2] = 0;  // (rotation only: the depth binding is not read)
    return dv;
}

// One pass into dst (w x h of it): eyes `ne` blocks (2: both eyes, a view each, into dst's two layers), the sources
// sv[e] (SHADER_READ_ONLY_OPTIMAL) and depths dv[e] (none: the colour, not read).
static void pass(VkCommandBuffer cb, const XrEyeTarget *dst, int ne, const UBlock u[2], const VkImageView sv[2], const VkImageView dv[2],
                 uint32_t w, uint32_t h) {
    int v = ne > 1;
    VkPipeline p = pipe_for(dst->format, v);
    if (!p) return;
    VkSpan s = vk_transient_copy(u, sizeof u[0] * (size_t)ne, 256);
    VkDescriptorSet set = vk_alloc_set(g.sl[v]);
    VkDescriptorBufferInfo bi = {s.buf, s.off, sizeof u[0] * (size_t)ne};
    VkDescriptorImageInfo im[4];
    for (int e = 0; e < ne; e++) {
        im[2 * e] = (VkDescriptorImageInfo){vk_default_sampler(true), sv[e], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        im[2 * e + 1] = dv[e] ? (VkDescriptorImageInfo){vk_default_sampler(false), dv[e], VK_IMAGE_LAYOUT_GENERAL} : im[2 * e];
    }
    VkWriteDescriptorSet wr[5];
    uint32_t nb = (uint32_t)(1 + 2 * ne);
    for (uint32_t i = 0; i < nb; i++) {
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1};
        if (i == 0) wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, wr[i].pBufferInfo = &bi;
        else wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[i].pImageInfo = &im[i - 1];
    }
    rec_UpdateDescriptorSets(nb, wr);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst->view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1, .viewMask = v ? 3u : 0u,
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
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl[v], 0, 1, &set, 0, NULL);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    st.rps++;
    for (int e = 0; e < ne; e++) st.passes++, st.px += (uint64_t)(1 + (int)u[e].fl[2]);
}

// How far, in pixels at the image's centre, the change from Vs to Vt moves anything: the rotation's angle, and (with
// depth) the eye's move against half its distance to the table's centre
static float moved_px(const float Vs[16], const float Vt[16], const float Pt[16], uint32_t w, bool depth, const float anchor[3]) {
    float M[3][3];  // Rs Rt^T; its angle from both its sine and cosine parts (acos alone: 0.03 degrees of noise near 0)
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) M[r][c] = Vs[r * 4 + 0] * Vt[c * 4 + 0] + Vs[r * 4 + 1] * Vt[c * 4 + 1] + Vs[r * 4 + 2] * Vt[c * 4 + 2];
    float sx = M[2][1] - M[1][2], sy = M[0][2] - M[2][0], sz = M[1][0] - M[0][1];
    float ang = atan2f(0.5f * sqrtf(sx * sx + sy * sy + sz * sz), 0.5f * (M[0][0] + M[1][1] + M[2][2] - 1));
    float f = 0.5f * (float)w * fabsf(Pt[0]);  // (pixels a radian at the centre)
    float px = ang * f;
    if (depth) {
        float Es[3], Et[3], d2 = 0, m2 = 0;
        for (int k = 0; k < 3; k++) {
            Es[k] = -(Vs[k] * Vs[3] + Vs[4 + k] * Vs[7] + Vs[8 + k] * Vs[11]);
            Et[k] = -(Vt[k] * Vt[3] + Vt[4 + k] * Vt[7] + Vt[8 + k] * Vt[11]);
            m2 += (Et[k] - Es[k]) * (Et[k] - Es[k]), d2 += (Es[k] - anchor[k]) * (Es[k] - anchor[k]);
        }
        px += sqrtf(m2) / fmaxf(0.5f * sqrtf(d2), 1e-3f) * f;
    }
    return px;
}

// What reproj_eyes planned for the set being presented, for reproj_submit (the render thread's)
static struct {
    int n;  // refreshes the set is due on (0: nothing to draw)
    bool depth;
    float floor_y;
    XrEyeTarget src[2];
    float Vs[2][16], Ps[2][16];
    bool want[3], fol[3];  // ([k - 1]: xr_reshow_times gives at most 4 refreshes)
    float Vt[3][2][16], Pt[3][2][16];
    uint32_t w[2], h[2];  // the part of each eye's target to draw
} plan;

// (exec.c vk_stereo_eyes, both eyes drawn into the set's eye images and its views set, still inside the set's commands)
// the set's re-shown refreshes: their views located now, the ones to draw planned; nothing recorded here (reproj_submit).
// new_outputs: both eye images came from new composites this frame (their noted depth is theirs). filled: each eye's
// rendered size as its XR image shows it (the eye output's filled part; the image's own size when upscaled into it).
void reproj_eyes(bool new_outputs, const uint32_t filled[2][2]) {
    if (!cfg.init) cfg_init();
    stats_tick();
    plan.n = 0;
    int mode = xr_reproj_mode();
    if (mode < XR_REPROJ_ROT) return;
    int64_t times[4];
    int n = xr_reshow_times(times);
    if (n < 2) return;
    st.sets++, st.due += (uint64_t)(n - 1);
    for (int e = 0; e < 2; e++)
        if (!xr_eye_target(e, &plan.src[e]) || !stereo_eye_matrices(e, plan.Vs[e], plan.Ps[e])) { st.failed += (uint64_t)(n - 1); return; }
    if (!init()) { st.failed += (uint64_t)(n - 1); return; }
    float anchor[3] = {0}, lo[3], hi[3];
    bool room, zone;
    bool table = stereo_table(anchor, &room, &zone, lo, hi);
    plan.depth = mode >= XR_REPROJ_POS && new_outputs && noted[0].have && noted[1].have && table;
    plan.floor_y = anchor[1];
    if (mode >= XR_REPROJ_POS && !plan.depth) st.rot_only++;
    for (int e = 0; e < 2; e++) {
        uint32_t w = plan.src[e].extent.width, h = plan.src[e].extent.height;
        plan.w[e] = cfg.rect && filled[e][0] && filled[e][0] < w ? filled[e][0] : w;
        plan.h[e] = cfg.rect && filled[e][1] && filled[e][1] < h ? filled[e][1] : h;
    }
    bool any = false;
    for (int k = 1; k < n; k++) {
        plan.want[k - 1] = false;
        XrEyeView ev[2];
        if (!xr_reshow_views(k, ev)) { st.failed++; continue; }
        float px = 0;
        bool ok = true, fol = false;
        float ahead = mode == XR_REPROJ_FOLLOW ? (float)((times[k] - times[0]) / 1e9) : 0.0f;
        for (int e = 0; e < 2 && ok; e++) {
            bool f;
            ok = stereo_eye_view_at(e, ev[e].orientation, ev[e].position, ev[e].fov, ahead, plan.Vt[k - 1][e], plan.Pt[k - 1][e], &f);
            fol |= f;
            if (ok) px = fmaxf(px, moved_px(plan.Vs[e], plan.Vt[k - 1][e], plan.Pt[k - 1][e], plan.src[e].extent.width, plan.depth, anchor));
        }
        if (!ok) { st.failed++; continue; }
        if (px < cfg.skip_px) { st.skipped++; continue; }
        plan.want[k - 1] = true, plan.fol[k - 1] = fol, any = true;
    }
    if (any) plan.n = n;
}

// (device.c vk_frame_end, right after the set's own submission, before it goes to the XR thread) each planned refresh's
// images, in a submission of its own: the set's work is done without them, and the XR thread shows each as soon as its
// own is done. Their reads of the set's images are submitted before the set can be released (see the top).
void reproj_submit(void) {
    int n = plan.n;
    plan.n = 0;
    if (n < 2) return;
    bool mv = cfg.mv;
    // (one part for both eyes: the layer names one rect a refresh)
    uint32_t w = plan.w[0] > plan.w[1] ? plan.w[0] : plan.w[1], h = plan.h[0] > plan.h[1] ? plan.h[0] : plan.h[1];
    VkImageView dv[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE}, sv[2] = {plan.src[0].view, plan.src[1].view};
    bool drew = false;
    for (int k = 1; k < n; k++) {
        if (!plan.want[k - 1]) continue;
        XrEyeTarget dst[2];
        if (mv ? !xr_reshow_target(k, -1, &dst[0]) : !xr_reshow_target(k, 0, &dst[0]) || !xr_reshow_target(k, 1, &dst[1])) {
            st.failed++;
            continue;
        }
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);  // (the set's images, written by the submission before)
        uint32_t tq = ts_begin(cb);
        for (int e = 0; e < 2; e++) layout_to(cb, plan.src[e].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        UBlock u[2];
        for (int e = 0; e < 2; e++)
            dv[e] = eye_block(&u[e], plan.Vs[e], plan.Ps[e], plan.Vt[k - 1][e], plan.Pt[k - 1][e], e, plan.depth, plan.floor_y, w, h);
        if (mv) pass(cb, &dst[0], 2, u, sv, dv, w, h);
        else
            for (int e = 0; e < 2; e++) pass(cb, &dst[e], 1, &u[e], &sv[e], &dv[e], w, h);
        // (the set's images back in the layout the runtime takes them in)
        for (int e = 0; e < 2; e++) layout_to(cb, plan.src[e].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ts_end(cb, tq);
        uint64_t seq = vk_pending_seq();
        vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
        xr_reshow_ready(k, seq, w, h);
        st.part += (double)w * h / ((double)dst[0].extent.width * dst[0].extent.height);
        st.drawn++, st.followed += plan.fol[k - 1], st.subs++, drew = true;
    }
    st.sets_drawn += drew;
}
#endif
