// The XR thread: one xrWaitFrame / xrBeginFrame / xrEndFrame per display refresh, the game's vsync from it, and the
// hand-off of the presenting thread's sets (GalaxyFrame's showPendingVk / settleVk / finishFrameVk, reduced: LAS can't
// draw a frame again at each refresh, so the loop only shows sets; see xr.h).
#define _GNU_SOURCE  // (pthread_setname_np)
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../paths.h"
#include "../rt.h"
#include "../nvn/pace.h"
#include "xr_input.h"
#include "xr_pointer.h"
#include "xr_internal.h"
#include "hz_filter.h"
#include "../nvn/vulkan/edge.h"

void vi_external_vsync(uint64_t now_ns);  // runtime/hle/vi.c

static struct {  // (XR thread; the presenting thread's fields under g_xr.mu)
    uint64_t refreshes, missed, vsyncs, released, gpu_late, reshown, no_render;
    uint64_t waits, wait_ns, skipped;  // presenting thread: xr_set_begin waits for the last set's release
    uint64_t latency_ns;               // set submitted -> released
    // the pose check: refreshes whose projection layer did not carry the poses its eye images were rendered with
    // (the executor's eyes only; stub eyes have no render pose), executor sets made with eyes of two latches, and
    // sets shown for the first time: their latch -> display and located-for -> display times
    uint64_t pose_ok, pose_bad, eye_split, firsts;
    int64_t lat_sum_ns, vt_err_sum_ns, vt_err_max_ns;
    // the frame end: the queue lock's wait and the runtime's xrEndFrame (SteamVR's waits behind the queue's backlog)
    uint64_t end_n, end_slow, lock_slow, end_sum_ns, end_max_ns;
    // (LARECOMP_XR_REPROJ) re-shown refreshes: with an image drawn for them, without one (none due, skipped as too
    // small a change, or past the last due: a late next set), and (fresh) with a pose located for them; past: shown later
    // than the last refresh drawn for (the image of that one, a period or more old); fell: the image drawn for it not
    // finished by then (the newest finished one shown); a set's first refresh on the display time its poses were
    // located for, a period or more later, or earlier; held: refreshes a finished set waited for its own
    // (xr_release_completed); rs_ahead: the drawn images found finished 2 or more, 1, 0 refreshes before the one
    // they are for, or after it
    uint64_t rs_drawn, rs_none, rs_fresh, rs_past, rs_fell, first_on, first_late, first_early, held, rs_ahead[4];
    // (LARECOMP_XR_BFI) refreshes lit and dark; sets lit on the refresh their poses were located for, later (late: the
    // first lit refresh after it), replaced before they were lit; lit refreshes with no new set: left dark (a flash
    // skipped), the set before lit again (the late test, and a second in a row: a stall); flashes 1, 2, 3, 4, 5+ refreshes
    // after the one before; the lit refreshes' phase moved (a lead grown by one)
    uint64_t bfi_lit, bfi_dark, bfi_on, bfi_late, bfi_unlit, bfi_skip, bfi_relit, bfi_stall, bfi_gap[5], bfi_rephase;
} st;

// LARECOMP_XR_REPROJ=off|fresh|rot|pos|follow (xr.h; docs/af/STEREO.md "Re-shown refreshes"); the VR tab sets it live
static int g_reproj = -1;
static void reproj_init(void) {
    if (__atomic_load_n(&g_reproj, __ATOMIC_ACQUIRE) >= 0) return;
    const char *e = getenv("LARECOMP_XR_REPROJ");
    int m = XR_REPROJ_OFF;
    if (e && *e) {
        if (!strcmp(e, "fresh")) m = XR_REPROJ_FRESH;
        else if (!strcmp(e, "rot")) m = XR_REPROJ_ROT;
        else if (!strcmp(e, "pos")) m = XR_REPROJ_POS;
        else if (!strcmp(e, "follow")) m = XR_REPROJ_FOLLOW;
        else if (strcmp(e, "off") && strcmp(e, "0")) LOG("[xr] LARECOMP_XR_REPROJ=%s unknown (off, fresh, rot, pos, follow): off\n", e);
    }
    int expect = -1;
    if (__atomic_compare_exchange_n(&g_reproj, &expect, m, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && m)
        LOG("[xr] re-shown refreshes: %s\n", m == XR_REPROJ_FRESH ? "a fresh pose on the same images (a test)"
                                          : m == XR_REPROJ_ROT ? "drawn again for their head pose (rotation)"
                                          : m == XR_REPROJ_POS ? "drawn again for their head pose (rotation and position)"
                                                               : "drawn again for their head pose and the table's follow");
}
static _Atomic int g_bfi_armed;  // (xr_bfi_armed; the XR thread's)
int xr_reproj_mode(void) {
    reproj_init();
    // (Motion Clarity armed: the refreshes a redraw would be for show nothing, so none is made and their swapchains
    // are never created)
    if (atomic_load(&g_bfi_armed)) return XR_REPROJ_OFF;
    return __atomic_load_n(&g_reproj, __ATOMIC_ACQUIRE);
}
void xr_set_reproj_mode(int mode) {
    if (mode < XR_REPROJ_OFF || mode > XR_REPROJ_FOLLOW) return;
    reproj_init();
    if (__atomic_exchange_n(&g_reproj, mode, __ATOMIC_ACQ_REL) != mode) LOG("[xr] re-shown refreshes: mode %d (VR settings)\n", mode);
}

// LARECOMP_XR_BFI=0|1, Motion Clarity (xr.h; docs/af/STEREO.md "Motion Clarity"); the VR tab sets it live. Armed (the XR
// thread's, each refresh): wanted, at a rate that shows a 60 FPS frame on two refreshes. Flashing: armed, with the game at
// 60 (not the pacer's 30 FPS hold or the FPS target's 30, noted by the presenting thread with each set: g_bfi_game30)
// and sets coming for most lit refreshes (the watchdog, bfi_step).
static int g_bfi_want = -1;
static _Atomic int g_bfi_game30, g_bfi_state;
static void bfi_init(void) {
    if (__atomic_load_n(&g_bfi_want, __ATOMIC_ACQUIRE) >= 0) return;
    const char *e = getenv("LARECOMP_XR_BFI");
    int on = e && atoi(e) != 0, expect = -1;
    if (__atomic_compare_exchange_n(&g_bfi_want, &expect, on, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && on)
        LOG("[xr] motion clarity: on (LARECOMP_XR_BFI)\n");
}
static bool bfi_wanted(void) {
    bfi_init();
    return __atomic_load_n(&g_bfi_want, __ATOMIC_ACQUIRE) > 0;
}
void xr_set_bfi(int on) {
    bfi_init();
    if (__atomic_exchange_n(&g_bfi_want, on ? 1 : 0, __ATOMIC_ACQ_REL) != (on ? 1 : 0)) LOG("[xr] motion clarity: %s (VR settings)\n", on ? "on" : "off");
}
bool xr_bfi_armed(void) { return atomic_load(&g_bfi_armed); }
int xr_bfi_state(void) { return bfi_wanted() ? atomic_load(&g_bfi_state) : 0; }
float xr_bfi_gain(void) {
    static float g = -1;
    if (g < 0) {
        const char *e = getenv("LARECOMP_XR_BFI_GAIN");
        g = e ? (float)atof(e) : 0;
        if (g <= 1.0f) g = 0;
        if (g > 2.0f) g = 2.0f;
    }
    return g > 0 && xr_bfi_state() == 1 ? g : 0;
}

static _Atomic int g_per_frame = 2;  // (xr_refreshes_per_frame: the XR thread's cadence rule)
int xr_refreshes_per_frame(void) { return atomic_load(&g_per_frame); }

bool xr_active(void) { return g_xr.thread_on && g_xr.session; }
bool xr_session_running(void) { return g_xr.running; }
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
    uint64_t wait_ns = waited ? plat_now_ns() - t0 : 0;
    if (waited) st.waits++, st.wait_ns += wait_ns;
    if (busy) st.skipped++;
    pthread_mutex_unlock(&g_xr.mu);
    if (busy || g_xr.stop) return false;
    uint32_t idx;
    if (!xr_chain_acquire(&g_xr.hud[pair], &idx)) return false;
    memset(&g_xr.building, 0, sizeof g_xr.building);  // (views: xr_set_views, from the eye images' latches)
    g_xr.building.pair = pair;
    g_xr.building.hud = idx;
    g_xr.building.wait_ns = wait_ns;
    g_xr.in_set = true;
    *hud_index = (pair ? g_xr.hud[0].count : 0) + idx;  // (XrChainInfo: the first pair's images, then the second's)
    return true;
}

void xr_set_submit(uint64_t seq) {
    if (!g_xr.in_set) return;
    XrSet *s = &g_xr.building;
    if (!s->views_ok) xr_latch_views(NULL);  // (no eye images of the executor: stub eyes or none)
    s->seq = seq;
    s->alpha = edge_passthrough_wanted();  // (the mode its eye images were just drawn in)
    s->made_ns = plat_now_ns();
    s->valid = true;
    if (bfi_wanted()) {
        // (Motion Clarity) the refresh it is lit on: the one its poses were located for, on the display's grid when armed
        // (predict_time). And whether the game runs at 30 now: the pacer's hold, or the FPS target's (pace.c)
        int64_t g = atomic_load(&g_xr.grid_t), per = xr_bfi_period();
        if (xr_bfi_armed() && !s->grid0 && s->views_ok && s->vtime && g && per > 0) {
            int64_t d = s->vtime - g, n = d >= 0 ? (d + per / 2) / per : -((-d + per / 2) / per);
            s->grid0 = g + n * per, s->period = per;
        }
        extern bool pace_target_30(void);
        atomic_store(&g_bfi_game30, pace_is_slow() || pace_target_30());
    }
    pthread_mutex_lock(&g_xr.mu);
    if (s->exec_eyes && s->rid[0] != s->rid[1]) st.eye_split++;
    g_xr.pending = *s;
    pthread_mutex_unlock(&g_xr.mu);
    g_xr.in_set = false;
}

// ---- the XR thread
static int64_t periods(int64_t d, int64_t per) { return d >= 0 ? (d + per / 2) / per : -((-d + per / 2) / per); }

// (LARECOMP_XR_REPROJ) a finished set's first possible refresh, `pdt`: its lead, whole periods from its latch's vsync
// target, into the last XR_LEADS; the prediction's lead is their latest (predict_time)
static void lead_note(const XrSet *s, XrTime pdt) {
    int64_t per = atomic_load(&g_xr.grid_period);
    if (!s->vref || per <= 0) return;
    // (LARECOMP_XR_BFI, armed: less the time its present waited for the set before, held for its own refresh. Without
    // that, a phase a refresh later than the sets need holds every set, the next present waits for it, and its own lead
    // grows to match: the extra refresh never goes, and in a heavy scene the waits grow until the pacer holds 30)
    static int down = -1;
    if (down < 0) down = getenv("LARECOMP_XR_BFI_PHASE_DOWN") ? atoi(getenv("LARECOMP_XR_BFI_PHASE_DOWN")) : 32;
    bool bfi = xr_bfi_armed();
    int64_t m = periods(pdt - s->vref - (bfi ? (int64_t)s->wait_ns : 0), per);
    if (m < -4 || m > 12) return;  // (a hitch or a load, not the pipeline)
    g_xr.leads[g_xr.lead_n++ % XR_LEADS] = (int8_t)m;
    if (g_xr.lead_n < XR_LEADS / 2) return;
    int n = g_xr.lead_n < XR_LEADS ? g_xr.lead_n : XR_LEADS, lead = g_xr.leads[0];
    for (int i = 1; i < n; i++) if (g_xr.leads[i] > lead) lead = g_xr.leads[i];
    int old = atomic_exchange(&g_xr.lead, lead);
    // (LARECOMP_XR_BFI) the predictions are moved later onto the lit refreshes' phase (predict_time). The phase is first
    // taken from the lead. A lead grown onto the other phase moves it one refresh later at once (one gap of three
    // refreshes between two flashes), so the sets are not late on every refresh. A lead shrunk onto the other phase
    // moves it only once it has stayed there for LARECOMP_XR_BFI_PHASE_DOWN sets (32, half a second; 0: never): until
    // then each set is held a refresh longer than it needs and its present waits for the one before, and the move puts
    // one set's refresh right after a flash, so that set is lit a refresh late or not at all
    int64_t ph = atomic_load(&g_xr.bfi_phase), bp = xr_bfi_period();
    if (!bfi) return;
    if (!ph) {
        atomic_store(&g_xr.bfi_phase, s->vref + lead * bp), g_xr.bfi_off = 0;
    } else if (!(periods(s->vref + lead * bp - ph, bp) & 1)) {
        g_xr.bfi_off = 0;
    } else if ((old != XR_LEAD_NONE && lead > old) || (down > 0 && ++g_xr.bfi_off >= down)) {
        atomic_store(&g_xr.bfi_phase, ph + bp), g_xr.bfi_off = 0, st.bfi_rephase++;
    }
}

// (LARECOMP_XR_REPROJ, mu held) a set's re-shown refreshes' images whose submission is done (all of them: `all`, the set
// being replaced, whose submissions came before its successor's), released: from then on a layer may name them
static void reshow_release(XrSet *s, uint64_t done, XrTime pdt, bool all) {
    for (int k = 0; k < XR_RESHOW_MAX; k++) {
        if ((!s->rs_has[k][0] && !s->rs_has[k][1]) || s->rs_rel[k] || (!all && s->rs_seq[k] > done)) continue;
        for (int i = 0; i < 2; i++) if (s->rs_has[k][i]) xr_chain_release(&g_xr.rs[s->pair][k][i]);
        s->rs_rel[k] = true;
        if (s->rs_ok[k] && pdt && s->grid0 && s->period > 0 && !all) {  // (how early: refreshes before the one it is for)
            int64_t d = s->grid0 + (k + 1) * s->period - pdt, m = d >= 0 ? (d + s->period / 2) / s->period : -((-d + s->period / 2) / s->period);
            st.rs_ahead[m >= 2 ? 0 : m == 1 ? 1 : m == 0 ? 2 : 3]++;
        }
    }
}

// (LARECOMP_XR_BFI, the XR thread's) on: following the lit refreshes (armed, the game at 60); slow: the watchdog's
// verdict, most of the last 16 lit refreshes without a new set (the game not at 60: every refresh shows the set while
// it lasts); last: the last lit refresh; miss: the last 16 lit refreshes, a bit set for each without a new set; run:
// those in a row
static struct { bool on, slow; XrTime last; uint32_t miss; int run; } bfi;

// The pending set, once the GPU finished its submission: its images released, and it becomes the one shown. Also
// while the session isn't running (no frames, pdt 0), so the presenting thread never waits on a display that isn't
// there. (LARECOMP_XR_REPROJ rot / pos / follow) A set finished before the refresh its poses were located for waits for
// it (at most XR_RESHOW_MAX + 1 refreshes): shown earlier, its images would be a refresh ahead of the head; the set
// shown meanwhile has its own image for that refresh. The images drawn for its later refreshes are submissions of
// their own after the set's: released each once its own is done, whether the set is pending or shown.
void xr_release_completed(XrTime pdt) {
    pthread_mutex_lock(&g_xr.mu);
    uint64_t done = vk_completed_seq();
    if (g_xr.shown_any) reshow_release(&g_xr.shown, done, pdt, false);
    if (g_xr.pending.valid) reshow_release(&g_xr.pending, done, pdt, false);
    if (g_xr.pending.valid) {
        // (its redraws read its eye images, and move them to a sampled layout and back: the set is released only once
        // they are done too, so the runtime never has an image this side still touches. LARECOMP_XR_REPROJ_JOINT=0, a
        // test: released once its own submission is done and the redraws are submitted, 1 to 4 ms sooner on the Steam
        // Frame; only where a layout change of a colour image is no work for the driver, as on Turnip)
        static int joint = -1;
        if (joint < 0) joint = !(getenv("LARECOMP_XR_REPROJ_JOINT") && atoi(getenv("LARECOMP_XR_REPROJ_JOINT")) == 0);
        uint64_t need = joint && g_xr.pending.reads > g_xr.pending.seq ? g_xr.pending.reads : g_xr.pending.seq;
        if (done >= need && rec_submitted() >= g_xr.pending.reads) {
            XrSet *p = &g_xr.pending;
            if (pdt && !p->ready) p->ready = pdt, lead_note(p, pdt);
            // (LARECOMP_XR_BFI: up to 8 refreshes, so a lead grown by a hitch does not let a set replace the one shown
            // before that one's lit refresh: it would never be lit)
            bool bfi_hold = xr_bfi_armed();
            if (pdt && p->grid0 && p->period > 0 && (xr_reproj_mode() >= XR_REPROJ_ROT || bfi_hold) && 2 * (p->grid0 - pdt) > p->period &&
                p->grid0 - pdt <= (bfi_hold ? 8 : XR_RESHOW_MAX + 1) * p->period) {
                st.held++;
                pthread_mutex_unlock(&g_xr.mu);
                return;
            }
            XrSet s = g_xr.pending;
            xr_chain_release(&g_xr.hud[s.pair]);
            for (int i = 0; i < 2; i++) if (s.has_eye[i]) xr_chain_release(&g_xr.eyes[s.pair][i]);
            // (the redraws of the set shown until now were submitted before this one's own submission: done)
            if (g_xr.shown_any) reshow_release(&g_xr.shown, done, pdt, true);
            if (g_xr.shown_any && bfi.on && !g_xr.shown.bfi_lit) st.bfi_unlit++;
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
// xrBeginSession on plat_now_ns's clock (docs/af/TOOLING.md; tools/session-report.py reads it).
// GPU ms per eye come from the stereo executor (xr_note_eye_gpu; -1 while unknown).
static FILE *tlog;
static void timing_open(void) {
    char p[1024], q[1040];
    snprintf(p, sizeof p, "%s/xr_timing.log", rt_data_root());
    snprintf(q, sizeof q, "%s.prev", p);
    remove(q);  // (Windows' rename does not replace a file)
    rename(p, q);
    tlog = fopen(p, "w");
    if (!tlog) LOG("[xr] cannot write %s\n", p);
}
static double time_ms(XrTime t) {  // an XrTime on xr_timing.log's clock
    int64_t ns = t;  // (SteamVR's on Linux and the mock's XrTime are CLOCK_MONOTONIC ns already)
    xr_time_to_plat(t, &ns);
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

// LARECOMP_XR_POSE_LOG=<file>: per refresh, the head (VIEW in LOCAL) and the eye gaze (relative to the head) at the
// refresh's predicted display time, in the text format xrmock's MOCK_XR_POSE=file: and MOCK_XR_GAZE=file read
// ("t x y z yaw pitch roll gaze_yaw gaze_pitch": seconds since xrBeginSession, metres, degrees; yaw unwrapped, nan when
// there is no gaze), so a headset session can be replayed on the desktop (docs/af/TOOLING.md). Replayed as the mock's
// STAGE, the mock's LOCAL becomes the recording's first pose, which is where the game measures from anyway.
// Unset: one test per refresh, nothing else.
static FILE *plog;
static int plog_on = -1;
static void pose_log(XrTime t) {
    if (plog_on < 0) {
        const char *e = getenv("LARECOMP_XR_POSE_LOG");
        plog_on = e && *e;
        if (plog_on && !(plog = fopen(e, "w"))) LOG("[xr] cannot write the pose log %s\n", e), plog_on = 0;
        if (plog_on) setvbuf(plog, NULL, _IOLBF, 0);  // (whole lines: the game can leave by _exit)
        if (plog_on)
            fprintf(plog, "# LARECOMP_XR_POSE_LOG: t x y z yaw pitch roll gaze_yaw gaze_pitch (VIEW in LOCAL at each refresh's display "
                          "time; seconds since xrBeginSession, metres, degrees; xrmock MOCK_XR_POSE=file:, MOCK_XR_GAZE=file)\n");
    }
    if (!plog_on) return;
    XrSpaceLocation loc = {.type = XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (XR_FAILED(p_xrLocateSpace(g_xr.view, g_xr.local, t, &loc)) || (loc.locationFlags & need) != need) return;
    const XrQuaternionf q = loc.pose.orientation;
    const float r2d = 57.2957795f;
    // yaw about +y, then pitch about x, then roll about z (the mock's order): yaw and pitch from the forward vector,
    // roll from what is left of q once yaw and pitch are taken out
    float fx = -2 * (q.x * q.z + q.w * q.y), fy = -2 * (q.y * q.z - q.w * q.x), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    float yaw = atan2f(-fx, -fz), pitch = asinf(fminf(1.0f, fmaxf(-1.0f, fy)));
    float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f), cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    XrQuaternionf a = {sp * cy, sy * cp, -sy * sp, cy * cp};  // qy * qx; its conjugate times q is the roll about z
    float rz = a.w * q.z - a.x * q.y + a.y * q.x - a.z * q.w, rw = a.w * q.w + a.x * q.x + a.y * q.y + a.z * q.z;
    float roll = 2 * atan2f(rz, rw);
    if (roll > 3.14159265f) roll -= 6.2831853f;  // (q and -q are one rotation)
    if (roll < -3.14159265f) roll += 6.2831853f;
    static float last_yaw;
    static bool have;
    float yd = yaw * r2d;
    if (have) yd += 360.0f * roundf((last_yaw - yd) / 360.0f);  // (unwrapped: the mock interpolates linearly)
    last_yaw = yd, have = true;
    float d[3], gy = NAN, gp = NAN;
    if (xr_input_gaze(g_xr.view, t, d)) gy = atan2f(-d[0], -d[2]) * r2d, gp = asinf(fminf(1.0f, fmaxf(-1.0f, d[1]))) * r2d;
    fprintf(plog, "%.5f %.5f %.5f %.5f %.4f %.4f %.4f %.3f %.3f\n", time_ms(t) / 1e3, loc.pose.position.x, loc.pose.position.y,
            loc.pose.position.z, yd, pitch * r2d, roll * r2d, gy, gp);
}

// The VR tab's Refresh Rate, between its two 90 Hz choices (30 FPS, 60 FPS test), while the game runs (runtime/vr_settings.c):
// the display's rate itself is SteamVR's, set by the launch wrapper (xrRequestDisplayRefreshRateFB is refused), but the
// game's rate at that rate is ours: the pacer's lock and frame time follow at the next refresh. 60: the 60 fps test, 0: as
// the display's rate gives (30 fps below 100 Hz). -1: LARECOMP_XR_FPS, read at the first refresh.
static int g_fps_req = -1;
void xr_set_game_fps(int fps) { __atomic_store_n(&g_fps_req, fps == 60 ? 60 : 0, __ATOMIC_RELEASE); }

// (LARECOMP_XR_BFI) whether refresh t shows the set shown (lit) or no projection layer (dark); *first: this is the
// refresh the set is lit on. A set is lit once, on the refresh its poses were located for (its grid line, on the lit
// refreshes' phase: predict_time), never sooner than two refreshes after the last flash. A set that is not finished by
// then (late) is lit on the first refresh of the phase after it, 2 periods after its poses; the lit refresh it missed
// stays dark (a flash skipped: three dark refreshes), as does any other lit refresh with no new set. Lighting the set
// before again there would show its image twice, 16.7 ms apart: the double image this removes, once
// (LARECOMP_XR_BFI_LATE=relight, a test, does that). A second such refresh in a row is a stall (a load, a hitch), not a
// late set: the set before is lit again then, so the view does not go black. Sets missing on most lit refreshes (the
// game at 30 for a reason the pacer does not say) light every refresh until they come again (the watchdog).
static bool bfi_step(XrTime t, int64_t per, bool *first) {
    static int relight = -1;
    if (relight < 0) relight = getenv("LARECOMP_XR_BFI_LATE") && !strcmp(getenv("LARECOMP_XR_BFI_LATE"), "relight");
    XrSet *s = &g_xr.shown;
    *first = false;
    if (!s->bfi_slot) s->bfi_slot = s->grid0 ? s->grid0 : t;
    int64_t since = bfi.last ? periods(t - bfi.last, per) : 2, d = periods(t - s->bfi_slot, per);
    int gap = since < 1 ? 0 : since > 5 ? 4 : (int)since - 1;
    if (!s->bfi_lit && d >= 0 && !(d & 1) && since >= 2) {
        s->bfi_lit = true, *first = true;
        bfi.last = t, bfi.miss <<= 1, bfi.run = 0;
        if (bfi.slow && __builtin_popcount(bfi.miss & 0xffff) <= 1) bfi.slow = false;
        int64_t ph = atomic_load(&g_xr.bfi_phase);  // (the phase's anchor follows the flashes on it: no drift)
        if (ph && !(periods(t - ph, per) & 1)) atomic_store(&g_xr.bfi_phase, t);
        if (!bfi.slow) st.bfi_gap[gap]++, d ? st.bfi_late++ : st.bfi_on++;
        return true;
    }
    if (since < 2 || (since & 1)) return bfi.slow;
    bfi.miss = bfi.miss << 1 | 1;
    bfi.run++;
    if (__builtin_popcount(bfi.miss & 0xffff) >= 6) bfi.slow = true;
    if (bfi.slow) return true;
    if (s->bfi_lit && (bfi.run >= 2 || relight)) {  // (a set not lit yet is not lit before its own refresh)
        st.bfi_gap[gap]++, bfi.run >= 2 ? st.bfi_stall++ : st.bfi_relit++;
        bfi.last = t;
        return true;
    }
    st.bfi_skip++;
    return false;
}

static void end_frame(const XrFrameState *fs, bool new_set) {
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection proj = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad = {.type = XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad laser[2];
    const XrCompositionLayerBaseHeader *layers[4];
    uint32_t n = 0;
    // (LARECOMP_XR_BFI) a dark refresh has no projection layer; the HUD / menu panel and the laser stay on every refresh:
    // quads the compositor places for each refresh's own head pose do not double, and their text stays as bright and
    // steady as without it. first: the set's first refresh shown (with it: the one it is lit on)
    bool lit = true, first = new_set;
    if (bfi.on && fs->shouldRender && g_xr.shown_any) {
        lit = bfi_step(fs->predictedDisplayTime, xr_bfi_period(), &first);
        lit ? st.bfi_lit++ : st.bfi_dark++;
    }
    if (fs->shouldRender && g_xr.shown_any) {
        const XrSet *s = &g_xr.shown;
        if (lit && s->views_ok && s->has_eye[0] && s->has_eye[1]) {
            // the eye images for this refresh (LARECOMP_XR_REPROJ): the set's own, or those drawn for the refresh whose
            // display time is nearest this one's, each with the views it was drawn for; past the last one drawn (the next
            // set is late) the last one stays
            const XrView *views = s->views;
            const XrChain *ch[2] = {&g_xr.eyes[s->pair][0], &g_xr.eyes[s->pair][1]};
            int k = 0;
            if (s->grid0 && s->period > 0 && s->rs_n) {
                int64_t d = fs->predictedDisplayTime - s->grid0;
                k = d <= 0 ? 0 : (int)((d + s->period / 2) / s->period);
                if (k > s->rs_n) k = s->rs_n, st.rs_past++;
                if (k > 0 && s->rs_ok[k - 1] && !s->rs_rel[k - 1]) st.rs_fell++;  // (its submission not done yet)
                while (k > 0 && !(s->rs_ok[k - 1] && s->rs_rel[k - 1])) k--;
            }
            XrView fresh[2];
            int mode = xr_reproj_mode();
            if (k) {
                views = s->rs_views[k - 1], ch[0] = &g_xr.rs[s->pair][k - 1][0], ch[1] = &g_xr.rs[s->pair][k - 1][1];
                st.rs_drawn++;
            } else if (!new_set && mode == XR_REPROJ_FRESH && xr_locate_in(fs->predictedDisplayTime, s->space, fresh)) {
                views = fresh;  // (the test: a pose located for this refresh on the set's own images)
                st.rs_fresh++;
            } else if (!new_set && mode >= XR_REPROJ_ROT) {
                st.rs_none++;
            }
            if (s->exec_eyes && views == s->views) {
                if (xr_layer_matches_render(s)) st.pose_ok++;
                else st.pose_bad++;
            }
            if (first && s->grid0 && s->period > 0) {
                int64_t d = fs->predictedDisplayTime - s->grid0;
                if (2 * d >= s->period) st.first_late++;
                else if (2 * d <= -s->period) st.first_early++;
                else st.first_on++;
            }
            if (first && s->latch_ns) {  // (its first refresh: how far ahead the poses were located, the latency)
                int64_t disp = fs->predictedDisplayTime - atomic_load(&g_xr.xr_minus_plat);
                int64_t lat = disp - (int64_t)s->latch_ns, err = s->vtime - fs->predictedDisplayTime;
                st.firsts++, st.lat_sum_ns += lat, st.vt_err_sum_ns += err < 0 ? -err : err;
                if ((err < 0 ? -err : err) > st.vt_err_max_ns) st.vt_err_max_ns = err < 0 ? -err : err;
                if (lat > 0 && lat < 250000000ll) {  // (a load's hitch is not the pipeline's latency)
                    int64_t l = atomic_load(&g_xr.lat_ns);
                    atomic_store(&g_xr.lat_ns, l ? l + (lat - l) / 8 : lat);
                }
            }
            for (int i = 0; i < 2; i++) {
                // (a drawn refresh: both eyes may be the layers of one image, and only its top left part drawn)
                bool lay = k && ch[0]->layers > 1;
                const XrChain *c = lay ? ch[0] : ch[i];
                uint32_t w = k ? s->rs_w[k - 1] : c->w, h = k ? s->rs_h[k - 1] : c->h;
                pv[i] = (XrCompositionLayerProjectionView){
                    .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = views[i].pose, .fov = views[i].fov,
                    .subImage = {c->sc, {{0, 0}, {(int32_t)w, (int32_t)h}}, lay ? (uint32_t)i : 0}};
            }
            proj.space = s->space;
            // (LARECOMP_VR_EDGE=passthrough: the eye images carry premultiplied alpha, 0 in the void: runtime/nvn/vulkan/edge.c)
            if (s->alpha) proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            proj.viewCount = 2;
            proj.views = pv;
            layers[n++] = (const XrCompositionLayerBaseHeader *)&proj;
        }
        // the HUD: a flat screen straight ahead at eye height, LARECOMP_XR_QUAD="distance,width" (2 m, 2.2 m)
        // (LARECOMP_XR_HUD=0, a test: no HUD / menu layer at all, to tell what is on it from what is in the eye images)
        static int hud_on = -1;
        if (hud_on < 0) hud_on = !getenv("LARECOMP_XR_HUD") || atoi(getenv("LARECOMP_XR_HUD")) != 0;
        const XrChain *hc = &g_xr.hud[s->pair];
        float w = g_xr.quad_width, h = w * (float)hc->h / (float)hc->w;
        quad.space = g_xr.app;
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // (the stereo executor's HUD is transparent around the layout)
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage = (XrSwapchainSubImage){hc->sc, {{0, 0}, {(int32_t)hc->w, (int32_t)hc->h}}, 0};
        quad.pose = (XrPosef){.orientation = {0, 0, 0, 1}, .position = {0, 0, -g_xr.quad_dist}};
        quad.size = (XrExtent2Df){w, h};
        if (hud_on) layers[n++] = (const XrCompositionLayerBaseHeader *)&quad;
        // the menu laser over it (its own layers: drawn under the panel they hid behind it, GalaxyFrame's first bug)
        for (uint32_t i = 0, k = xr_pointer_layers(laser); i < k; i++) layers[n++] = (const XrCompositionLayerBaseHeader *)&laser[i];
        if (!new_set && lit) st.reshown++;
    } else {
        st.no_render++;
    }
    // (Table Edge Passthrough: a dark refresh keeps the alpha blend, so it shows the room with nothing over it; the table
    // is on half the refreshes and the room on all, so the room shows through the table. LARECOMP_XR_BFI_ROOM=0, a test:
    // dark refreshes opaque, black, the room flashing with the table)
    static int room = -1;
    if (room < 0) room = !getenv("LARECOMP_XR_BFI_ROOM") || atoi(getenv("LARECOMP_XR_BFI_ROOM")) != 0;
    bool blend = g_xr.has_alpha_blend && g_xr.shown_any && g_xr.shown.alpha && (lit || room);
    XrFrameEndInfo ei = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = fs->predictedDisplayTime,
                         .environmentBlendMode = blend ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = n, .layers = layers};
    uint64_t t0 = plat_now_ns();
    vk_queue_lock();
    uint64_t t1 = plat_now_ns();
    XrResult r = XR_RT(p_xrEndFrame(g_xr.session, &ei));
    vk_queue_unlock();
    uint64_t t2 = plat_now_ns();
    st.end_n++, st.end_sum_ns += t2 - t1;
    if (t2 - t1 > st.end_max_ns) st.end_max_ns = t2 - t1;
    if (t2 - t1 > 2000000) st.end_slow++;
    if (t1 - t0 > 1000000) st.lock_slow++;
    xr_ok_(r, "xrEndFrame");
}

static void log_stats(double secs) {
    pthread_mutex_lock(&g_xr.mu);
    LOG("[xr] %.0f s: %llu refreshes at %.0f Hz (%llu missed), %llu vsyncs, %llu sets shown (%.1f ms after submission), "
        "%llu refreshes re-showed a set, %llu waited for the GPU, %llu without layers; presenting: %llu waits (%.2f ms avg), "
        "%llu frames skipped; pose mismatches %llu of %llu refreshes with the executor's eyes, %llu sets with eyes of two frames; "
        "latch to display %.1f ms (poses located for %.1f ms off it, max %.1f; predict %s %.1f ms); xrEndFrame avg %.2f ms, max %.1f, "
        "%llu over 2 ms, %llu waited over 1 ms for the queue\n", secs, (unsigned long long)st.refreshes,
        g_xr.display_hz, (unsigned long long)st.missed,
        (unsigned long long)st.vsyncs, (unsigned long long)st.released, st.released ? st.latency_ns / 1e6 / st.released : 0.0,
        (unsigned long long)st.reshown, (unsigned long long)st.gpu_late, (unsigned long long)st.no_render,
        (unsigned long long)st.waits, st.waits ? st.wait_ns / 1e6 / st.waits : 0.0, (unsigned long long)st.skipped,
        (unsigned long long)st.pose_bad, (unsigned long long)(st.pose_ok + st.pose_bad), (unsigned long long)st.eye_split,
        st.firsts ? st.lat_sum_ns / 1e6 / st.firsts : 0.0, st.firsts ? st.vt_err_sum_ns / 1e6 / st.firsts : 0.0, st.vt_err_max_ns / 1e6,
        g_xr.predict ? "measured" : "vsync", atomic_load(&g_xr.lat_ns) / 1e6, st.end_n ? st.end_sum_ns / 1e6 / st.end_n : 0.0,
        st.end_max_ns / 1e6, (unsigned long long)st.end_slow, (unsigned long long)st.lock_slow);
    if (st.rs_drawn + st.rs_none + st.rs_fresh)
        LOG("[xr] re-shown refreshes: %llu with images drawn for their pose (%llu of them past the last drawn for: the next "
            "set late; %llu an older one: the image for theirs not finished), %llu with the set's own (none drawn: skipped; or "
            "none finished), "
            "%llu with a fresh pose on the set's images; a set's first refresh: %llu on its grid line, %llu later, %llu earlier; "
            "%llu refreshes a finished set waited for its own (lead %d); drawn images found finished 2+ / 1 / 0 refreshes "
            "before theirs, or after: %llu / %llu / %llu / %llu\n",
            (unsigned long long)st.rs_drawn, (unsigned long long)st.rs_past, (unsigned long long)st.rs_fell,
            (unsigned long long)st.rs_none, (unsigned long long)st.rs_fresh, (unsigned long long)st.first_on,
            (unsigned long long)st.first_late, (unsigned long long)st.first_early, (unsigned long long)st.held,
            atomic_load(&g_xr.lead), (unsigned long long)st.rs_ahead[0], (unsigned long long)st.rs_ahead[1],
            (unsigned long long)st.rs_ahead[2], (unsigned long long)st.rs_ahead[3]);
    if (bfi_wanted()) {
        static const char *const k_state[] = {"off", "flashing", "waits for 120 Hz at 60 fps", "waits: the game at 30"};
        LOG("[xr] motion clarity (%s): %llu refreshes lit, %llu dark; sets lit on the refresh their poses were located for "
            "%llu, later %llu, never %llu; lit refreshes with no new set: %llu left dark (a flash skipped), %llu the set "
            "before again (%llu a stall); flashes 1 / 2 / 3 / 4 / 5+ refreshes after the one before: %llu / %llu / %llu / "
            "%llu / %llu; phase moved %llu (lead %d)\n",
            k_state[atomic_load(&g_bfi_state) & 3], (unsigned long long)st.bfi_lit, (unsigned long long)st.bfi_dark,
            (unsigned long long)st.bfi_on, (unsigned long long)st.bfi_late, (unsigned long long)st.bfi_unlit,
            (unsigned long long)st.bfi_skip, (unsigned long long)(st.bfi_relit + st.bfi_stall), (unsigned long long)st.bfi_stall,
            (unsigned long long)st.bfi_gap[0], (unsigned long long)st.bfi_gap[1], (unsigned long long)st.bfi_gap[2],
            (unsigned long long)st.bfi_gap[3], (unsigned long long)st.bfi_gap[4], (unsigned long long)st.bfi_rephase,
            atomic_load(&g_xr.lead));
    }
    memset(&st, 0, sizeof st);
    pthread_mutex_unlock(&g_xr.mu);
}

static void *xr_thread(void *arg) {
    (void)arg;
    plat_thread_realtime(8333333);
    plat_thread_perf_core(PLAT_PERF_VSYNC);
    int64_t last_d = 0;
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
            xr_release_completed(0);
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
        {   // XrTime - plat_now_ns() (both CLOCK_MONOTONIC on SteamVR and the mock: 0 there; converted when the runtime can)
            int64_t ns;
            if (xr_time_to_plat(fs.predictedDisplayTime, &ns)) atomic_store(&g_xr.xr_minus_plat, fs.predictedDisplayTime - ns);
        }
        int64_t period = fs.predictedDisplayPeriod > 0 ? fs.predictedDisplayPeriod : 8333333;
        atomic_store(&g_xr.grid_period, period);  // (the display's grid: the re-shown refreshes' display times, xr_reshow_times)
        atomic_store(&g_xr.grid_t, fs.predictedDisplayTime);
        {   // a new rate once it holds HZ_FILTER_FRAMES refreshes (hz_filter.h): one-frame blips don't flip the 30 fps hold
            static HzFilter hzf;
            static uint32_t blips_logged;
            static uint64_t blip_log_ns;
            if (hzf.hz != g_xr.display_hz) hzf.hz = g_xr.display_hz, hzf.run = 0;  // set by xr_session.c too (rate events)
            if (hz_filter_step(&hzf, period)) g_xr.display_hz = hzf.hz;
            if (hzf.rejected != blips_logged && now - blip_log_ns > 10000000000ull) {
                LOG("[xr] display period blips ignored: %u (last %.2f ms)\n", hzf.rejected, hzf.last_blip / 1e6);
                blips_logged = hzf.rejected, blip_log_ns = now;
            }
        }
        // The game's rate at the display's (hz_filter.h hz_rule): 60 FPS at a multiple of 60 Hz, held at 30 FPS at a
        // multiple of 30 (90), 60 FPS with an uneven cadence at the others (72, 80, 144). LARECOMP_XR_FPS=60 (a test):
        // 60 FPS at 90 Hz too, uneven; =30 (a test): held at 30 at 120 Hz too, as the pacer holds it on the headset when
        // the GPU frame does not fit 60, the pacer's 120 Hz budgets kept. The VR tab's Refresh Rate changes the 90 Hz
        // choice while the game runs (xr_set_game_fps).
        static HzCadence cad;
        static bool bfi_rate;  // (LARECOMP_XR_BFI) a 60 FPS frame on two refreshes, evenly: 120 Hz (119.88), not held at 30
        {
            static int locked_key = -1;
            static int test60 = -1, test30 = -1;
            if (test60 < 0) test60 = getenv("LARECOMP_XR_FPS") && atoi(getenv("LARECOMP_XR_FPS")) == 60;
            if (test30 < 0) test30 = getenv("LARECOMP_XR_FPS") && atoi(getenv("LARECOMP_XR_FPS")) == 30;
            int fr = __atomic_exchange_n(&g_fps_req, -1, __ATOMIC_ACQ_REL);
            if (fr >= 0 && (fr == 60) != (test60 == 1)) test60 = fr == 60, LOG("[xr] game frame rate at 90 Hz: %s (VR settings)\n", test60 ? "60 fps (test)" : "30 fps");
            HzRule rule = hz_rule(g_xr.display_hz, test60 ? 60 : test30 ? 30 : 0);
            int key = (int)lroundf(g_xr.display_hz * 10) * 4 + test60 * 2 + test30;
            if (key != locked_key) {
                pace_xr_lock(rule.hold30 ? 30 : 0);
                pace_xr_frame_ms(rule.frame_ms);
                hz_cadence_set(&cad, &rule);
                float per = rule.hold30 ? g_xr.display_hz / 30 : g_xr.display_hz / 60;
                atomic_store(&g_per_frame, (int)ceilf(per - 0.01f));
                bfi_rate = rule.even && !rule.hold30 && fabsf(per - 2.0f) < 0.05f;
                if (test60) LOG("[xr] test: 60 fps at %.0f Hz (uneven cadence)\n", g_xr.display_hz);
                if (test30 && rule.hold30) LOG("[xr] test: the game held at 30 fps (LARECOMP_XR_FPS=30)\n");
                if (rule.even) LOG("[xr] display at %.0f Hz: the game %s fps (%.0f refresh%s a frame)\n", g_xr.display_hz,
                                   rule.hold30 ? "held at 30" : "at 60", per, per < 1.5f ? "" : "es");
                else LOG("[xr] display at %.0f Hz: the game %s fps, %d or %d refreshes a frame (uneven: 120 Hz gives 60 fps "
                         "and 90 Hz 30 fps evenly)\n", g_xr.display_hz, rule.hold30 ? "held at 30" : "at 60", (int)per,
                         (int)per + 1);
                locked_key = key;
            }
        }
        {   // Motion Clarity (LARECOMP_XR_BFI): armed at 120 Hz with a 60 FPS game, flashing while the game gives 60;
            // otherwise every refresh shows the set, as off (said once a change)
            bool want = bfi_wanted(), armed = want && bfi_rate, on = armed && !atomic_load(&g_bfi_game30);
            atomic_store(&g_bfi_armed, armed);
            if (!armed) atomic_store(&g_xr.bfi_phase, 0);
            if (on != bfi.on) bfi = (typeof(bfi)){.on = on};
            int state = !want ? 0 : !armed ? 2 : !on || bfi.slow ? 3 : 1;
            static int logged;
            if (state != logged) {
                if (state == 1) LOG("[xr] motion clarity: flashing (each set on one refresh of two at %.0f Hz)\n", g_xr.display_hz);
                else if (state == 2) LOG("[xr] motion clarity: waits for 120 Hz with the game at 60 fps (display at %.0f Hz%s): every refresh shows the set\n",
                                         g_xr.display_hz, g_xr.display_hz > 118 && g_xr.display_hz < 122 ? ", the game held at 30" : "");
                else if (state == 3) LOG("[xr] motion clarity: waits while the game runs at 30 fps (%s): every refresh shows the set\n",
                                         on ? "no new set on most lit refreshes" : "held by the pacer or the FPS target");
                else if (logged) LOG("[xr] motion clarity: off\n");
                logged = state;
            }
            atomic_store(&g_bfi_state, state);
        }
        xr_input_frame();  // the controllers, once per refresh, just before the game's vsync
        pose_log(fs.predictedDisplayTime);
        // The game's 60 Hz vsync from the display's refreshes (hz_cadence_step): every second one at 120 Hz, two of three
        // at 90 Hz, by time at the uneven rates (by display time, so a missed refresh doesn't slow the game down). The
        // frame starting now is shown two refreshes on.
        int64_t step = last_d ? fs.predictedDisplayTime - last_d : period;
        int64_t n_ref = last_d ? (step + period / 2) / period : 1;
        if (last_d && step > period * 3 / 2) st.missed += (uint64_t)n_ref - 1;
        if (step > 100000000) step = period, n_ref = 1;  // (a pause: start over)
        last_d = fs.predictedDisplayTime;
        st.refreshes++;
        if (hz_cadence_step(&cad, step, n_ref < 1 ? 1 : (int)n_ref)) {
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
        xr_input_pointer(fs.predictedDisplayTime);  // the menu laser at this refresh's display time, in the app space now
        uint64_t before = st.released, late_before = st.gpu_late;
        xr_release_completed(fs.predictedDisplayTime);
        uint64_t t_end = plat_now_ns();
        end_frame(&fs, st.released != before);
        timing_line(frame_no++, t_wait0, now, &fs, t_begin, t_end, st.gpu_late != late_before);
        if (now - stats_at >= 5000000000ull) {
            if (tlog) fflush(tlog);
            if (plog) fflush(plog);
            log_stats((now - stats_at) / 1e9);
            stats_at = now;
        }
    }
    return NULL;
}

bool xr_thread_start(void) {
    g_xr.stop = false;
    const char *pe = getenv("LARECOMP_XR_PREDICT");
    g_xr.predict = !(pe && !strcmp(pe, "vsync"));
#ifdef _WIN32
    // (the measured prediction compares our clock with the runtime's: without the conversion the two are unrelated on
    // Windows, so the latest vsync's display time is used, as LARECOMP_XR_PREDICT=vsync)
    if (g_xr.predict && !xr_time_convertible()) g_xr.predict = 0, LOG("[xr] no time conversion from the runtime: poses predicted from the vsync\n");
#endif
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
    if (plog) fclose(plog), plog = NULL, plog_on = 0;
    xr_drain_gpu("swapchain destruction");
    xr_chains_destroy();
    xr_session_destroy();
    LOG("[xr] session and instance destroyed\n");
}
