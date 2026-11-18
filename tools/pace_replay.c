// A session's frames replayed through the frame pacer (runtime/nvn/pace.c): the per-frame "[pace] period ..." lines of a
// LARECOMP_LOG_PACE=1 log, and the VR mode's rate lines ("[xr] display at N Hz: the game ..."), in order. Prints the
// pacer's own [pace] decisions, a line per 5 s (the log's frame rate and the replay's, whether held, the GPU time) and
// the share of gameplay time held at 30 where the GPU fit the VR game frame.
// Build + run: tools/pace_replay.sh <log> [options] (or tools/test_pace.py's compile line with this file instead).
//   --as-logged  the log's frames as they were (the periods of the session's own holds included): the replay's
//                decisions next to the session's, to check the replay against the build that wrote the log
//   (default)    where the replay's hold differs from the session's, the frame is remade from its work: a held frame
//                let go at 60 takes 16.67 ms, or 33.3 when it would have been late (by its GPU time, the late shares
//                the 120 Hz sessions of 2026-11-18 showed at 60: see late60), its game thread without the second world
//                step; a frame at 60 the replay holds takes 33.3 ms with the second step added
//   --rates      the display rate of each 5 s stats line ("[xr] 5 s: ... at N Hz") passed to the pacer when it changes,
//                as an xr_loop.c that tells the pacer every rate change would (60 FPS rates: a 16.67 ms game frame); the
//                session's own build only told it when the game's rate choice changed
//   --cadence    the frames of the VR mode's 30 fps lock on a simulated 60 Hz vsync clock instead: each frame's game
//                thread works its logged busy time (plus --idle <ms> of waits inside the work, which the busy time
//                doesn't count), waits for its vsync event (the gate pace.c sets: runtime/hle/vi.c g_vsync_game_from)
//                and presents; prints the 50 ms and under-25 ms periods the replay's pacer gives next to the log's.
//                --from <text>: counted from the log line holding the text (e.g. "jumping to Field / Field_11C") on,
//                to the next warp, its first 60 frames left out (as the bench's pacestat.py)
//   -q           no per-5 s lines
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/pace.h"

// ---- what pace.c reads (as tools/test_pace.c)
static uint64_t t_ns = 1000000000ull, cpu_ns, gpu_total_ns, render_cpu_ns;
__thread uint64_t g_blocked_ns, g_rq_wait_ns, g_rq_lock_ns, g_submit_ns, g_gpu_wait_ns, g_present_ns, g_hooks_ns;
volatile uint64_t g_vsync_count, g_vsync_ns, g_vsync_game_from;
uint64_t pace_test_game_mgr;
static uint8_t game_mgr[0x500];
static bool gameplay = true;
static char level[64] = "Field";
uint64_t plat_now_ns(void) { return t_ns; }
// (the held cadence's wait is in the logged period already: no time passes here; --cadence: the clock's)
#define VSYNC_NS 16666667ull
static bool cadence;
static void vsyncs(void) { g_vsync_count = t_ns / VSYNC_NS, g_vsync_ns = g_vsync_count * VSYNC_NS; }
void plat_sleep_until_ns(uint64_t t) {
    if (!cadence) { g_vsync_count += 2; return; }
    if (t > t_ns) t_ns = t;
    vsyncs();
}
uint64_t plat_thread_cpu_ns(void) { return cpu_ns; }
uint64_t rq_render_cpu_ns(void) { return render_cpu_ns; }
int64_t pace_gpu_ns(void) { return (int64_t)gpu_total_ns; }
int game_in_play(void) { return 1; }
bool world_on_screen(void) { return gameplay; }
const char *game_level_name(void) { return level; }
uint64_t game_camera_zone(void) { return 0; }
void slow_frame_present(uint64_t now, uint64_t gcpu) { (void)now, (void)gcpu; }
int output_scale_dynamic_state(void) { return 0; }  // (the VR mode: the Video tab's Dynamic resolution is off)

// The share of frames at 60 shown late (a game vsync missed) by the GPU's time per frame (its ~5-frame average, ms) in the
// 2026-11-18 sessions at 120 Hz: 1-4% under 15 ms, 10-12% at 15-17, ~40% over 17.
static double late60(double gpu) { return gpu < 13 ? 0.015 : gpu < 15 ? 0.04 : gpu < 17 ? 0.11 : 0.40; }
static uint64_t rng = 9;
static double rnd(void) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return (rng >> 11) / (double)(1ull << 53); }

static void xr_rate(double hz, bool held30, double frame_ms, bool quiet) {
    pace_xr_lock(held30 ? 30 : 0);
    pace_xr_frame_ms(frame_ms);
    if (!quiet) printf("      (pacer told: %.0f Hz, %s, game frame %.2f ms)\n", hz, held30 ? "held at 30" : "60 free", frame_ms);
}

// --cadence (see the top): the game thread's own wait, for the next vsync that signals its event
static uint64_t game_vsync_after(uint64_t t) {
    uint64_t k = t / VSYNC_NS + 1, from = g_vsync_game_from;
    while (k < from && from - k <= 2) k++;
    return k;
}
static int replay_cadence(FILE *f, double idle, const char *from) {
    char line[4096];
    bool counting = !from, any_lock = false;
    int skip = 60, n = 0, log50 = 0, log16 = 0, r50 = 0, r16 = 0, both50 = 0;
    double lat = 0;
    uint64_t last = 0;
    vsyncs();
    while (fgets(line, sizeof line, f)) {
        if (from && strstr(line, from)) { counting = true, skip = 60; continue; }
        if (from && counting && n && strstr(line, "[warp] jumping")) counting = false;
        double h;
        if (sscanf(line, "[xr] display at %lf Hz: the game", &h) == 1) {
            bool held30 = strstr(line, "held at 30") != NULL;
            pace_xr_lock(held30 ? 30 : 0), pace_xr_frame_ms(held30 ? 0 : 2e3 / h), any_lock = held30;
            continue;
        }
        double p, b, u, c, q, cpu, rc, g;
        if (sscanf(line, "[pace] period %lf blocked %lf busy %lf cost %lf ms (queue wait %lf, cpu %lf, render cpu %lf, gpu %lf,",
                   &p, &b, &u, &c, &q, &cpu, &rc, &g) != 8 || !any_lock)
            continue;
        gameplay = !strstr(line, "(no gameplay)");
        uint64_t t0 = t_ns;
        if (u > 100) u = 100;  // (a load: not a frame's work)
        t_ns += (uint64_t)((u + idle) * 1e6);
        g_blocked_ns += (uint64_t)(idle * 1e6);
        uint64_t shown = game_vsync_after(t_ns) * VSYNC_NS;
        g_blocked_ns += shown - t_ns;
        t_ns = shown;
        vsyncs();
        cpu_ns += (uint64_t)(cpu * 1e6);
        render_cpu_ns += (uint64_t)(rc * 1e6) + 1;
        if (g >= 0) gpu_total_ns += (uint64_t)(g * 1e6);
        double rp = last ? (shown - last) / 1e6 : 0;
        last = shown;
        if (counting && gameplay && skip-- <= 0) {
            n++, lat += (shown - t0) / 1e6;
            log50 += p > 45, log16 += p < 25, r50 += rp > 45, r16 += rp < 25, both50 += p > 45 && rp > 45;
        }
        pace_frame();
    }
    printf("%d frames%s%s, idle %.1f ms a frame: the log %d at 50 ms (%.1f%%), %d under 25; the replay %d at 50 ms (%.1f%%; "
           "%d of them the log's too), %d under 25; start to present %.1f ms on average\n", n, from ? " from " : "",
           from ? from : "", idle, log50, 100.0 * log50 / (n ? n : 1), log16, r50, 100.0 * r50 / (n ? n : 1), both50, r16,
           lat / (n ? n : 1));
    return 0;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    bool as_logged = false, rates = false, quiet = false;
    const char *from = NULL;
    double idle = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--as-logged")) as_logged = true;
        else if (!strcmp(argv[i], "--cadence")) cadence = true;
        else if (!strcmp(argv[i], "--idle") && i + 1 < argc) idle = atof(argv[++i]);
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = argv[++i];
        else if (!strcmp(argv[i], "--rates")) rates = true;
        else if (!strcmp(argv[i], "-q")) quiet = true;
        else path = argv[i];
    }
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) {
        fprintf(stderr, "usage: pace_replay <log> [--as-logged] [--rates] [--cadence [--idle <ms>] [--from <text>]] [-q]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    game_mgr[0x4e3] = 1;  // (the game allows its 30 fps mode)
    pace_test_game_mgr = (uint64_t)(uintptr_t)game_mgr;
    if (cadence) return replay_cadence(f, idle, from);
    char line[4096];
    bool log_slow = false, log_lock = false;  // the session's own pacer hold, and its VR lock at 30
    double hz = 0, told_hz = 0, gpu_ema = -1, start = t_ns / 1e9;
    // per 5 s window and in all: time (s) at 60 / held, the log's and the replay's; gameplay time held where the GPU fit
    double w_t = 0, w_log_frames = 0, w_frames = 0, w_held = 0, w_log_held = 0, w_gpu = 0;
    int w_n = 0;
    double all_t = 0, all_held = 0, all_log_held = 0, fit_t = 0, fit_held = 0, fit_log_held = 0;
    double fit_lim = 1e3 / 60 * 0.90;  // (the VR release limit at 120 Hz: PACE_VR_GPU of the game frame)
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "[pace] ", 7) && strncmp(line, "[pace] period", 13)) {  // the session's own decisions
            if (strstr(line, "holding the game's 30 fps mode")) log_slow = true;
            if (strstr(line, "back to the game's own choice") || strstr(line, "trying 60")) log_slow = false;
            if (!quiet) printf("  log %6.1fs %s", t_ns / 1e9 - start, line);
            continue;
        }
        char *v = strstr(line, " level ");
        if (!strncmp(line, "[vk] t=", 7) && v) sscanf(v + 7, "%63s", level);
        double h;
        if (sscanf(line, "[xr] display at %lf Hz: the game", &h) == 1) {
            hz = told_hz = h;
            bool held30 = strstr(line, "held at 30") != NULL;
            log_lock = held30;
            double fm = strstr(line, "(1 or 2 refreshes") ? 1e3 / 60 : strstr(line, "(1 refresh ") ? 1e3 / h : held30 && !strstr(line, "(4 refreshes") ? 0 : 2e3 / h;
            if (!quiet) printf("  log %6.1fs %s", t_ns / 1e9 - start, line);
            xr_rate(h, held30, fm, quiet);
            continue;
        }
        if (rates && sscanf(line, "[xr] %*d s: %*d refreshes at %lf Hz", &h) == 1 && told_hz && fabs(h - told_hz) > 1) {
            // (the rate the stats line names, if the pacer was last told another; the game's rate by the rule of an
            // xr_loop.c that passes every change: held at 30 at a multiple of 30 that isn't one of 60, else 60)
            bool held30 = fmod(h + 0.5, 30) < 1 && fmod(h + 0.5, 60) >= 1;
            if (!quiet) printf("  rate %5.1fs the display at %.0f Hz (was %.0f)\n", t_ns / 1e9 - start, h, told_hz);
            hz = told_hz = h;
            log_lock = held30;
            xr_rate(h, held30, held30 ? 0 : 1e3 / 60, quiet);
            continue;
        }
        double p, b, u, c, q, cpu, rc, g;
        if (sscanf(line, "[pace] period %lf blocked %lf busy %lf cost %lf ms (queue wait %lf, cpu %lf, render cpu %lf, gpu %lf,",
                   &p, &b, &u, &c, &q, &cpu, &rc, &g) != 8)
            continue;
        gameplay = !strstr(line, "(no gameplay)");
        if (g >= 0) gpu_ema = gpu_ema < 0 ? g : gpu_ema + (g - gpu_ema) * 0.2;
        bool log_held = log_slow || log_lock, held = game_mgr[0x4e2] != 0;
        if (!as_logged && held != log_held && p < 100) {  // (remade from its work: see the top)
            if (held) {  // the session ran it at 60, the replay holds it: two world steps, 33.3 ms
                double p30 = p > 33.4 ? ceil(p / 16.667) * 16.667 : 33.333;
                u += cpu, cpu *= 2, p = p30, b = p30 - u;
            } else {  // the session held it, the replay lets it go: one world step, 16.67 ms unless late at 60
                double step2 = fmin(cpu / 2, u / 2);
                u -= step2, cpu -= step2;
                p = rnd() < late60(gpu_ema) || u > 16.2 ? 33.333 : 16.667;
                if (u > p) p = ceil(u / 16.667) * 16.667;
                b = p - u;
            }
        }
        t_ns += (uint64_t)(p * 1e6);
        g_blocked_ns += (uint64_t)(b * 1e6);
        cpu_ns += (uint64_t)(cpu * 1e6);
        render_cpu_ns += (uint64_t)(rc * 1e6) + 1;  // (+1: a render thread)
        if (g >= 0) gpu_total_ns += (uint64_t)(g * 1e6);
        g_vsync_count++;
        bool was = pace_is_slow();
        pace_frame();
        if (pace_is_slow() != was && !quiet) printf("  replay %6.1fs %s\n", t_ns / 1e9 - start, pace_is_slow() ? "HELD" : "BACK at 60");
        held = game_mgr[0x4e2] != 0;
        double s = p / 1e3;
        w_t += s, w_frames++, w_log_frames += 1, w_gpu += gpu_ema, w_n++;
        if (held) w_held += s;
        if (log_held) w_log_held += s;
        if (gameplay) {
            all_t += s, all_held += held ? s : 0, all_log_held += log_held ? s : 0;
            if (gpu_ema >= 0 && gpu_ema < fit_lim && hz >= 100) fit_t += s, fit_held += held ? s : 0, fit_log_held += log_held ? s : 0;
        }
        if (w_t >= 5) {
            if (!quiet)
                printf("  %6.1fs %3.0f Hz: held %3.0f%% (log %3.0f%%), %4.1f fps, gpu %4.1f ms\n", t_ns / 1e9 - start, hz,
                       100 * w_held / w_t, 100 * w_log_held / w_t, w_frames / w_t, w_gpu / w_n);
            w_t = w_frames = w_log_frames = w_held = w_log_held = w_gpu = 0, w_n = 0;
        }
    }
    printf("gameplay %.0f s: held at 30 %.0f%% (the session %.0f%%); at 100 Hz and up with the GPU under %.1f ms "
           "(%.0f s): held %.0f%% (the session %.0f%%)\n", all_t, 100 * all_held / (all_t ? all_t : 1),
           100 * all_log_held / (all_t ? all_t : 1), fit_lim, fit_t, 100 * fit_held / (fit_t ? fit_t : 1),
           100 * fit_log_held / (fit_t ? fit_t : 1));
    return 0;
}
