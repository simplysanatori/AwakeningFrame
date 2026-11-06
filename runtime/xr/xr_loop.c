// The XR thread: one xrWaitFrame / xrBeginFrame / xrEndFrame per display refresh, the game's vsync from it, and the
// hand-off of the presenting thread's sets (GalaxyFrame's showPendingVk / settleVk / finishFrameVk, reduced: LAS can't
// draw a frame again at each refresh, so the loop only shows sets; see xr.h).
#define _GNU_SOURCE  // (pthread_setname_np)
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../paths.h"
#include "../rt.h"
#include "../nvn/pace.h"
#include "xr_internal.h"

void vi_external_vsync(uint64_t now_ns);  // runtime/hle/vi.c

#define GAME_PERIOD_NS 16666667ll  // the game's vsync: 60 Hz

static struct {  // (XR thread; the presenting thread's fields under g_xr.mu)
    uint64_t refreshes, missed, vsyncs, released, gpu_late, reshown, no_render;
    uint64_t waits, wait_ns, skipped;  // presenting thread: xr_set_begin waits for the last set's release
    uint64_t latency_ns;               // set submitted -> released
} st;

bool xr_active(void) { return g_xr.thread_on && g_xr.session; }
int64_t xr_target_display_time(void) { return atomic_load(&g_xr.target_time); }
float xr_display_hz(void) { return g_xr.display_hz; }

// ---- the presenting thread
bool xr_set_begin(uint64_t timeout_ns, uint32_t *hud_index) {
    if (!xr_active() || g_xr.in_set) return false;
    uint64_t t0 = plat_now_ns(), dl = t0 + timeout_ns;
    pthread_mutex_lock(&g_xr.mu);
    bool waited = false;
    while (g_xr.pending.valid && !g_xr.stop) {
        waited = true;
        // (plat_now_ns is CLOCK_MONOTONIC; the condition variable waits on CLOCK_REALTIME: re-armed in short steps)
        uint64_t now = plat_now_ns();
        if (now >= dl) break;
        struct timespec rt;
        clock_gettime(CLOCK_REALTIME, &rt);
        uint64_t step = dl - now > 2000000ull ? 2000000ull : dl - now;
        uint64_t at = (uint64_t)rt.tv_sec * 1000000000ull + (uint64_t)rt.tv_nsec + step;
        struct timespec ts = {(time_t)(at / 1000000000ull), (long)(at % 1000000000ull)};
        pthread_cond_timedwait(&g_xr.cv, &g_xr.mu, &ts);
    }
    bool busy = g_xr.pending.valid;
    int pair = g_xr.shown_any ? 1 - g_xr.shown.pair : 0;  // (the pair no layer names now; shown changes only at a release)
    if (waited) st.waits++, st.wait_ns += plat_now_ns() - t0;
    if (busy) st.skipped++;
    pthread_mutex_unlock(&g_xr.mu);
    if (busy || g_xr.stop) return false;
    uint32_t idx;
    if (!xr_chain_acquire(&g_xr.hud[pair], &idx)) return false;
    memset(&g_xr.building, 0, sizeof g_xr.building);
    if (g_xr.early_ok) {  // (the stereo executor rendered this frame's eyes with these: xr_latch_views_early)
        memcpy(g_xr.building.views, g_xr.early_views, sizeof g_xr.building.views);
        g_xr.building.space = g_xr.early_space;
        g_xr.building.views_ok = true;
        g_xr.early_ok = false;
    }
    g_xr.building.pair = pair;
    g_xr.building.hud = idx;
    g_xr.in_set = true;
    *hud_index = (pair ? g_xr.hud[0].count : 0) + idx;  // (XrChainInfo: the first pair's images, then the second's)
    return true;
}

void xr_set_submit(uint64_t seq) {
    if (!g_xr.in_set) return;
    XrSet *s = &g_xr.building;
    if (!s->views_ok) xr_latch_views(NULL);
    s->seq = seq;
    s->made_ns = plat_now_ns();
    s->valid = true;
    pthread_mutex_lock(&g_xr.mu);
    g_xr.pending = *s;
    pthread_mutex_unlock(&g_xr.mu);
    g_xr.in_set = false;
}

// ---- the XR thread
// The pending set, once the GPU finished its submission: its images released, and it becomes the one shown. Also
// while the session isn't running (no frames), so the presenting thread never waits on a display that isn't there.
void xr_release_completed(void) {
    pthread_mutex_lock(&g_xr.mu);
    if (g_xr.pending.valid) {
        if (vk_completed_seq() >= g_xr.pending.seq) {
            XrSet s = g_xr.pending;
            xr_chain_release(&g_xr.hud[s.pair]);
            for (int i = 0; i < 2; i++) if (s.has_eye[i]) xr_chain_release(&g_xr.eyes[s.pair][i]);
            g_xr.shown = s;
            g_xr.shown_any = true;
            g_xr.pending.valid = false;
            st.released++;
            st.latency_ns += plat_now_ns() - s.made_ns;
            pthread_cond_broadcast(&g_xr.cv);
        } else {
            st.gpu_late++;
        }
    }
    pthread_mutex_unlock(&g_xr.mu);
}

static _Atomic int g_gpu_us[2] = {-1, -1};  // (xr_note_eye_gpu) microseconds
void xr_note_eye_gpu(float ms0, float ms1) {
    atomic_store(&g_gpu_us[0], ms0 < 0 ? -1 : (int)(ms0 * 1000.0f));
    atomic_store(&g_gpu_us[1], ms1 < 0 ? -1 : (int)(ms1 * 1000.0f));
}

// xr_timing.log in the data folder (the previous session's as xr_timing.log.prev): one line per refresh, ms since
// xrBeginSession on plat_now_ns's clock (docs/af/TOOLING.md; tools/session-report.py reads it). GPU times per eye are
// GPU ms per eye come from the stereo executor (xr_note_eye_gpu; -1 while unknown).
static FILE *tlog;
static void timing_open(void) {
    char p[1024], q[1040];
    snprintf(p, sizeof p, "%s/xr_timing.log", rt_data_root());
    snprintf(q, sizeof q, "%s.prev", p);
    rename(p, q);
    tlog = fopen(p, "w");
    if (!tlog) LOG("[xr] cannot write %s\n", p);
}
static double time_ms(XrTime t) {  // an XrTime on xr_timing.log's clock
    struct timespec ts;
    uint64_t ns = (uint64_t)t;  // (SteamVR's and the mock's XrTime are CLOCK_MONOTONIC ns already)
    if (p_xrConvertTimeToTimespecTimeKHR && XR_SUCCEEDED(p_xrConvertTimeToTimespecTimeKHR(g_xr.inst, t, &ts)))
        ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return ((double)ns - (double)g_xr.begin_ns) / 1e6;
}
static void timing_line(uint64_t f, uint64_t t_wait0, uint64_t t_ret, const XrFrameState *fs, uint64_t t_begin, uint64_t t_end, bool late) {
    if (!tlog) return;
    time_t wall = time(NULL);
    struct tm tm;
    localtime_r(&wall, &tm);
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    double b = (double)g_xr.begin_ns;
    fprintf(tlog, "%02d:%02d:%02d.%03ld xrt f=%llu t=%.3f pdt=%.3f period=%.3f wait=%.3f begin=%.3f end=%.3f gpu0=%.3f gpu1=%.3f late=%d\n",
            tm.tm_hour, tm.tm_min, tm.tm_sec, rt.tv_nsec / 1000000, (unsigned long long)f, (t_ret - b) / 1e6,
            time_ms(fs->predictedDisplayTime), fs->predictedDisplayPeriod / 1e6, (t_ret - t_wait0) / 1e6, (t_begin - b) / 1e6,
            (t_end - b) / 1e6, atomic_load(&g_gpu_us[0]) / 1000.0, atomic_load(&g_gpu_us[1]) / 1000.0, late);
}

static void end_frame(const XrFrameState *fs, bool new_set) {
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection proj = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad = {.type = XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t n = 0;
    if (fs->shouldRender && g_xr.shown_any) {
        const XrSet *s = &g_xr.shown;
        if (s->views_ok && s->has_eye[0] && s->has_eye[1]) {
            for (int i = 0; i < 2; i++)
                pv[i] = (XrCompositionLayerProjectionView){
                    .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = s->views[i].pose, .fov = s->views[i].fov,
                    .subImage = {g_xr.eyes[s->pair][i].sc, {{0, 0}, {(int32_t)g_xr.eyes[s->pair][i].w, (int32_t)g_xr.eyes[s->pair][i].h}}, 0}};
            proj.space = s->space;
            proj.viewCount = 2;
            proj.views = pv;
            layers[n++] = (const XrCompositionLayerBaseHeader *)&proj;
        }
        // the HUD: a flat screen straight ahead at eye height, LARECOMP_XR_QUAD="distance,width" (2 m, 2.2 m)
        const XrChain *hc = &g_xr.hud[s->pair];
        float w = g_xr.quad_width, h = w * (float)hc->h / (float)hc->w;
        quad.space = g_xr.app;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage = (XrSwapchainSubImage){hc->sc, {{0, 0}, {(int32_t)hc->w, (int32_t)hc->h}}, 0};
        quad.pose = (XrPosef){.orientation = {0, 0, 0, 1}, .position = {0, 0, -g_xr.quad_dist}};
        quad.size = (XrExtent2Df){w, h};
        layers[n++] = (const XrCompositionLayerBaseHeader *)&quad;
        if (!new_set) st.reshown++;
    } else {
        st.no_render++;
    }
    XrFrameEndInfo ei = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = fs->predictedDisplayTime,
                         .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = n, .layers = layers};
    vk_queue_lock();
    XrResult r = XR_RT(p_xrEndFrame(g_xr.session, &ei));
    vk_queue_unlock();
    xr_ok_(r, "xrEndFrame");
}

static void log_stats(double secs) {
    pthread_mutex_lock(&g_xr.mu);
    LOG("[xr] %.0f s: %llu refreshes at %.0f Hz (%llu missed), %llu vsyncs, %llu sets shown (%.1f ms after submission), "
        "%llu refreshes re-showed a set, %llu waited for the GPU, %llu without layers; presenting: %llu waits (%.2f ms avg), "
        "%llu frames skipped\n", secs, (unsigned long long)st.refreshes, g_xr.display_hz, (unsigned long long)st.missed,
        (unsigned long long)st.vsyncs, (unsigned long long)st.released, st.released ? st.latency_ns / 1e6 / st.released : 0.0,
        (unsigned long long)st.reshown, (unsigned long long)st.gpu_late, (unsigned long long)st.no_render,
        (unsigned long long)st.waits, st.waits ? st.wait_ns / 1e6 / st.waits : 0.0, (unsigned long long)st.skipped);
    memset(&st, 0, sizeof st);
    pthread_mutex_unlock(&g_xr.mu);
}

static void *xr_thread(void *arg) {
    (void)arg;
    plat_thread_realtime(8333333);
    plat_thread_perf_core(PLAT_PERF_VSYNC);
    int64_t last_d = 0, acc = 0;
    uint64_t frame_no = 0;
    uint64_t stats_at = plat_now_ns();
    while (!g_xr.stop) {
        bool quit = false;
        xr_poll_events(&quit);
        if (quit) {
            LOG("[xr] the runtime ends the session: quitting\n");
            xr_request_app_quit();
            break;
        }
        if (!g_xr.running) {
            xr_release_completed();
            last_d = 0;
            plat_sleep_until_ns(plat_now_ns() + 10000000ull);
            continue;
        }
        XrFrameState fs = {.type = XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo wi = {.type = XR_TYPE_FRAME_WAIT_INFO};
        if (!tlog) timing_open();
        uint64_t t_wait0 = plat_now_ns();
        if (!XR_OK(p_xrWaitFrame(g_xr.session, &wi, &fs))) {
            plat_sleep_until_ns(plat_now_ns() + 5000000ull);
            continue;
        }
        uint64_t now = plat_now_ns();
        int64_t period = fs.predictedDisplayPeriod > 0 ? fs.predictedDisplayPeriod : 8333333;
        if (!g_xr.display_hz || g_xr.display_hz * period < 0.99e9 || g_xr.display_hz * period > 1.01e9) g_xr.display_hz = 1e9f / (float)period;
        {   // below 100 Hz (90): the game held in its 30 fps mode (pace.c pace_xr_lock); 120 Hz: 60 as the pacer decides
            static int locked = -1;
            int want = g_xr.display_hz < 100 ? 30 : 0;
            if (want != locked) {
                pace_xr_lock(want);
                LOG("[xr] display at %.0f Hz: the game %s\n", g_xr.display_hz, want ? "held at 30 fps (3 refreshes a frame)" : "at 60 fps (2 refreshes a frame)");
                locked = want;
            }
        }
        // The game's 60 Hz vsync from the display's refreshes: every second one at 120 Hz, two of three at 90 Hz (by
        // display time, so a missed refresh doesn't slow the game down). The frame starting now is shown two refreshes on.
        int64_t step = last_d ? fs.predictedDisplayTime - last_d : period;
        if (last_d && step > period * 3 / 2) st.missed += (uint64_t)((step + period / 2) / period) - 1;
        if (step > 100000000) step = period;  // (a pause: start over)
        last_d = fs.predictedDisplayTime;
        st.refreshes++;
        acc += step;
        if (acc >= GAME_PERIOD_NS - 500000) {
            acc -= GAME_PERIOD_NS;
            if (acc > GAME_PERIOD_NS) acc = 0;
            atomic_store(&g_xr.target_time, fs.predictedDisplayTime + 2 * period);
            vi_external_vsync(now);
            st.vsyncs++;
        }
        XrFrameBeginInfo bi = {.type = XR_TYPE_FRAME_BEGIN_INFO};
        uint64_t t_begin = plat_now_ns();
        vk_queue_lock();
        XrResult r = XR_RT(p_xrBeginFrame(g_xr.session, &bi));
        vk_queue_unlock();
        if (XR_FAILED(r)) { xr_ok_(r, "xrBeginFrame"); continue; }
        xr_update_app_space(fs.predictedDisplayTime);
        uint64_t before = st.released, late_before = st.gpu_late;
        xr_release_completed();
        uint64_t t_end = plat_now_ns();
        end_frame(&fs, st.released != before);
        timing_line(frame_no++, t_wait0, now, &fs, t_begin, t_end, st.gpu_late != late_before);
        if (now - stats_at >= 5000000000ull) {
            if (tlog) fflush(tlog);
            log_stats((now - stats_at) / 1e9);
            stats_at = now;
        }
    }
    return NULL;
}

bool xr_thread_start(void) {
    g_xr.stop = false;
    if (pthread_create(&g_xr.thread, NULL, xr_thread, NULL) != 0) return false;
    pthread_setname_np(g_xr.thread, "xr");
    g_xr.thread_on = true;
    return true;
}

void xr_thread_stop(void) {
    if (!g_xr.thread_on) return;
    g_xr.stop = true;
    pthread_mutex_lock(&g_xr.mu);
    pthread_cond_broadcast(&g_xr.cv);
    pthread_mutex_unlock(&g_xr.mu);
    pthread_join(g_xr.thread, NULL);
    g_xr.thread_on = false;
}

// ---- start / stop (device.c)
bool xr_start(VkQueue queue, uint32_t qfam, uint32_t hud_w, uint32_t hud_h, XrChainInfo *hud) {
    g_xr.queue = queue;
    g_xr.qfam = qfam;
    if (!xr_session_create() || !xr_chains_create(hud_w, hud_h)) return false;
    memset(hud, 0, sizeof *hud);
    hud->format = g_xr.hud[0].view_fmt;
    hud->extent = (VkExtent2D){g_xr.hud[0].w, g_xr.hud[0].h};
    for (int p = 0; p < 2; p++)
        for (uint32_t i = 0; i < g_xr.hud[p].count; i++, hud->count++)
            hud->images[hud->count] = g_xr.hud[p].img[i], hud->views[hud->count] = g_xr.hud[p].view[i];
    if (!xr_thread_start()) return false;
    LOG("[xr] VR mode on: HUD %ux%u on a quad %.1f m wide at %.1f m\n", hud_w, hud_h, g_xr.quad_width, g_xr.quad_dist);
    return true;
}

void xr_shutdown(void) {
    static bool done;
    if (done || !g_xr.inst) return;
    done = true;
    xr_thread_stop();
    if (tlog) fclose(tlog), tlog = NULL;
    xr_drain_gpu("swapchain destruction");
    xr_chains_destroy();
    xr_session_destroy();
    LOG("[xr] session and instance destroyed\n");
}
