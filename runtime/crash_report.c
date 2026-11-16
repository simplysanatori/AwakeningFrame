// AwakeningFrame: crash reports the player chooses to send (docs/TECHNICAL.md "Crash reports").
// A fatal error (rt_fatal_trace) writes crash-reports/last-crash.txt in the data folder: the fatal line and its plain
// words (crash_explain.c), VR or flat, the stack as addresses and the modules they are in with their GNU build IDs.
// The next start reads it and removes it, so it is asked about once. When this build can send it, main.c shows the
// start screen with the question (launcher.mm crash_report_popup): Don't Send (the default), See What's Sent, Send
// Report. Only Send Report sends, and only that report, to the Sentry project this build has: AF_SENTRY_DSN at build
// time (Makefile), else a sentry-dsn file next to the program (steam-frame/build.sh puts one in a release). Without one
// nothing is asked. The report goes with the system's curl, as the start screen's update check does: no Sentry SDK,
// no crash handler of Sentry's, nothing in the background. The home folder, the user name and the host name are
// replaced everywhere before it is shown, and IP and MAC addresses in the log.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "crash_report.h"

#ifndef __linux__
void crash_report_init(void) {}
void crash_report_record(void) {}
void crash_report_test_arm(void) {}
bool crash_report_pending(void) { return false; }
const char *crash_report_reason(void) { return ""; }
const char *crash_report_text(void) { return ""; }
void crash_report_send(void) {}
int crash_report_state(char *msg, size_t n) { if (n) msg[0] = 0; return 0; }
void crash_report_done(void) {}
#else
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "paths.h"
#include "platform.h"
#include "rt.h"

#ifndef AF_SENTRY_DSN
#define AF_SENTRY_DSN ""
#endif
#ifndef AF_GIT_SHA
#define AF_GIT_SHA ""
#endif

enum { LOG_LINES = 2000, MAX_FRAMES = 64, MAX_MODS = 32 };

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c: f_<guest address>

static char g_release[96], g_env[16], g_dsn[512], g_dir[1100];

// ---- a growing string
typedef struct { char *s; size_t n, cap; } Str;
static void str_add(Str *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->s, cap);
        if (!p) return;
        b->s = p, b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void str_cat(Str *b, const char *s) { str_add(b, s, strlen(s)); }
static void __attribute__((format(printf, 2, 3))) str_fmt(Str *b, const char *fmt, ...) {
    char t[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n > 0) str_add(b, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}
// a JSON string, at most max bytes of s (0: all; never inside a UTF-8 character)
static void str_json(Str *b, const char *s, size_t max) {
    size_t len = strlen(s);
    if (max && len > max) {
        len = max;
        while (len && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
    }
    str_cat(b, "\"");
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        char e[8] = {(char)c, 0};
        if (c == '"' || c == '\\') e[0] = '\\', e[1] = (char)c, e[2] = 0;
        else if (c == '\n') strcpy(e, "\\n");
        else if (c < 0x20) snprintf(e, sizeof e, "\\u%04x", c);
        str_cat(b, e);
    }
    str_cat(b, "\"");
}

static void first_line(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    size_t l = strlen(out);
    while (l && isspace((unsigned char)out[l - 1])) out[--l] = 0;
}

// ---- what identifies the player, replaced: the home folder, the user name, the host name; in the log also IPv4, IPv6
// and MAC addresses
static char g_home[512], g_user[64], g_host[128];

static void scrub_init(void) {
    const char *h = getenv("HOME");
    if (h && strlen(h) > 1) snprintf(g_home, sizeof g_home, "%s", h);
    size_t l = strlen(g_home);
    while (l > 1 && g_home[l - 1] == '/') g_home[--l] = 0;
    const char *u = getenv("USER");
    if (!u || !*u) u = getenv("LOGNAME");
    // (SteamOS's own account names are words of its own too: "steamos" in kernel and package names)
    if (u && strlen(u) >= 3 && strcasecmp(u, "steamos") && strcasecmp(u, "deck")) snprintf(g_user, sizeof g_user, "%s", u);
    if (gethostname(g_host, sizeof g_host - 1) || strlen(g_host) < 3 || !strcasecmp(g_host, "steamos")) g_host[0] = 0;
}

static void replace_into(Str *o, const char *in, const char *from, const char *to) {
    size_t fl = strlen(from);
    for (const char *p = in, *q; *p; p = q + fl) {
        q = fl ? strstr(p, from) : NULL;
        if (!q) { str_cat(o, p); break; }
        str_add(o, p, (size_t)(q - p));
        str_cat(o, to);
    }
}

static size_t match_mac(const char *p) {  // aa:bb:cc:dd:ee:ff (or with -)
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)p[i * 3]) || !isxdigit((unsigned char)p[i * 3 + 1])) return 0;
        if (i < 5 && p[i * 3 + 2] != ':' && p[i * 3 + 2] != '-') return 0;
    }
    return isalnum((unsigned char)p[17]) || p[17] == ':' ? 0 : 17;
}
static size_t match_ipv4(const char *p) {
    const char *s = p;
    for (int i = 0; i < 4; i++) {
        int v = 0, d = 0;
        while (isdigit((unsigned char)*s) && d < 4) v = v * 10 + (*s++ - '0'), d++;
        if (!d || d > 3 || v > 255) return 0;
        if (i < 3 && *s++ != '.') return 0;
    }
    return isalnum((unsigned char)*s) || (*s == '.' && isdigit((unsigned char)s[1])) ? 0 : (size_t)(s - p);
}
static size_t match_ipv6(const char *p) {  // a run of hex digits and colons: with "::", or eight groups
    const char *s = p;
    int colons = 0, digits = 0;
    while (isxdigit((unsigned char)*s) || *s == ':') colons += *s == ':', digits += isdigit((unsigned char)*s) != 0, s++;
    if (isalnum((unsigned char)*s) || *s == '_' || !digits) return 0;
    bool dbl = false;
    for (const char *q = p; q + 1 < s; q++) dbl |= q[0] == ':' && q[1] == ':';
    return (dbl && colons >= 2) || colons == 7 ? (size_t)(s - p) : 0;
}

// in, with the player's names replaced (and network addresses: net), into a new string
static char *scrub(const char *in, bool net) {
    Str a = {0}, b = {0}, c = {0};
    if (g_home[0]) replace_into(&a, in, g_home, "~");
    else str_cat(&a, in);
    if (!a.s) return strdup("");
    if (g_user[0]) replace_into(&b, a.s, g_user, "<user>");
    else str_cat(&b, a.s);
    free(a.s);
    if (g_host[0]) replace_into(&c, b.s, g_host, "<host>");
    else str_cat(&c, b.s);
    free(b.s);
    if (!net || !c.s) return c.s ? c.s : strdup("");
    Str o = {0};
    for (const char *p = c.s; *p;) {
        char before = p > c.s ? p[-1] : ' ';
        if (!isalnum((unsigned char)before) && before != ':' && before != '.' && before != '_') {
            size_t n;
            if ((n = match_mac(p)) || (n = match_ipv4(p)) || (n = match_ipv6(p))) {
                str_cat(&o, match_mac(p) ? "<mac>" : "<ip>");
                p += n;
                continue;
            }
        }
        str_add(&o, p, 1);
        p++;
    }
    free(c.s);
    return o.s ? o.s : strdup("");
}

// ---- the crashed run's record
typedef struct { uint64_t base, size; char id[80], path[512]; } Mod;
typedef struct { uint64_t addr, off; char sym[96]; } Frame;
static struct {
    char release[96], env[16], mode[8], reason[160], error[512];
    int pid;
    long long time;
    Frame f[MAX_FRAMES + 1];
    int nf;
    Mod m[MAX_MODS];
    int nm;
} g_rec;
static bool g_pending;
static char *g_report, *g_log, *g_text;  // (scrubbed) report.txt, the log's last lines, both as See What's Sent shows them
static char g_gpu[160], g_driver[200], g_os[160];

// dl_iterate_phdr: the module whose first page is m->base (dladdr's dli_fbase): its size and GNU build ID
static int find_mod(struct dl_phdr_info *info, size_t sz, void *arg) {
    (void)sz;
    Mod *m = arg;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr < lo) lo = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > hi) hi = ph->p_vaddr + ph->p_memsz;
    }
    if (lo == UINT64_MAX || ((info->dlpi_addr + lo) & ~0xFFFull) != m->base) return 0;
    m->size = hi - (lo & ~0xFFFull);
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_NOTE) continue;
        const char *p = (const char *)(uintptr_t)(info->dlpi_addr + ph->p_vaddr), *end = p + ph->p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const ElfW(Nhdr) *nh = (const void *)p;
            const char *name = p + sizeof *nh;
            const unsigned char *desc = (const unsigned char *)name + ((nh->n_namesz + 3) & ~3u);
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && !memcmp(name, "GNU", 4)) {
                for (unsigned j = 0; j < nh->n_descsz && j < 32; j++) sprintf(m->id + 2 * j, "%02x", desc[j]);
                return 1;
            }
            p = (const char *)desc + ((nh->n_descsz + 3) & ~3u);
        }
    }
    return 1;
}

static atomic_int g_recording;
void crash_report_record(void) {
    if (atomic_fetch_add(&g_recording, 1) || !g_dir[0]) return;
    mkdir(g_dir, 0755);
    char tmp[1200], path[1200];
    snprintf(tmp, sizeof tmp, "%s/last-crash.tmp", g_dir);
    snprintf(path, sizeof path, "%s/last-crash.txt", g_dir);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    uint64_t pc = 0;
    const char *line = rt_crash_line(&pc);
    char head[160];
    rt_crash_headline(head, sizeof head);
    const char *xr = getenv("LARECOMP_XR");
    fprintf(f, "release: %s\nenvironment: %s\nmode: %s\npid: %d\ntime: %lld\nreason: %s\nerror: %.*s\n", g_release, g_env,
            xr && *xr && strcmp(xr, "0") ? "vr" : "flat", (int)getpid(), (long long)time(NULL), head,
            (int)strcspn(line, "\n"), line);
    // the stack from the crash's own pc (the signal handler's frames before it go); a FATAL from where it was called
    void *bt[MAX_FRAMES];
    int n = backtrace(bt, MAX_FRAMES), first = 0;
    if (pc) {
        first = -1;
        for (int i = 0; i < n && first < 0; i++)
            if ((uint64_t)(uintptr_t)bt[i] == pc) first = i;
    }
    Mod mods[MAX_MODS];
    int nm = 0;
    for (int i = first; i < n; i++) {
        uint64_t a = i < 0 ? pc : (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = rt_host_fn_name(a, &off);
        if (!s) s = plat_symbol(a, &off);
        if (s) fprintf(f, "frame: %#llx %s+%#llx\n", (unsigned long long)a, s, (unsigned long long)off);
        else fprintf(f, "frame: %#llx\n", (unsigned long long)a);
        Dl_info di = {0};
        if (!dladdr((void *)(uintptr_t)a, &di) || !di.dli_fbase) continue;
        uint64_t base = (uint64_t)(uintptr_t)di.dli_fbase;
        int k = 0;
        while (k < nm && mods[k].base != base) k++;
        if (k == nm && nm < MAX_MODS) {
            mods[nm] = (Mod){.base = base};
            snprintf(mods[nm].path, sizeof mods[nm].path, "%s", di.dli_fname ? di.dli_fname : "?");
            nm++;
        }
    }
    for (int k = 0; k < nm; k++) {
        dl_iterate_phdr(find_mod, &mods[k]);
        fprintf(f, "module: %#llx %#llx %s %s\n", (unsigned long long)mods[k].base, (unsigned long long)mods[k].size,
                mods[k].id[0] ? mods[k].id : "-", mods[k].path);
    }
    fclose(f);
    rename(tmp, path);
    fprintf(stderr, "[crash-report] recorded for the next start: %s\n", path);
}

// The crashed run's log: moved aside by this start (posix_log.h) as lasrecomp-backup-<YYYYMMDD-HHMMSS>-<pid>[-<n>].log
static bool find_log(int pid, char *out, size_t n) {
    DIR *d = opendir(rt_data_root());
    if (!d) return false;
    char want[24];
    snprintf(want, sizeof want, "-%d", pid);
    size_t wl = strlen(want);
    time_t best = 0;
    for (struct dirent *e; (e = readdir(d));) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (strncmp(nm, "lasrecomp-backup-", 17) || l < 32 + wl + 4 || strcmp(nm + l - 4, ".log") ||
            strncmp(nm + 32, want, wl) || (nm[32 + wl] != '.' && nm[32 + wl] != '-'))
            continue;
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", rt_data_root(), nm);
        if (stat(p, &st) == 0 && st.st_mtime >= best) best = st.st_mtime, snprintf(out, n, "%s", p);
    }
    closedir(d);
    return best != 0;
}

// The log's last LOG_LINES lines, and the GPU and driver from its "[vk] using" and "[vk] driver" lines (device.c)
static void read_log(const char *path, Str *tail) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    long count = 0;
    while (fgets(line, sizeof line, f)) {
        count++;
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "[vk] using ", 11)) snprintf(g_gpu, sizeof g_gpu, "%s", line + 11);
        else if (!strncmp(line, "[vk] driver ", 12)) snprintf(g_driver, sizeof g_driver, "%.*s", (int)strcspn(line + 12, ";"), line + 12);
    }
    rewind(f);
    long skip = count > LOG_LINES ? count - LOG_LINES : 0;
    for (long i = 0; fgets(line, sizeof line, f); i++)
        if (i >= skip) str_cat(tail, line);
    fclose(f);
}

static void load(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/last-crash.txt", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *v = strchr(line, ':');
        if (!v) continue;
        *v++ = 0;
        while (*v == ' ') v++;
        if (!strcmp(line, "release")) snprintf(g_rec.release, sizeof g_rec.release, "%s", v);
        else if (!strcmp(line, "environment")) snprintf(g_rec.env, sizeof g_rec.env, "%s", v);
        else if (!strcmp(line, "mode")) snprintf(g_rec.mode, sizeof g_rec.mode, "%s", v);
        else if (!strcmp(line, "reason")) snprintf(g_rec.reason, sizeof g_rec.reason, "%s", v);
        else if (!strcmp(line, "error")) snprintf(g_rec.error, sizeof g_rec.error, "%s", v);
        else if (!strcmp(line, "pid")) g_rec.pid = atoi(v);
        else if (!strcmp(line, "time")) g_rec.time = atoll(v);
        else if (!strcmp(line, "frame") && g_rec.nf < MAX_FRAMES + 1) {
            Frame *fr = &g_rec.f[g_rec.nf];
            char *e;
            fr->addr = strtoull(v, &e, 16);
            if (e == v) continue;
            while (*e == ' ') e++;
            char *plus = strrchr(e, '+');
            if (plus) fr->off = strtoull(plus + 1, NULL, 16), *plus = 0;
            snprintf(fr->sym, sizeof fr->sym, "%s", e);
            g_rec.nf++;
        } else if (!strcmp(line, "module") && g_rec.nm < MAX_MODS) {
            Mod *m = &g_rec.m[g_rec.nm];
            unsigned long long base, size;
            int at = 0;
            if (sscanf(v, "%llx %llx %79s %n", &base, &size, m->id, &at) < 3 || !at) continue;
            m->base = base, m->size = size;
            if (!strcmp(m->id, "-")) m->id[0] = 0;
            snprintf(m->path, sizeof m->path, "%s", v + at);
            g_rec.nm++;
        }
    }
    fclose(f);
    remove(path);  // (asked about once: answered or not)
    LOG("[crash-report] the last run (pid %d, %s) ended with an error: %s (%s)\n", g_rec.pid, g_rec.release, g_rec.reason,
        g_rec.error);
    if (!g_dsn[0]) {
        LOG("[crash-report] this build has no Sentry DSN: no Send Report\n");
        return;
    }

    scrub_init();
    PlatSysInfo si = {0};
    plat_system_info(&si);
    snprintf(g_os, sizeof g_os, "%s", si.os);
    Str log = {0};
    char lp[1400];
    if (g_rec.pid > 0 && find_log(g_rec.pid, lp, sizeof lp)) read_log(lp, &log);
    g_log = scrub(log.s ? log.s : "", true);
    free(log.s);

    Str r = {0};
    char when[64] = "";
    time_t t = (time_t)g_rec.time;
    strftime(when, sizeof when, "%Y-%m-%d %H:%M UTC", gmtime(&t));
    str_fmt(&r, "AwakeningFrame crash report\n\nRelease: %s (%s)\nWhat happened: %s\nError: %s\nMode: %s\nTime: %s\n",
            g_rec.release, g_rec.env, g_rec.reason, g_rec.error, strcmp(g_rec.mode, "vr") ? "Flat" : "VR", when);
    str_fmt(&r, "System: %s\nGPU: %s\nDriver: %s\n\nStack (the crash first):\n", g_os[0] ? g_os : "unknown",
            g_gpu[0] ? g_gpu : "unknown", g_driver[0] ? g_driver : "unknown");
    for (int i = 0; i < g_rec.nf; i++) {
        const Frame *fr = &g_rec.f[i];
        const char *mod = "?";
        uint64_t moff = 0;
        for (int k = 0; k < g_rec.nm; k++)
            if (fr->addr >= g_rec.m[k].base && fr->addr < g_rec.m[k].base + g_rec.m[k].size) {
                mod = strrchr(g_rec.m[k].path, '/') ? strrchr(g_rec.m[k].path, '/') + 1 : g_rec.m[k].path;
                moff = fr->addr - g_rec.m[k].base;
            }
        str_fmt(&r, "  %#018llx  %s+%#llx", (unsigned long long)fr->addr, mod, (unsigned long long)moff);
        if (fr->sym[0]) str_fmt(&r, "  %s+%#llx", fr->sym, (unsigned long long)fr->off);
        str_cat(&r, "\n");
    }
    str_cat(&r, "\nModules:\n");
    for (int k = 0; k < g_rec.nm; k++)
        str_fmt(&r, "  %#llx %#llx %s %s\n", (unsigned long long)g_rec.m[k].base, (unsigned long long)g_rec.m[k].size,
                g_rec.m[k].id[0] ? g_rec.m[k].id : "(no build ID)", g_rec.m[k].path);
    g_report = scrub(r.s ? r.s : "", false);
    free(r.s);

    Str all = {0};
    str_cat(&all, g_report);
    str_fmt(&all, "\n\nlasrecomp.log, the last %d lines:\n\n", LOG_LINES);
    str_cat(&all, g_log[0] ? g_log : "(no log found)\n");
    g_text = all.s;
    g_pending = true;
}

void crash_report_init(void) {
    char exe[1024], dir[1024] = "", p[1100], ver[64] = "";
    if (plat_exe_path(exe, sizeof exe) && strrchr(exe, '/')) {
        snprintf(dir, sizeof dir, "%.*s", (int)(strrchr(exe, '/') - exe), exe);
        snprintf(p, sizeof p, "%s/VERSION", dir);  // (a release: install.sh copies it next to the program)
        first_line(p, ver, sizeof ver);
    }
    snprintf(g_dsn, sizeof g_dsn, "%s", AF_SENTRY_DSN);
    if (!g_dsn[0] && dir[0]) {
        snprintf(p, sizeof p, "%s/sentry-dsn", dir);
        first_line(p, g_dsn, sizeof g_dsn);
    }
    if (ver[0]) snprintf(g_release, sizeof g_release, "awakeningframe@%s", ver), snprintf(g_env, sizeof g_env, "production");
    else snprintf(g_release, sizeof g_release, "awakeningframe@%s", AF_GIT_SHA[0] ? AF_GIT_SHA : "unknown"),
         snprintf(g_env, sizeof g_env, "development");
    snprintf(g_dir, sizeof g_dir, "%s/crash-reports", rt_data_root());
    load();
}

bool crash_report_pending(void) { return g_pending; }
const char *crash_report_reason(void) { return g_rec.reason; }
const char *crash_report_text(void) { return g_text ? g_text : ""; }
void crash_report_done(void) { g_pending = false; }

// ---- Send Report: one envelope (the event, report.txt and the log as attachments) to the DSN's project
static atomic_int g_state;
static char g_msg[160];

// https://<key>@<host>/[<prefix>/]<project> -> https://<host>[/<prefix>]/api/<project>/envelope/, and the key
static bool parse_dsn(char *url, size_t un, char *key, size_t kn) {
    const char *s = g_dsn, *sch = strstr(s, "://"), *at = strchr(s, '@');
    if (!sch || !at || at < sch || strncmp(s, "https://", 8)) return false;
    const char *host = at + 1, *slash = strchr(host, '/'), *last = strrchr(host, '/');
    if (!slash || !last[1]) return false;
    int kl = (int)strcspn(sch + 3, ":@");
    snprintf(key, kn, "%.*s", kl, sch + 3);
    snprintf(url, un, "https://%.*s%.*s/api/%s/envelope/", (int)(slash - host), host, (int)(last - slash), slash, last + 1);
    return kl > 0;
}

// Sentry's debug ID of an ELF file: its build ID's first 16 bytes as a little-endian GUID
static void debug_id(const char *build_id, char *out, size_t n) {
    unsigned char b[16] = {0};
    for (int i = 0; i < 16 && build_id[2 * i] && build_id[2 * i + 1]; i++) sscanf(build_id + 2 * i, "%2hhx", &b[i]);
    snprintf(out, n, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[3], b[2], b[1], b[0], b[5],
             b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void event_json(Str *b, const char *id) {
    char *err = scrub(g_rec.error, false), *gpu = scrub(g_gpu, false), *drv = scrub(g_driver, false);
    str_fmt(b, "{\"event_id\":\"%s\",\"timestamp\":%lld,\"platform\":\"native\",\"level\":\"fatal\","
               "\"logger\":\"awakeningframe\",\"release\":", id, g_rec.time);
    str_json(b, g_rec.release[0] ? g_rec.release : g_release, 0);
    str_cat(b, ",\"environment\":");
    str_json(b, g_rec.env[0] ? g_rec.env : g_env, 0);
    str_cat(b, ",\"message\":{\"formatted\":");
    char msg[700];
    snprintf(msg, sizeof msg, "%s: %s", g_rec.reason, err);
    str_json(b, msg, 0);
    str_cat(b, "},\"tags\":{\"report\":\"crash\",\"mode\":");
    str_json(b, strcmp(g_rec.mode, "vr") ? "flat" : "vr", 0);
    str_cat(b, ",\"reason\":");
    str_json(b, g_rec.reason, 200);
    if (gpu[0]) str_cat(b, ",\"gpu\":"), str_json(b, gpu, 200);
    if (drv[0]) str_cat(b, ",\"driver\":"), str_json(b, drv, 200);
    if (g_os[0]) str_cat(b, ",\"os\":"), str_json(b, g_os, 200);
    // the crash: a signal's type is "signal N", a FATAL's "fatal error"; frames oldest first, as Sentry wants them
    char type[32] = "fatal error";
    if (!strncmp(g_rec.error, "signal ", 7))
        snprintf(type, sizeof type, "%.*s", 7 + (int)strcspn(g_rec.error + 7, " "), g_rec.error);
    str_cat(b, "},\"exception\":{\"values\":[{\"type\":");
    str_json(b, type, 0);
    str_cat(b, ",\"value\":");
    str_json(b, err, 0);
    str_fmt(b, ",\"mechanism\":{\"type\":\"%s\",\"handled\":false},\"stacktrace\":{\"frames\":[",
            strncmp(g_rec.error, "signal ", 7) ? "generic" : "signalhandler");
    for (int i = g_rec.nf - 1; i >= 0; i--) {
        const Frame *fr = &g_rec.f[i];
        str_fmt(b, "%s{\"instruction_addr\":\"%#llx\"", i == g_rec.nf - 1 ? "" : ",", (unsigned long long)fr->addr);
        for (int k = 0; k < g_rec.nm; k++)
            if (fr->addr >= g_rec.m[k].base && fr->addr < g_rec.m[k].base + g_rec.m[k].size) {
                char *pk = scrub(g_rec.m[k].path, false);
                str_cat(b, ",\"package\":");
                str_json(b, pk, 0);
                str_fmt(b, ",\"image_addr\":\"%#llx\"", (unsigned long long)g_rec.m[k].base);
                free(pk);
                break;
            }
        if (fr->sym[0]) str_cat(b, ",\"function\":"), str_json(b, fr->sym, 0);
        str_cat(b, "}");
    }
    str_cat(b, "]}}]},\"debug_meta\":{\"images\":[");
    for (int k = 0; k < g_rec.nm; k++) {
        const Mod *m = &g_rec.m[k];
        char *pk = scrub(m->path, false);
        str_fmt(b, "%s{\"type\":\"elf\",\"code_file\":", k ? "," : "");
        str_json(b, pk, 0);
        str_fmt(b, ",\"image_addr\":\"%#llx\",\"image_size\":%llu", (unsigned long long)m->base, (unsigned long long)m->size);
        if (m->id[0]) {
            char did[40];
            debug_id(m->id, did, sizeof did);
            str_fmt(b, ",\"code_id\":\"%s\",\"debug_id\":\"%s\"", m->id, did);
        }
        str_cat(b, "}");
        free(pk);
    }
    // no user, no place, no IP: Sentry is asked not to take the sender's address for the event (and the project is set
    // not to store IP addresses); without an empty geo it derives a city from the request
    str_cat(b, "]},\"user\":{\"geo\":{}},\"sdk\":{\"name\":\"awakeningframe.crash-report\",\"version\":\"1\","
               "\"settings\":{\"infer_ip\":\"never\"}}}");
    free(err), free(gpu), free(drv);
}

static void envelope_item(Str *e, const char *filename, const char *data) {
    size_t n = strlen(data);
    str_fmt(e, "{\"type\":\"attachment\",\"length\":%zu,\"filename\":\"%s\",\"content_type\":\"text/plain\"}\n", n, filename);
    str_add(e, data, n);
    str_cat(e, "\n");
}

static void *send_thread(void *arg) {
    (void)arg;
    char url[700], key[128], id[33], envp[1200], outp[1200];
    int state = 3;
    if (!parse_dsn(url, sizeof url, key, sizeof key)) {
        snprintf(g_msg, sizeof g_msg, "This build's Sentry DSN isn't valid.");
        goto done;
    }
    {
        uint64_t a = 0, b = 0;
        FILE *r = fopen("/dev/urandom", "rb");
        if (!r || fread(&a, 8, 1, r) != 1 || fread(&b, 8, 1, r) != 1)
            a = plat_now_ns() ^ ((uint64_t)getpid() << 32), b = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ull;
        if (r) fclose(r);
        snprintf(id, sizeof id, "%016llx%016llx", (unsigned long long)a, (unsigned long long)b);
    }
    Str ev = {0}, env = {0};
    event_json(&ev, id);
    str_fmt(&env, "{\"event_id\":\"%s\"}\n{\"type\":\"event\",\"length\":%zu}\n", id, ev.n);
    str_add(&env, ev.s, ev.n);
    str_cat(&env, "\n");
    envelope_item(&env, "report.txt", g_report ? g_report : "");
    if (g_log && g_log[0]) envelope_item(&env, "lasrecomp.log", g_log);
    free(ev.s);
    mkdir(g_dir, 0755);
    snprintf(envp, sizeof envp, "%s/envelope.tmp", g_dir);
    snprintf(outp, sizeof outp, "%s/send.log", g_dir);
    FILE *f = fopen(envp, "wb");
    bool written = f && env.s && fwrite(env.s, 1, env.n, f) == env.n;
    if (f) fclose(f);
    free(env.s);
    if (!written) {
        snprintf(g_msg, sizeof g_msg, "Couldn't write the report in %s.", g_dir);
        goto done;
    }
    char auth[256], data[1300];
    snprintf(auth, sizeof auth, "X-Sentry-Auth: Sentry sentry_version=7, sentry_client=awakeningframe/1, sentry_key=%s", key);
    snprintf(data, sizeof data, "@%s", envp);
    const char *av[] = {"/usr/bin/curl", "-sS", "--fail", "--proto", "=https", "--max-time", "30", "-H",
                        "Content-Type: application/x-sentry-envelope", "-H", auth, "--data-binary", data, url, NULL};
    PlatProc p = plat_spawn(av, outp, false);
    int rc = p ? plat_proc_wait(p, true) : -1;
    remove(envp);
    char why[160] = "";
    first_line(outp, why, sizeof why);
    remove(outp);
    if (rc == 0) {
        state = 2;
        LOG("[crash-report] sent (event %s)\n", id);
    } else {
        LOG("[crash-report] not sent: curl exit %d %s\n", rc, why);
        if (rc < 0) snprintf(g_msg, sizeof g_msg, "curl isn't installed.");
        else if (rc == 6 || rc == 7 || rc == 28 || rc == 35) snprintf(g_msg, sizeof g_msg, "No connection. Check your Wi-Fi.");
        else if (rc == 22) snprintf(g_msg, sizeof g_msg, "The server didn't take the report.");
        else snprintf(g_msg, sizeof g_msg, "Couldn't send it (curl error %d).", rc);
    }
done:
    atomic_store(&g_state, state);
    return NULL;
}

void crash_report_send(void) {
    if (!g_pending || atomic_load(&g_state) == 1 || atomic_load(&g_state) == 2) return;
    atomic_store(&g_state, 1);
    pthread_t t;
    if (pthread_create(&t, NULL, send_thread, NULL) == 0) pthread_detach(t);
    else snprintf(g_msg, sizeof g_msg, "Couldn't start sending."), atomic_store(&g_state, 3);
}

int crash_report_state(char *msg, size_t n) {
    int s = atomic_load(&g_state);
    if (n) snprintf(msg, n, "%s", s == 3 ? g_msg : "");
    return s;
}

// ---- AF_SENTRY_TEST_CRASH=1 (session.env): a real crash (SIGSEGV) 10 s after the game starts, to try all of the above
static void *test_crash(void *arg) {
    (void)arg;
    sleep(10);
    LOG("[crash-report] AF_SENTRY_TEST_CRASH: crashing on purpose now (a test)\n");
    *(volatile int *)(uintptr_t)8 = 1;
    return NULL;
}
void crash_report_test_arm(void) {
    static bool armed;
    const char *e = getenv("AF_SENTRY_TEST_CRASH");
    if (armed || !e || !*e || !strcmp(e, "0")) return;
    armed = true;
    pthread_t t;
    if (pthread_create(&t, NULL, test_crash, NULL) == 0) pthread_detach(t);
}
#endif
