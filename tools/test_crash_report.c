// Crash reports (runtime/crash_report.c), Linux: the record a crash writes, what the next start makes of it (the
// crashed run's log found by its pid, the GPU and driver, the last 2,000 lines, the home folder, user name and network
// addresses replaced), the DSN's endpoint, the debug IDs, and Send Report's envelope and curl call (curl stubbed: nothing
// is sent). The event's JSON goes to argv[1] for a JSON check.
//   clang -std=gnu11 -O1 -Iruntime -Iruntime/nvn -Wl,--build-id -rdynamic -DAF_SENTRY_DSN='"https://key1@example.invalid/42"' \
//         tools/test_crash_report.c -pthread -ldl -o build/test-crash-report && build/test-crash-report build/crash-event.json
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "platform.h"

static char g_root[512];
const char *rt_data_root(void) { return g_root; }
bool plat_exe_path(char *buf, size_t size) { snprintf(buf, size, "%s/app/lasrecomp", g_root); return true; }
const char *plat_symbol(uint64_t pc, uint64_t *offset) { (void)pc, (void)offset; return NULL; }
void plat_system_info(PlatSysInfo *si) { snprintf(si->os, sizeof si->os, "TestOS 1.0 (aarch64)"); }
uint64_t plat_now_ns(void) { return 1; }
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) { (void)pc, (void)offset; return NULL; }
const char *rt_crash_line(uint64_t *pc) { *pc = 0; return "signal 11 accessing 0x8 in /home/someone/x\n"; }
void rt_crash_headline(char *out, size_t n) { snprintf(out, n, "The game crashed"); }
static char g_argv[16][1400], g_envelope[1 << 20];
static int g_argc;
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)log_path, (void)append_log;
    for (g_argc = 0; argv[g_argc] && g_argc < 16; g_argc++) snprintf(g_argv[g_argc], sizeof g_argv[0], "%s", argv[g_argc]);
    FILE *f = fopen(g_argv[g_argc - 2] + 1, "rb");  // (--data-binary @file)
    if (f) g_envelope[fread(g_envelope, 1, sizeof g_envelope - 1, f)] = 0, fclose(f);
    return 1;
}
int plat_proc_wait(PlatProc p, bool block) { (void)p, (void)block; return 0; }

#include "../runtime/crash_report.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    static char b[1 << 20];
    b[fread(b, 1, sizeof b - 1, f)] = 0;
    fclose(f);
    return strdup(b);
}

int main(int argc, char **argv) {
    char tmpl[] = "/tmp/crashreport-XXXXXX";
    if (!mkdtemp(tmpl)) return 2;
    snprintf(g_root, sizeof g_root, "%s", tmpl);
    char p[1200];
    setenv("HOME", "/home/someone", 1);
    setenv("USER", "someone", 1);
    setenv("LARECOMP_XR", "1", 1);

    crash_report_init();
    CHECK(!crash_report_pending(), "pending with no record");
    CHECK(!strncmp(g_release, "awakeningframe@", 15) && !strcmp(g_env, "development"), "release %s %s", g_release, g_env);
    crash_report_record();
    snprintf(p, sizeof p, "%s/crash-reports/last-crash.txt", g_root);
    char *rec = slurp(p);
    CHECK(rec && strstr(rec, "mode: vr\n") && strstr(rec, "reason: The game crashed\n") && strstr(rec, "\nframe: 0x"),
          "record: %s", rec ? rec : "(none)");
    CHECK(rec && strstr(rec, "\nmodule: 0x") && !strstr(rec, " - "), "a module without its build ID: %s", rec ? rec : "");

    // the crashed run's log, as this start moved it aside: [vk] lines early, 2,500 lines, names and addresses at the end
    snprintf(p, sizeof p, "%s/lasrecomp-backup-20261116-100000-%d.log", g_root, (int)getpid());
    FILE *f = fopen(p, "w");
    fprintf(f, "[log] 2026-11-16 10:00:00, pid %d\n[vk] using Turnip Adreno (TM) 750\n"
               "[vk] driver turnip Mesa 26.3.0; graphics pipeline library: yes\nFIRSTLINE\n", (int)getpid());
    for (int i = 0; i < 2500; i++) fprintf(f, "filler %d\n", i);
    fprintf(f, "data /home/someone/.local/share/lasrecomp user someone@ ip 192.0.2.20 mac 12:34:56:78:9a:bc "
               "v6 fe80::1c2b:3a4d and 2001:db8:0:0:0:0:0:1 at 10:00:00 std::string version 26.3.0\n");
    fclose(f);
    crash_report_init();
    CHECK(crash_report_pending(), "not pending after a crash");
    const char *t = crash_report_text();
    CHECK(strstr(t, "GPU: Turnip Adreno (TM) 750") && strstr(t, "Driver: turnip Mesa 26.3.0\n"), "gpu / driver:\n%s", t);
    CHECK(strstr(t, "Mode: VR") && strstr(t, "Release: awakeningframe@"), "head");
    CHECK(!strstr(t, "FIRSTLINE") && !strstr(t, "filler 500\n") && strstr(t, "filler 501\n"), "not the last 2,000 lines");
    CHECK(!strstr(t, "/home/someone") && !strstr(t, "someone") && strstr(t, "data ~/.local/share/lasrecomp user <user>@"),
          "home / user left in");
    CHECK(strstr(t, "ip <ip> mac <mac> v6 <ip> and <ip> at 10:00:00 std::string version 26.3.0"), "network:\n%s",
          strstr(t, "data ~") ? strstr(t, "data ~") : t);
    CHECK(strstr(t, "Error: signal 11 accessing 0x8 in ~/x"), "error not scrubbed");
    snprintf(p, sizeof p, "%s/crash-reports/last-crash.txt", g_root);
    CHECK(access(p, F_OK) != 0, "the record is still there: it would be asked about again");

    char url[700], key[128];
    CHECK(parse_dsn(url, sizeof url, key, sizeof key) && !strcmp(url, "https://example.invalid/api/42/envelope/") &&
          !strcmp(key, "key1"), "dsn: %s %s", url, key);
    char did[40];
    debug_id("0123456789abcdef0123456789abcdef01234567", did, sizeof did);
    CHECK(!strcmp(did, "67452301-ab89-efcd-0123-456789abcdef"), "debug id %s", did);

    Str ev = {0};
    event_json(&ev, "0123456789abcdef0123456789abcdef");
    CHECK(strstr(ev.s, "\"type\":\"signal 11\"") && strstr(ev.s, "\"mode\":\"vr\"") &&
          strstr(ev.s, "\"infer_ip\":\"never\"") && strstr(ev.s, "\"debug_id\":\"") && !strstr(ev.s, "someone"),
          "event: %s", ev.s);
    if (argc > 1 && (f = fopen(argv[1], "w"))) fputs(ev.s, f), fclose(f);

    crash_report_send();
    char msg[160];
    for (int i = 0; i < 500 && crash_report_state(msg, sizeof msg) == 1; i++) usleep(10000);
    CHECK(crash_report_state(msg, sizeof msg) == 2, "not sent: %s", msg);
    CHECK(g_argc == 14 && !strcmp(g_argv[0], "/usr/bin/curl") && !strcmp(g_argv[13], url) &&
          strstr(g_argv[10], "sentry_key=key1"), "curl call");
    CHECK(!strncmp(g_envelope, "{\"event_id\":\"", 13) && strstr(g_envelope, "{\"type\":\"event\",\"length\":") &&
          strstr(g_envelope, "\"filename\":\"report.txt\"") && strstr(g_envelope, "\"filename\":\"lasrecomp.log\"") &&
          !strstr(g_envelope, "someone"), "envelope");
    snprintf(p, sizeof p, "%s/crash-reports/envelope.tmp", g_root);
    CHECK(access(p, F_OK) != 0, "the envelope was left behind");

    snprintf(p, sizeof p, "rm -rf '%s'", g_root);
    if (system(p)) {}
    printf("crash report tests: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
