// The display rate from XrFrameState.predictedDisplayPeriod, with one-frame blips ignored.
// SteamVR at 120 Hz now and then reports a single period of 16.67 or 25 ms (a missed refresh); taking each one as a
// rate change flipped the game into its 30 fps hold and back (the 2026-11-06 13:44 session: 54 flips). A new rate is
// accepted only once HZ_FILTER_FRAMES periods in a row agree on it (within 1%): 0.25 s at 120 Hz, 0.33 s at 90 Hz.
// Header-only so tools/test_hz_filter.c can replay it without a session.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HZ_FILTER_FRAMES 30

typedef struct {
    float hz;           // the accepted rate (0: none yet, the next period is taken as is)
    float cand;         // a different rate seen run frames in a row
    int run;
    uint32_t rejected;  // runs of a different rate that ended before they were accepted (blips)
    int64_t last_blip;  // the last blip's period (ns)
} HzFilter;

static inline bool hz_filter_same(float a, float b) { return a * 0.99f < b && b < a * 1.01f; }

// One frame's period; true when the accepted rate (f->hz) changes.
static inline bool hz_filter_step(HzFilter *f, int64_t period_ns) {
    if (period_ns <= 0) return false;
    float hz = 1e9f / (float)period_ns;
    if (!f->hz) {
        f->hz = hz, f->run = 0;
        return true;
    }
    if (hz_filter_same(f->hz, hz)) {
        if (f->run) f->rejected++;
        f->run = 0;
        return false;
    }
    if (f->run && hz_filter_same(f->cand, hz)) f->run++;
    else {
        if (f->run) f->rejected++;
        f->cand = hz, f->run = 1;
    }
    f->last_blip = period_ns;
    if (f->run < HZ_FILTER_FRAMES) return false;
    f->hz = f->cand, f->run = 0;
    return true;
}

// ---- what the game does at a display rate (xr_loop.c; docs/af/PCVR.md "Refresh rates"). The game runs at 60 FPS or in
// its 30 FPS mode, nothing between, so only a display at a multiple of one of them shows every frame for the same number
// of refreshes:
//   a multiple of 60 Hz (60, 120): 60 FPS, each frame hz/60 refreshes;
//   a multiple of 30 Hz but not of 60 (90, 150): held at 30 FPS, each frame hz/30 refreshes (60 FPS there alternates one
//     and two refreshes: LARECOMP_XR_FPS=60, the VR tab's 90 Hz (60 FPS, Test));
//   anything else (72, 80, 144 Hz): no even cadence exists, so 60 FPS, each frame one or two refreshes (two or three at
//     144) in turn: at 60 the uneven steps are smaller and the motion closer to even than at 30, which is uneven there too.
// fps_test: LARECOMP_XR_FPS (60: 60 FPS at a 30 Hz multiple too; 30: held at 30 at the other rates too; 0: neither).
typedef struct {
    bool hold30;      // pace_xr_lock(30)
    bool even;        // every frame shown for the same number of refreshes
    double frame_ms;  // pace_xr_frame_ms: the game frame the VR GPU limits scale from (0: held at 30 for good)
    int num, den;     // the game's vsync every num / den refreshes (hz_cadence; 0: by time, 16.67 ms)
} HzRule;

static inline int hz_multiple(float hz, float of) {  // hz / of when it is a whole number within 1%, else 0
    int k = (int)(hz / of + 0.5f);
    return k >= 1 && hz_filter_same(k * of, hz) ? k : 0;
}

static inline HzRule hz_rule(float hz, int fps_test) {
    HzRule r = {0};
    int k60 = hz > 0 ? hz_multiple(hz, 60) : 0, k30 = hz > 0 ? hz_multiple(hz, 30) : 0;
    if (k60) {  // (60 Hz: SteamVR throttling 120 Hz to every other refresh, framesToThrottle=1)
        r.even = true, r.num = k60, r.den = 1;
        r.hold30 = fps_test == 30;
        r.frame_ms = 1e3 * k60 / hz;  // (16.67; the 30 test keeps these budgets too)
    } else if (k30 || hz <= 0) {  // (no rate yet: held, as the 90 Hz path)
        r.even = fps_test != 60;
        r.num = k30, r.den = 2;  // (a vsync every k30 / 2 refreshes: two of three at 90 Hz)
        r.hold30 = fps_test != 60;
        r.frame_ms = r.hold30 ? 0 : 1e3 / 60;
    } else {
        r.hold30 = fps_test == 30;
        r.frame_ms = 1e3 / 60;
    }
    return r;
}

// The game's 60 Hz vsync from the display's refreshes (xr_loop.c). At an even rate refreshes are counted: a vsync every
// num / den of them (2 at 120 Hz, two of three at 90), so a display slightly off its nominal rate (119.88 Hz) never gives
// a frame one refresh more or less - the game then keeps the display's pace (59.94 FPS). At the other rates, by time: a
// vsync at the first refresh at or after each 16.67 ms (half a millisecond early counts). refreshes: how many the last
// step was (1, more when the runtime skipped some). Returns whether this refresh is a vsync.
#define HZ_GAME_PERIOD_NS 16666667ll
typedef struct {
    int num, den;
    int64_t acc;  // by time: ns since the last vsync; counting: refreshes x den since the last vsync
} HzCadence;

static inline void hz_cadence_set(HzCadence *c, const HzRule *r) {
    if (c->num != r->num || c->den != r->den) c->num = r->num, c->den = r->den, c->acc = 0;
}

static inline bool hz_cadence_step(HzCadence *c, int64_t step_ns, int refreshes) {
    if (c->num > 0) {
        c->acc += (int64_t)refreshes * c->den;
        if (c->acc < c->num) return false;
        c->acc -= c->num;
        if (c->acc > c->num) c->acc = 0;  // (refreshes skipped: one vsync, then from the start, as by time)
        return true;
    }
    c->acc += step_ns;
    if (c->acc < HZ_GAME_PERIOD_NS - 500000) return false;
    c->acc -= HZ_GAME_PERIOD_NS;
    if (c->acc > HZ_GAME_PERIOD_NS) c->acc = 0;
    return true;
}
