// Overworld streaming (docs/af/STREAM.md): what the Field keeps loaded around Link, and levers to show more of it in
// the VR diorama. Everything here is off unless its LARECOMP_STREAM_* variable is set.
#include "../game_addresses.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"
#include "stream.h"

// LARECOMP_STREAM_BT=<path substring>[:<count>]: the host backtrace (guest functions are f_<addr>) of the first
// <count> (default 4) romfs reads whose path contains the substring (who asks for a field room's files).
void romfs_file_read_hook(const char *path) {
    const char *bt = ENV("LARECOMP_STREAM_BT");
    if (!bt || !path) return;
    static char pat[128];
    static int left = -1;
    if (left < 0) {
        snprintf(pat, sizeof pat, "%s", bt);
        char *c = strchr(pat, ':');
        left = c ? atoi(c + 1) : 4;
        if (c) *c = 0;
    }
    if (left <= 0 || !strstr(path, pat)) return;
    left--;
    LOG("[stream] read %s from:\n", path);
    plat_print_backtrace();
}

// LARECOMP_STREAM_SCAN=<frame>:<v>[/<v>...],<frame>:<v>[/<v>...],... (research): a memory search for the variables that
// follow Link's room. Each '/'-separated column is a track (e.g. room column / row / index); at the first frame every
// aligned u16 and u32 in the game's writable memory holding the track's value is a candidate, at each later frame
// only those still holding that frame's value stay. The survivors (at most 64 per track) are logged after the last
// frame. A host thread watches the present counter, so nothing in the game's own code changes.
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>

#define SCAN_STEPS 8
#define SCAN_TRACKS 8
static int scan_n, scan_tracks;
static long scan_frame[SCAN_STEPS];
static long scan_val[SCAN_STEPS][SCAN_TRACKS];
typedef struct { uint64_t *a; size_t n, cap; } AddrVec;
static AddrVec scan_c[SCAN_TRACKS][2];  // [track][0: u16, 1: u32]

static void vec_push(AddrVec *v, uint64_t a) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 1 << 16;
        v->a = realloc(v->a, v->cap * sizeof *v->a);
    }
    v->a[v->n++] = a;
}

static void scan_first(void) {
    FILE *m = fopen("/proc/self/maps", "r");
    if (!m) return;
    char line[512];
    size_t pages = 0;
    const size_t PG = 4096;
    unsigned char *res = malloc(1 << 20);
    while (fgets(line, sizeof line, m)) {
        unsigned long long lo, hi;
        char perm[8];
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perm) != 3 || perm[0] != 'r' || perm[1] != 'w') continue;
        if (hi <= 0x7000000000ull || lo >= 0x7800000000ull) continue;  // (guest memory only)
        for (uint64_t chunk = lo; chunk < hi; chunk += (1ull << 20) * PG) {  // (resident pages only: mincore)
            uint64_t end = chunk + (1ull << 20) * PG < hi ? chunk + (1ull << 20) * PG : hi;
            if (mincore((void *)(uintptr_t)chunk, end - chunk, res) != 0) continue;
            for (uint64_t pg = chunk; pg < end; pg += PG) {
                if (!(res[(pg - chunk) / PG] & 1)) continue;
                pages++;
                const uint16_t *h = (const uint16_t *)(uintptr_t)pg;
                for (size_t i = 0; i < PG / 2; i++) {
                    uint16_t v16 = h[i];
                    if (v16 > 4096) continue;  // (all the tracked values are small)
                    for (int t = 0; t < scan_tracks; t++) {
                        long want = scan_val[0][t];
                        if (v16 != (uint16_t)want) continue;
                        vec_push(&scan_c[t][0], pg + 2 * i);
                        if (!(i & 1) && h[i + 1] == 0) vec_push(&scan_c[t][1], pg + 2 * i);
                    }
                }
            }
        }
    }
    free(res);
    fclose(m);
    LOG("[stream] scan: %zu resident guest pages searched\n", pages);
}

static void scan_filter(int step) {
    for (int t = 0; t < scan_tracks; t++)
        for (int w = 0; w < 2; w++) {
            AddrVec *v = &scan_c[t][w];
            size_t k = 0;
            for (size_t i = 0; i < v->n; i++) {
                uint64_t a = v->a[i];
                uint32_t x = w ? *(volatile uint32_t *)(uintptr_t)a : *(volatile uint16_t *)(uintptr_t)a;
                if (x == (w ? (uint32_t)scan_val[step][t] : (uint16_t)scan_val[step][t])) v->a[k++] = a;
            }
            v->n = k;
        }
}

static void *scan_thread(void *arg) {
    (void)arg;
    extern uint64_t renderer_present_count(void);
    for (int s = 0; s < scan_n;) {
        if ((long)renderer_present_count() < scan_frame[s]) { usleep(2000); continue; }
        if (s == 0) scan_first(); else scan_filter(s);
        for (int t = 0; t < scan_tracks; t++)
            LOG("[stream] scan step %d (frame %ld) track %d value %ld: %zu u16, %zu u32 candidates\n", s,
                (long)renderer_present_count(), t, scan_val[s][t], scan_c[t][0].n, scan_c[t][1].n);
        s++;
    }
    for (int t = 0; t < scan_tracks; t++)
        for (int w = 0; w < 2; w++)
            for (size_t i = 0; i < scan_c[t][w].n && i < 64; i++)
                LOG("[stream] scan track %d u%d at %#llx\n", t, w ? 32 : 16, (unsigned long long)scan_c[t][w].a[i]);
    return NULL;
}

__attribute__((constructor)) static void scan_init(void) {
    const char *e = getenv("LARECOMP_STREAM_SCAN");
    if (!e) return;
    for (const char *p = e; *p && scan_n < SCAN_STEPS;) {
        scan_frame[scan_n] = strtol(p, (char **)&p, 10);
        int t = 0;
        while (*p == ':' || *p == '/') { p++; if (t < SCAN_TRACKS) scan_val[scan_n][t++] = strtol(p, (char **)&p, 10); }
        if (!scan_n || t < scan_tracks) scan_tracks = t;
        scan_n++;
        while (*p == ',') p++;
    }
    pthread_t th;
    pthread_create(&th, NULL, scan_thread, NULL);
    pthread_detach(th);
}
