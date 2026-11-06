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
