// In-between frames for the VR mode (LARECOMP_XR_INBETWEEN=0|1|2, the VR tab's In-between Frames row: Off / On = 1; off
// by default; docs/af/STEREO.md "In-between frames").
//
// At 60 FPS on a 120 Hz display each set of eye images is shown on two refreshes (four at 120 Hz in the 30 FPS hold,
// three at 90 Hz). The second shows the same images for a head that has moved on: the world steps with the head every
// other refresh (the doubling). Here each refresh after a set's first gets eye images of its own: the game frame's
// command lists, as logged by frame interpolation (runtime/nvn/interp.c: its draws and every uniform copy), run again
// through the stereo executor (per-eye cameras or multiview, the Table Edge pass, foveation, dynamic resolution, the
// culls with the new views) for views located for that refresh. Things that move in the world step at 60 (the frame's
// own uniforms); the HUD panel keeps the set's image.
//
// When. The doubling that is left is the change of the head pose's prediction error from one refresh to the next: an
// image predicted further ahead of its display next to one predicted less far is a jump between them. So every
// refresh's image is predicted the same time ahead of its display:
//   1 (On) the set's own images stay its first refresh's. Its views are located for that refresh on the display's grid
//          and the XR thread holds a set finished early (xr_set_grid, as for LARECOMP_XR_REPROJ rot / pos / follow); the
//          in-between for refresh k is located k periods after the set's own views were. As far ahead as the frame's
//          own latch to display (the Steam Frame ~48 ms, the desktop profile ~33 ms).
//   2      every refresh gets an in-between, its first too (the set's own images only where one is missing), each
//          located a lead L before its display: L shorter than the frame's own latch to display. L adapts each half
//          second: down 1 ms when the render thread came to over a tenth of them after their time (L longer than the
//          frame's latch to its present), else up 1 ms when an image was not finished for its refresh (the XR thread's
//          count), else down 0.1 ms (LARECOMP_XR_INBETWEEN_LEAD_MS=<ms>: fixed). One image more a frame.
// They are drawn on the render thread right after the set went to the XR thread (device.c vk_frame_end), the images
// acquired first, each in-between in a submission of its own, and go to the set where it is then, pending or shown
// (xr.h xr_ib_*); the XR thread picks among finished images by display time (xr_loop.c end_frame). The waits for their
// times and the replays count as replay time: the pacer and the game's GPU time leave them out.
//
// Where. Only where they fit, decided from measured times each half second (a [inbetween] line at each change): the
// game frame's GPU time, the in-betweens' (measured while made, else the game frame's times their ratio to it, averaged
// over the half seconds measured) and the compositor's (LARECOMP_XR_INBETWEEN_RESERVE_MS a refresh, 1.5) against the
// frame's refreshes; the render thread's time for the frame (from its views' latch to its set's submission: the waits
// for the game's lists and the GPU in it, as the next frame waits behind them), its waits for the refreshes and the
// replays against the same (the render thread's, the half second's median: a stall is not what an area costs; a half
// second is in one area, begun again at a change). Over in two half seconds in a row: stepped aside, and the area (the
// level and the camera's zone box) noted; back there only once the estimate is under 85% for two seconds and a hold has
// passed: 3 s, doubled for each step aside within 10 s of the last return, 60 s at most. The estimate without them is
// low where the frame's own render-thread time grows with them (the GPU and the queue busier: Mabe on the desktop), so
// such an area gets a try at those growing intervals, each ending within a second; a stall that put them aside where
// they fit (a load, another program) costs only the hold. In an area not noted they come back as soon as it fits. While they are made, dynamic resolution gives each image of the
// frame its share of the frame's refreshes less the compositor's (inbetween_dynres_ms). Stepped aside is the same as
// off: no logging, no grid, no replays; Reprojection (LARECOMP_XR_REPROJ) acts there as set, and nowhere else (exec.c
// stereo_eyes). A frame also gets none when its log is not whole (an overflow, a freed texture), the storage its draws
// read changed, an object it used was freed, its render scale's allocation changed, or a Surroundings capture is under
// way (scene.c).
//   LARECOMP_XR_INBETWEEN_TEST_MS=<ms>[,<from>,<to>] (a test) adds ms to the game frame's measured GPU time and each
//   in-between's (from game frame `from` to `to`, present counts; else always): to see it step aside and come back
//   LARECOMP_XR_INBETWEEN_LOG=1: a [inbetween] line every 5 s with the counts and the times
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../platform.h"
#include "../../rt.h"
#include "../cam_override.h"
#include "../interp.h"
#include "stereo.h"
#include "vk.h"
#ifdef LARECOMP_HAVE_XR
#include "../../xr/xr.h"
#endif

bool inbetween_logging(void);
bool inbetween_takes_set(void);
void inbetween_frame_scale(float sx, float sy, float ax, float ay);
void inbetween_after_submit(void);
void inbetween_set(int on);
double inbetween_dynres_ms(double ms);

#ifndef LARECOMP_HAVE_XR
bool inbetween_logging(void) { return false; }
bool inbetween_takes_set(void) { return false; }
void inbetween_frame_scale(float sx, float sy, float ax, float ay) { (void)sx, (void)sy, (void)ax, (void)ay; }
void inbetween_after_submit(void) {}
void inbetween_set(int on) { (void)on; }
double inbetween_dynres_ms(double ms) { return ms; }
#else
void stereo_replay_views(const XrEyeView ev[2], uint64_t id);
void stereo_replay_views_off(void);
int stereo_inbetween(bool (*eye_target)(int, VkImage *, VkImageView *, VkFormat *, VkExtent2D *), uint64_t *seq, uint32_t filled[2][2]);
void renderer_get_scale(float *sx, float *sy);
void renderer_set_scale(float sx, float sy);
void renderer_get_alloc_scale(float *ax, float *ay);
int scene_ring_phase(void);
uint64_t renderer_exec_frame(void);
uint64_t renderer_present_count(void);
bool vk_gpu_split_ns(uint64_t out[4]);

static struct {
    bool init;
    int mode;  // what on is: 1, or 2 (LARECOMP_XR_INBETWEEN=2)
    double reserve_ms, test_ms, lead_ms;
    long test_from, test_to;
    bool log, force;  // (force: LARECOMP_XR_INBETWEEN_TEST_FORCE=1, a desktop test: made wherever they are wanted, fit or not)
} cfg;
static _Atomic int g_want = -1;  // 0 off, else the mode (-1: the environment's, read once)

static void cfg_init(void) {
    cfg.init = true;
    const char *e = getenv("LARECOMP_XR_INBETWEEN");
    int w = e ? atoi(e) : 0;
    w = w < 0 ? 0 : w > 2 ? 2 : w;
    cfg.mode = w == 2 ? 2 : 1;
    int none = -1;
    atomic_compare_exchange_strong(&g_want, &none, w);  // (the VR tab may have set it already)
    e = getenv("LARECOMP_XR_INBETWEEN_RESERVE_MS");
    cfg.reserve_ms = e ? atof(e) : 1.5;
    e = getenv("LARECOMP_XR_INBETWEEN_TEST_MS");
    cfg.test_from = 0, cfg.test_to = -1;
    if (e) sscanf(e, "%lf,%ld,%ld", &cfg.test_ms, &cfg.test_from, &cfg.test_to);
    e = getenv("LARECOMP_XR_INBETWEEN_LEAD_MS");
    cfg.lead_ms = e ? atof(e) : 0;
    cfg.log = getenv("LARECOMP_XR_INBETWEEN_LOG") != NULL;
    cfg.force = getenv("LARECOMP_XR_INBETWEEN_TEST_FORCE") && atoi(getenv("LARECOMP_XR_INBETWEEN_TEST_FORCE")) > 0;
    if (atomic_load(&g_want) > 0 || cfg.test_ms)
        LOG("[inbetween] %s; compositor's GPU %.1f ms a refresh; test %.1f ms\n",
            atomic_load(&g_want) == 2 ? "every refresh" : atomic_load(&g_want) ? "after the set's first refresh" : "off now", cfg.reserve_ms, cfg.test_ms);
}

void inbetween_set(int on) {
    if (!cfg.init) cfg_init();
    atomic_store(&g_want, on ? cfg.mode : 0);
    LOG("[inbetween] %s (VR settings)\n", on ? "on" : "off");
}

// ---- the rule (once a game frame, at its set's submission)
#define WIN 30
static struct {
    bool engaged;              // in-betweens being made; else stepped aside (or off): as off
    uint64_t frames;           // game frames seen (the test's count)
    uint64_t prev_gpu;
    bool have_prev;
    // this window's (WIN game frames in one area): sums, and the render thread's times a frame (their median counts: a
    // stall, a level's load, is not what the area costs)
    int wn, wib;               // frames, frames with in-betweens made
    double w_gpu, w_ib_gpu, w_test;
    double w_cpu[WIN], w_ib_cpu[WIN];  // the frame's, latch to set; its in-betweens' replays and their waits for the GPU
    int refr;                  // refreshes a frame (the last seen)
    int64_t period;
    int mode;                  // the mode they are made in (engaged)
    double lead_ms;            // (mode 2) L
    uint64_t fell_prev;
    int w_late;                // (mode 2) in-betweens the render thread came to after their time
    int over, under;           // windows in a row over the budget / under 85% of it
    double ratio_gpu, ratio_cpu;  // one in-between's time over the game frame's (measured, averaged over windows; 1 until then)
    uint64_t area, aside_until, back_at, hold_ns;
    uint64_t steps;            // changes so far
    float sx, sy, ax, ay;      // the render scale the frame was drawn at (inbetween_frame_scale)
    bool scale_ok;
} S = {.ratio_gpu = 1, .ratio_cpu = 1, .hold_ns = 3000000000ull, .lead_ms = 30};
static struct { uint64_t sets, made, gone, no_times, no_log, no_storage, freed, scale, capture, failed, late, ahead, stale; double wait_ms, cpu_ms, acq_ms, lock_ms; } st;
#define AREAS 64
static uint64_t g_no_fit[AREAS];  // the areas they stepped aside in (this session)
static int g_no_fit_n;

bool inbetween_logging(void) { return S.engaged; }

// The area: the level and the zone box the camera keeps to (a dungeon's room, an overworld screen's area)
static uint64_t area_now(void) {
    extern const char *game_level_name(void);  // (hle/warp.c)
    extern bool cam_override_zone(float lo[3], float hi[3]);
    const char *lv = game_level_name();
    uint64_t h = 1469598103934665603ull;
    for (const char *c = lv ? lv : ""; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    float lo[3], hi[3];
    if (cam_override_zone(lo, hi))
        for (int i = 0; i < 3; i++) h = (h ^ (uint64_t)(int64_t)lroundf(lo[i]) ^ (uint64_t)(int64_t)lroundf(hi[i]) << 20) * 1099511628211ull;
    return h | 1;
}
static int area_no_fit(uint64_t a) {  // (its index + 1; 0: not noted)
    for (int i = 0; i < g_no_fit_n && i < AREAS; i++) if (g_no_fit[i] == a) return i + 1;
    return 0;
}

static void note_change(bool on, const char *why, double gpu, double ib_gpu, double cpu, double ib_cpu, double budget, int refr) {
    S.engaged = on;
    if (on) S.mode = atomic_load(&g_want);
    xr_set_grid(on ? S.mode : 0);
    S.steps++;
    LOG("[inbetween] %s: %s (game frame GPU %.2f ms, in-betweens %.2f ms, render thread %.2f + %.2f ms; %.2f ms for %d refreshes)\n",
        on ? "made" : "stepped aside", why, gpu, ib_gpu, cpu, ib_cpu, budget, refr);
}

// The frame's measured times into the window; at its end, the rule. In an area they stepped aside in they come back
// only once the estimate is under 85% (and a hold has passed); in another area, as soon as it is under the budget.
static double median(double *v, int n) {
    double t[WIN];
    if (n <= 0) return 0;
    if (n > WIN) n = WIN;
    memcpy(t, v, n * sizeof *t);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && t[j - 1] > t[j]; j--) { double x = t[j]; t[j] = t[j - 1], t[j - 1] = x; }
    return n & 1 ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2;
}

static void rule(uint64_t latch) {
    uint64_t sp[4], now = plat_now_ns();
    uint64_t gpu = vk_gpu_split_ns(sp) ? sp[0] + sp[1] : 0;
    S.frames++;
    if (S.have_prev && gpu >= S.prev_gpu && latch && now > latch && now - latch < 200000000ull) {
        double g = (gpu - S.prev_gpu) / 1e6, c = (now - latch) / 1e6;
        if (cfg.test_ms && (long)S.frames >= cfg.test_from && (cfg.test_to < 0 || (long)S.frames < cfg.test_to))
            g += cfg.test_ms, S.w_test += cfg.test_ms;
        S.w_gpu += g, S.w_cpu[S.wn++] = c;
    }
    S.prev_gpu = gpu, S.have_prev = true;
    S.w_ib_gpu += interp_replay_gpu_take();  // (finished since the last frame: the in-betweens of a frame or two before)
    uint64_t area = area_now();
    if (area != S.area) {  // (the window's frames were the area left: a window of the new one decides for it)
        S.area = area;
        S.wn = S.wib = S.w_late = 0, S.w_gpu = S.w_ib_gpu = S.w_test = 0, S.over = S.under = 0;
        return;
    }
    if (S.wn < WIN) return;
    double n = S.wn, gpu_f = S.w_gpu / n, cpu_f = median(S.w_cpu, S.wn);
    int refr = S.refr > 1 ? S.refr : 2;
    double per = S.period > 0 ? S.period / 1e6 : 1000.0 / 120, budget = refr * per;
    int mode = S.engaged ? S.mode : atomic_load(&g_want), nib = refr - 1 + (mode == 2);  // (in-betweens a frame)
    double ib_gpu, ib_cpu;  // (one in-between)
    if (S.engaged && S.wib >= S.wn / 2) {
        // (the test's time stands for a heavier frame, which each in-between repeats; the ratios are averaged so that
        // one slow window, a stall, does not keep them out of an area afterwards)
        ib_gpu = S.w_ib_gpu / S.wib / nib + S.w_test / n, ib_cpu = median(S.w_ib_cpu, S.wib) / nib;
        if (gpu_f > 0.5) S.ratio_gpu = 0.75 * S.ratio_gpu + 0.25 * ib_gpu / gpu_f;
        if (cpu_f > 0.2) S.ratio_cpu = 0.75 * S.ratio_cpu + 0.25 * ib_cpu / cpu_f;
    } else ib_gpu = S.ratio_gpu * gpu_f, ib_cpu = S.ratio_cpu * cpu_f;
    double need_gpu = gpu_f + nib * ib_gpu + refr * cfg.reserve_ms;
    // (the render thread: the frame, then each in-between after its refresh's wait, all before the next frame's lists;
    // mode 2: the first one may follow the frame directly)
    double need_cpu = fmax(cpu_f, (refr - 1) * per) + (mode == 2 ? 2 : 1) * ib_cpu;
    if (S.engaged && S.mode == 2 && !cfg.lead_ms) {  // (L: see the top)
        uint64_t fell = xr_ib_fell();
        if (S.w_late > S.wn / 10) S.lead_ms -= 1;  // (its time came before the frame's present: none earlier than that)
        else if (fell > S.fell_prev) S.lead_ms += 1;
        else S.lead_ms -= 0.1;
        S.lead_ms = S.lead_ms < 8 ? 8 : S.lead_ms > 60 ? 60 : S.lead_ms;
        S.fell_prev = fell;
    }
    bool want = atomic_load(&g_want) > 0, fits = cfg.force || (need_gpu <= budget && need_cpu <= budget);
    if (!want) {
        if (S.engaged) note_change(false, "turned off", gpu_f, nib * ib_gpu, cpu_f, ib_cpu, budget, refr);
        S.over = S.under = 0;
    } else if (S.engaged) {
        S.over = fits ? 0 : S.over + 1;
        if (S.over >= 2) {
            uint64_t hold = S.back_at && now - S.back_at < 10000000000ull ? S.hold_ns * 2 : 3000000000ull;
            S.hold_ns = hold > 60000000000ull ? 60000000000ull : hold;
            S.aside_until = now + S.hold_ns;
            if (!area_no_fit(area)) g_no_fit[g_no_fit_n++ % AREAS] = area;
            note_change(false, need_gpu > budget ? "the GPU has no time for them here" : "the render thread has no time for them here",
                        gpu_f, nib * ib_gpu, cpu_f, ib_cpu, budget, refr);
            S.over = S.under = 0;
        }
    } else if (!area_no_fit(area)) {
        // (wanted, never made in this area yet: one line per area, so a session where they never fit says so; the
        // headset session of 2026-11-20 had none at all in the overworld and no line to tell)
        static uint64_t waited_area;
        if (!fits && waited_area != area) {
            waited_area = area;
            LOG("[inbetween] waiting: %s (game frame GPU %.2f ms, in-betweens %.2f ms, render thread %.2f + %.2f ms; %.2f ms for %d refreshes)\n",
                need_gpu > budget ? "the GPU has no time for them here" : "the render thread has no time for them here",
                gpu_f, nib * ib_gpu, cpu_f, ib_cpu, budget, refr);
        }
        if (fits) {
            S.back_at = now;
            S.fell_prev = xr_ib_fell();
            note_change(true, S.steps ? "they fit here" : "they fit", gpu_f, nib * ib_gpu, cpu_f, ib_cpu, budget, refr);
        }
        S.over = S.under = 0;
    } else {
        S.under = need_gpu <= 0.85 * budget && need_cpu <= 0.85 * budget ? S.under + 1 : 0;
        if (S.under >= 4 && now >= S.aside_until) {
            S.back_at = now;
            S.fell_prev = xr_ib_fell();
            note_change(true, "they fit here again", gpu_f, nib * ib_gpu, cpu_f, ib_cpu, budget, refr);
            S.over = S.under = 0;
        }
    }
    static uint64_t t_log;
    if (cfg.log && now - t_log > 5000000000ull) {
        t_log = now;
        extern void interp_replay_counts_take(uint64_t *draws, uint64_t *misses, uint64_t *cb_misses);
        uint64_t rd, rm, rc;
        interp_replay_counts_take(&rd, &rm, &rc);
        extern void inbetween_shared_counts_take(uint64_t out[3]);
        uint64_t sk[3];
        inbetween_shared_counts_take(sk);
        LOG("[inbetween] 5 s: %s; sets %llu, in-betweens made %llu, set gone %llu; none: no times %llu, log %llu, storage %llu, "
            "freed %llu, scale %llu, capture %llu, game ahead %llu, failed %llu; late %llu, camera changed %llu; game frame GPU %.2f ms, in-between %.2f ms (x%.2f), "
            "render thread %.2f ms, in-between %.2f ms; need %.2f / %.2f of %.2f ms; waits %.1f ms, replays %.1f ms (images %.1f, "
            "lock %.1f); lead %.1f ms; replayed draws %llu, not found in the frame's log %llu, uniform copies not found %llu; left out (shared targets): draws %llu, clears %llu, copies %llu\n",
            S.engaged ? "made" : want ? "stepped aside" : "off", (unsigned long long)st.sets, (unsigned long long)st.made,
            (unsigned long long)st.gone, (unsigned long long)st.no_times, (unsigned long long)st.no_log, (unsigned long long)st.no_storage,
            (unsigned long long)st.freed, (unsigned long long)st.scale, (unsigned long long)st.capture, (unsigned long long)st.ahead,
            (unsigned long long)st.failed, (unsigned long long)st.late, (unsigned long long)st.stale, gpu_f, ib_gpu, S.ratio_gpu, cpu_f, ib_cpu, need_gpu, need_cpu, budget, st.wait_ms, st.cpu_ms,
            st.acq_ms, st.lock_ms, S.mode == 2 ? (cfg.lead_ms ? cfg.lead_ms : S.lead_ms) : 0.0, (unsigned long long)rd,
            (unsigned long long)rm, (unsigned long long)rc, (unsigned long long)sk[0], (unsigned long long)sk[1], (unsigned long long)sk[2]);
        memset(&st, 0, sizeof st);
    }
    S.wn = S.wib = S.w_late = 0, S.w_gpu = S.w_ib_gpu = S.w_test = 0;
}

// (device.c, VR dynamic resolution) the game frame's GPU budget: while in-betweens are made each image of the frame
// gets its share of the frame's refreshes, less the compositor's
double inbetween_dynres_ms(double ms) {
    if (!S.engaged || S.period <= 0 || S.refr < 2) return ms;
    double img = S.refr * (S.period / 1e6 - cfg.reserve_ms) / (S.refr + (S.mode == 2));
    return img < ms ? img : ms;
}

void inbetween_frame_scale(float sx, float sy, float ax, float ay) {
    S.sx = sx, S.sy = sy, S.ax = ax, S.ay = ay, S.scale_ok = true;
}

// (exec.c stereo_eyes: the set's eyes drawn and its views set) the in-betweens make the set's later refreshes: the set
// placed on the display's grid (its refreshes' times, xr_reshow_times), Reprojection not run for it
bool inbetween_takes_set(void) {
    if (!S.engaged) return false;
    int64_t t[4];
    xr_reshow_times(t);
    return true;
}

// ---- the in-betweens of the set just submitted
static int g_k;
static VkExtent2D g_ext;
static bool ib_target(int eye, VkImage *img, VkImageView *view, VkFormat *fmt, VkExtent2D *ext) {
    XrEyeTarget t;
    uint64_t t0 = plat_now_ns();
    bool ok = xr_ib_target(g_k, eye, &t);
    st.acq_ms += (plat_now_ns() - t0) / 1e6;
    if (!ok) return false;
    *img = t.image, *view = t.view, *fmt = t.format, *ext = t.extent;
    g_ext = t.extent;
    return true;
}

void inbetween_after_submit(void) {
    if (!cfg.init) cfg_init();
    int64_t times[4];
    uint64_t latch = 0;
    bool hidden = false;
    int n = xr_ib_due(times, &latch, &hidden);  // (refresh k of times gets in-between k, 1 .. n - 1)
    if (n > 1) S.refr = n - hidden, S.period = times[1] - times[0];
    bool made = S.engaged;  // (the rule's decision applies from the next frame: this one was logged, or not, by the last)
    rule(latch);
    if (!made) return;
    st.sets++;
    if (n < 2) { st.no_times++; return; }
    if (!interp_last_logged()) { st.no_log++; return; }
    if (scene_ring_phase() != 0) { st.capture++; return; }  // (scene.c: a Surroundings capture's frames are its own)
    float sx, sy, ax, ay;
    renderer_get_scale(&sx, &sy);
    renderer_get_alloc_scale(&ax, &ay);
    if (!S.scale_ok || ax != S.ax || ay != S.ay) { st.scale++; return; }
    int64_t per = times[1] - times[0];
    uint64_t frame = renderer_exec_frame();  // (this present's game frame)
    double cpu = 0, wait = 0;
    int got = 0;
    interp_replay_last(true);
    int64_t lead = (int64_t)((cfg.lead_ms ? cfg.lead_ms : S.lead_ms) * 1e6);
    // (the images first, before the waits: acquiring takes the queue's lock, which the XR thread's frame end holds while
    // the runtime waits for the GPU)
    {
        uint64_t t0 = plat_now_ns();
        for (int k = 1; k < n; k++)
            for (int e = 0; e < 2; e++) {
                VkImage i;
                VkImageView v;
                VkFormat f;
                VkExtent2D x;
                g_k = k, ib_target(e, &i, &v, &f, &x);
            }
        wait += (plat_now_ns() - t0) / 1e6;
    }
    for (int k = 1; k < n; k++) {
        // (mode 1: as long after the set's latch as its refresh is after the set's first; 2: L before its display)
        uint64_t at = hidden ? xr_plat_time(times[k] - lead) : latch + (uint64_t)k * (uint64_t)per, t0 = plat_now_ns();
        if (t0 > at + (hidden ? 1000000 : (uint64_t)per / 2)) st.late++, S.w_late++;  // (located later: predicted less far ahead)
        interp_replay_sleep_until(at);
        uint64_t t1 = plat_now_ns();
        wait += (t1 - t0) / 1e6;
        // (the lock for each replay, not over the waits: the game thread's frees wait for it)
        bool locked = interp_replay_lock();
        st.lock_ms += (plat_now_ns() - t1) / 1e6;
        if (!locked) { st.freed++; break; }
        if (!interp_storage_same()) { st.no_storage++, interp_replay_unlock(); break; }
        // (the game thread two frames on: its memory of this frame's parity is being written - the cameras the
        // replay reads, its geometry)
        if (renderer_present_count() >= frame + 2) { st.ahead++, interp_replay_unlock(); break; }
        XrEyeView ev[2];
        uint64_t id;
        if (!xr_ib_views(k, ev, &id)) { st.failed++, interp_replay_unlock(); break; }
        stereo_replay_views(ev, id);
        renderer_set_scale(S.sx, S.sy);
        g_k = k, g_ext = (VkExtent2D){0, 0};
        uint64_t seq;
        uint32_t filled[2][2] = {{0}};
        uint32_t stale = g_cam_stale;
        int drawn = stereo_inbetween(ib_target, &seq, filled);
        st.stale += g_cam_stale != stale;  // (a camera changed since the replay's latch: the game's own drawn there)
        renderer_set_scale(sx, sy);
        stereo_replay_views_off();
        interp_replay_unlock();
        cpu += (plat_now_ns() - t1) / 1e6;
        // (both eyes drawn, or what was acquired goes back: a size of 0)
        if (xr_ib_ready(k, seq, drawn == 3 ? g_ext.width : 0, g_ext.height)) st.made++, got++;
        else if (drawn != 3) st.failed++;
        else st.gone++;
    }
    for (int k = 1; k < n; k++) xr_ib_ready(k, 0, 0, 0);  // (those not made after a break: their images back)
    interp_replay_last(false);
    st.wait_ms += wait, st.cpu_ms += cpu;
    if (got && S.wib < WIN) S.w_ib_cpu[S.wib++] = cpu;
}
#endif
