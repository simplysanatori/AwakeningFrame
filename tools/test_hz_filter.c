// The display-rate filter (runtime/xr/hz_filter.h): one-frame period blips ignored, real rate changes taken.
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

int main(int argc, char **argv) {
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
