// The display-rate filter (runtime/xr/hz_filter.h): one-frame period blips ignored, real rate changes taken; the game's
// rate at each display rate (hz_rule) and its vsync cadence (hz_cadence_step) at 60 to 144 Hz and slightly off-nominal ones.
// Build + run: clang -std=gnu11 -O1 -Iruntime tools/test_hz_filter.c -o build/test-hz-filter && build/test-hz-filter
// [xr_timing log ...]: also replays each log's period= column, counting rate changes the old one-frame check made
// (runtime/xr/xr_loop.c before the filter) against the filter's. Exits non-zero when a case fails.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xr/hz_filter.h"

#define NS(hz) ((int64_t)(1e9 / (hz)))
static int failed;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failed += !ok;
}

// Feeds n periods; returns the number of accepted changes, first_at: the frame of the first one (-1: none).
static int feed(HzFilter *f, int64_t period, int n, int *first_at, int *frame) {
    int changes = 0;
    for (int i = 0; i < n; i++, (*frame)++)
        if (hz_filter_step(f, period)) {
            if (first_at && *first_at < 0) *first_at = *frame;
            changes++;
        }
    return changes;
}

static void replay(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); failed++; return; }
    char line[512];
    float old_hz = 0;
    int old_changes = 0, new_changes = 0, frames = 0;
    HzFilter f = {0};
    while (fgets(line, sizeof line, fp)) {
        char *p = strstr(line, " period=");
        if (!p) continue;
        double ms = atof(p + 8);
        int64_t period = ms > 0 ? (int64_t)(ms * 1e6 + 0.5) : 8333333;
        frames++;
        if (!old_hz || old_hz * period < 0.99e9 || old_hz * period > 1.01e9) old_hz = 1e9f / (float)period, old_changes++;
        if (hz_filter_step(&f, period)) {
            new_changes++;
            printf("     frame %d: accepted %.0f Hz\n", frames, f.hz);
        }
    }
    fclose(fp);
    printf("     %s: %d frames; old check %d rate changes, filter %d (blips ignored %u)\n", path, frames, old_changes,
           new_changes, f.rejected);
    check(new_changes <= 3 && f.hz > 100, "log replay: no blip flips, ends at 120 Hz");
}


// The XR loop's vsync before the cadence (by time at every rate), for comparing the patterns at the nominal rates.
static bool old_step(int64_t *acc, int64_t step) {
    *acc += step;
    if (*acc < HZ_GAME_PERIOD_NS - 500000) return false;
    *acc -= HZ_GAME_PERIOD_NS;
    if (*acc > HZ_GAME_PERIOD_NS) *acc = 0;
    return true;
}

// n refreshes at hz through the rule's cadence: the refreshes each game frame is shown for (a frame: one vsync, two
// when held at 30), their least and most, and the game frames per second.
static void cadence(float hz, int fps_test, int n, int *lo, int *hi, double *fps) {
    HzRule r = hz_rule(hz, fps_test);
    HzCadence c = {0};
    hz_cadence_set(&c, &r);
    int64_t period = (int64_t)(1e9 / hz);
    int since = 0, vs = 0, frames = 0;
    *lo = 1 << 30, *hi = 0;
    for (int i = 0; i < n; i++) {
        since++;
        if (!hz_cadence_step(&c, period, 1)) continue;
        if (r.hold30 && (++vs & 1)) continue;
        if (frames++ > 2) *lo = since < *lo ? since : *lo, *hi = since > *hi ? since : *hi;  // (past the start)
        since = 0;
    }
    *fps = frames / (n / hz);
}

static void rates(void) {
    static const struct { float hz; int test; bool hold, even; int lo, hi; double fps; } k[] = {
        {120, 0, false, true, 2, 2, 60},     {119.88f, 0, false, true, 2, 2, 59.94}, {120.02f, 0, false, true, 2, 2, 60.01},
        {90, 0, true, true, 3, 3, 30},       {89.91f, 0, true, true, 3, 3, 29.97},   {90.05f, 0, true, true, 3, 3, 30.02},
        {60, 0, false, true, 1, 1, 60},      {72, 0, false, false, 1, 2, 60},        {80, 0, false, false, 1, 2, 60},
        {144, 0, false, false, 2, 3, 60},    {100, 0, false, false, 1, 2, 60},       {90, 60, false, false, 1, 2, 60},
        {120, 30, true, true, 4, 4, 30},     {150, 0, true, true, 5, 5, 30},
    };
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {
        HzRule r = hz_rule(k[i].hz, k[i].test);
        int lo, hi;
        double fps;
        cadence(k[i].hz, k[i].test, 20000, &lo, &hi, &fps);
        char what[160];
        snprintf(what, sizeof what, "%.2f Hz%s: %s fps, %s, %d-%d refreshes a frame, %.2f fps", k[i].hz,
                 k[i].test == 60 ? " (60 test)" : k[i].test == 30 ? " (30 test)" : "", r.hold30 ? "30" : "60",
                 r.even ? "even" : "uneven", lo, hi, fps);
        check(r.hold30 == k[i].hold && r.even == k[i].even && lo == k[i].lo && hi == k[i].hi && fps > k[i].fps - 0.05 &&
              fps < k[i].fps + 0.05, what);
    }
    // the frame time the pacer's VR limits scale from: as before at 60, 120 and the 90 Hz choices, 16.67 elsewhere
    check(hz_rule(120, 0).frame_ms > 16.6 && hz_rule(120, 0).frame_ms < 16.7 && hz_rule(90, 0).frame_ms == 0 &&
          hz_rule(90, 60).frame_ms > 16.6 && hz_rule(60, 0).frame_ms > 16.6 && hz_rule(72, 0).frame_ms > 16.6 &&
          hz_rule(144, 0).frame_ms > 16.6 && hz_rule(144, 0).frame_ms < 16.7, "frame time 16.67 ms (0 when held at 30 at 90 Hz)");
    // at exactly 90 and 120 Hz the counted cadence gives the vsyncs the timed one did
    for (int h = 0; h < 2; h++) {
        float hz = h ? 120 : 90;
        HzRule r = hz_rule(hz, 0);
        HzCadence c = {0};
        hz_cadence_set(&c, &r);
        int64_t acc = 0, period = (int64_t)(1e9 / hz + 0.5);
        int diff = 0;
        for (int i = 0; i < 5000; i++) diff += hz_cadence_step(&c, period, 1) != old_step(&acc, period);
        char what[64];
        snprintf(what, sizeof what, "%.0f Hz: the same vsyncs as before (%d differ)", hz, diff);
        check(diff == 0, what);
    }
    {   // skipped refreshes at 120 Hz (a step of 3 or 5): one vsync, the phase as the timed cadence had it
        HzRule r = hz_rule(120, 0);
        HzCadence c = {0};
        hz_cadence_set(&c, &r);
        int64_t acc = 0;
        int diff = 0, vs = 0;
        for (int i = 0; i < 400; i++) {
            int n = i % 37 == 5 ? 3 : i % 53 == 7 ? 5 : 1;
            bool a = hz_cadence_step(&c, n * 8333333ll, n), b = old_step(&acc, n * 8333333ll);
            diff += a != b, vs += a;
        }
        char what[80];
        snprintf(what, sizeof what, "skipped refreshes at 120 Hz: the timed cadence's vsyncs (%d of %d differ)", diff, vs);
        check(diff == 0, what);
    }
}

int main(int argc, char **argv) {
    rates();
    {   // steady 120 Hz with one-frame 16.67 and 25 ms blips (the 13:44 session)
        HzFilter f = {0};
        int frame = 0, first = -1, changes = feed(&f, NS(120), 100, &first, &frame);
        for (int b = 0; b < 55; b++) {
            changes += feed(&f, b % 11 == 10 ? 25000000 : NS(60), 1, NULL, &frame);
            changes += feed(&f, NS(120), 37 + b % 50, NULL, &frame);
        }
        check(changes == 1 && first == 0 && f.hz > 119 && f.hz < 121, "one-frame blips at 120 Hz: no rate change");
        check(f.rejected == 55, "one-frame blips counted (55)");
    }
    {   // short runs of 2..29 blips are still blips
        HzFilter f = {0};
        int frame = 0, changes = feed(&f, NS(120), 50, NULL, &frame);
        for (int n = 2; n < HZ_FILTER_FRAMES; n++) changes += feed(&f, NS(60), n, NULL, &frame) + feed(&f, NS(120), 10, NULL, &frame);
        check(changes == 1 && f.hz > 119, "runs of 2-29 frames at 60 Hz: no rate change");
    }
    {   // a mix of 16.67 and 25 ms in a row is no rate either
        HzFilter f = {0};
        int frame = 0, changes = feed(&f, NS(120), 50, NULL, &frame);
        for (int i = 0; i < 200; i++) changes += feed(&f, i & 1 ? 25000000 : NS(60), 1, NULL, &frame);
        check(changes == 1 && f.hz > 119, "alternating 60/40 Hz periods: no rate change");
    }
    {   // a real change, 90 -> 120 Hz, within half a second
        HzFilter f = {0};
        int frame = 0, first = -1;
        feed(&f, NS(90), 100, NULL, &frame);
        int start = frame;
        feed(&f, NS(120), 120, &first, &frame);
        double s = (first - start + 1) / 120.0;
        printf("     90 -> 120 Hz taken after %d frames (%.3f s)\n", first - start + 1, s);
        check(f.hz > 119 && f.hz < 121 && s <= 0.5, "90 -> 120 Hz picked up within 0.5 s");
    }
    {   // 120 -> 90 Hz with a blip in the middle of the new rate: still within 0.5 s
        HzFilter f = {0};
        int frame = 0, first = -1;
        feed(&f, NS(120), 100, NULL, &frame);
        int start = frame;
        feed(&f, NS(90), 10, &first, &frame);
        feed(&f, NS(45), 1, &first, &frame);
        feed(&f, NS(90), 60, &first, &frame);
        double s = (first - start + 1) / 90.0;
        printf("     120 -> 90 Hz with a blip taken after %d frames (%.3f s)\n", first - start + 1, s);
        check(f.hz > 89 && f.hz < 91 && s <= 0.5, "120 -> 90 Hz with a blip picked up within 0.5 s");
    }
    {   // no rate yet (0): the first period is taken as is; jitter under 1% is the same rate
        HzFilter f = {0};
        int frame = 0, changes = feed(&f, 8333333, 1, NULL, &frame) + feed(&f, 8380000, 50, NULL, &frame);
        check(changes == 1 && f.rejected == 0, "first period taken, sub-1% jitter ignored");
    }
    for (int i = 1; i < argc; i++) replay(argv[i]);
    printf("%s\n", failed ? "hz filter: FAILED" : "hz filter: all cases passed");
    return failed != 0;
}
