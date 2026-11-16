// A fatal error, explained to the player: the crash dialog (plat_fatal_notice) says what went wrong in plain words
// and what to do about it; the technical lines (FATAL / signal, backtrace, registers) stay in the log for bug reports.
// Known causes are recognised from what the runtime knows when it stops: the fatal line, where the crash was (the
// game code, LAS Recomp itself, the graphics driver), and the game files.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "game_addresses.h"
#include "platform.h"
#include "rt.h"

static char g_line[512];   // the first fatal line (FATAL message / "signal 11 accessing 0x10")
static uint64_t g_pc;      // a crash's host pc (0: a FATAL)
int rt_romfs_files = -1;   // the game's own RomFS files (runtime/services/fs.c; -1: not read yet)
uint64_t rt_romfs_bytes;   // their sizes summed (issue #391: one damaged file, every file there)
int rt_romfs_missing;      // the game's files not found by name and size (#391: a renamed file keeps count and bytes)
enum { ROMFS_FILES = 3836 };  // (every supported version: runtime/ui/launcher.mm counts the same)

void rt_fatal_msg(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "FATAL: %s", b);
    if (!g_line[0]) snprintf(g_line, sizeof g_line, "%s", b);
}

void rt_crash_fault(const char *what, uint64_t fa, uint64_t pc) {
    if (!g_line[0]) snprintf(g_line, sizeof g_line, "%s accessing %#llx", what, (unsigned long long)fa);
    if (!g_pc) g_pc = pc;
}

static bool has(const char *s) { return strstr(g_line, s) != NULL; }

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c: inside the game code?

static bool contains_nocase(const char *hay, const char *needle) {  // (no strcasestr on Windows)
    for (; *hay; hay++) {
        size_t i = 0;
        while (needle[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (!needle[i]) return true;
    }
    return false;
}

// The graphics driver's own code (the crash was inside it, not in LAS Recomp): Vulkan drivers on Windows / Linux,
// Apple's Metal driver
static bool in_gpu_driver(uint64_t pc) {
    char m[512];
    if (!pc || !plat_module_name(pc, m, sizeof m)) return false;
    static const char *const drivers[] = {"nvoglv", "nvgpucomp", "nvwgf", "libnvidia", "amdvlk", "amdxc", "atiumd",
                                          "atig6", "igvk", "igc64", "igd", "libvulkan_radeon", "libvulkan_intel",
                                          "libvulkan_nouveau", "libvulkan_lvp", "libvulkan_virtio", "AGXMetal",
                                          "MTLCompiler", "libLLVM", "radeonsi"};
    for (size_t i = 0; i < sizeof drivers / sizeof *drivers; i++)
        if (contains_nocase(m, drivers[i])) return true;
    return false;
}

enum { HEAD_N = 160, WHAT_N = 400 };
static void explain(char *head, char *what) {
    static const char *const report = "Please report it as a GitHub issue and attach the log file below. "
                                      "Your progress up to your last save or autosave is safe.";
    if (rt_romfs_files >= 0 && rt_romfs_files < ROMFS_FILES) {
        snprintf(head, HEAD_N, "Your game files are incomplete");
        snprintf(what, WHAT_N, "LAS Recomp found %d of the game's %d files, and the game stopped when it needed "
                 "one that's missing. Copy your game files again so none are missing, then set them up again on the "
                 "start screen (Game Files).", rt_romfs_files, (int)ROMFS_FILES);
    } else if (has("doesn't fit its language")) {
        snprintf(head, HEAD_N, "Your game files' text has been changed");
        snprintf(what, WHAT_N, "The game's text for this language comes from another language (a translation "
                 "patch), and the game stopped at a question it can't show that way. Set up unmodified game files on "
                 "the start screen (Game Files). To play in another language, pick it in Settings (Language).");
    } else if (has("built for another version")) {
        snprintf(head, HEAD_N, "The game code was built for other game files or another version of LAS Recomp");
        snprintf(what, WHAT_N, "Start LAS Recomp again: the start screen rebuilds the game code for these game "
                 "files by itself.");
    } else if (has("unsupported version of the game")) {
        snprintf(head, HEAD_N, "Wrong version of the game");
        snprintf(what, WHAT_N, "LAS Recomp needs version 1.0.0, 1.0.1 or 1.1.0 of the game. Set up those game "
                 "files on the start screen (Game Files).");
    } else if (has("no font for the game's text")) {
        snprintf(head, HEAD_N, "No font for the game's text");
        snprintf(what, WHAT_N, "Import the Switch fonts in Settings (Game font), or reinstall LAS Recomp - it "
                 "comes with a font of its own.");
    } else if (rt_romfs_files >= ROMFS_FILES &&
               (rt_romfs_missing > 0 ||
                (game_romfs_bytes(game_version()) && rt_romfs_bytes != game_romfs_bytes(game_version())))) {
        snprintf(head, HEAD_N, "Your game files are damaged or changed");
        snprintf(what, WHAT_N, "All of the game's files are there, but some don't match the game, and the game "
                 "stopped when it needed one of them. Copy your game files again, then set them up again on the start "
                 "screen (Game Files).");
    } else if (has("get_terminate") || has("uncaught C++ exception")) {  // (the game threw: recompiled code can't
                                                                         // unwind; runtime/hle/warp.c rt_cxa_throw)
        snprintf(head, HEAD_N, "The game ran into an error it couldn't recover from");
        snprintf(what, WHAT_N, "This is a bug in LAS Recomp, not a problem with your game files or PC. %s", report);
    } else if (in_gpu_driver(g_pc)) {
        snprintf(head, HEAD_N, "Your graphics driver crashed");
        snprintf(what, WHAT_N, "Update your graphics driver (NVIDIA, AMD or Intel) and try again. If it keeps "
                 "happening, please report it with the log file below.");
    } else if (has("VK_") || has("Vulkan")) {
        snprintf(head, HEAD_N, "Graphics driver error");
        snprintf(what, WHAT_N, "LAS Recomp needs a graphics driver with Vulkan 1.3: install the latest driver for "
                 "your GPU (NVIDIA, AMD or Intel). If it keeps happening, please report it with the log file below.");
    } else if (has("out of memory") || has("cannot allocate") || has("alloc failed")) {
        snprintf(head, HEAD_N, "Out of memory");
        snprintf(what, WHAT_N, "Close other programs and try again.");
    } else if (g_pc && rt_host_fn_name(g_pc, NULL)) {
        snprintf(head, HEAD_N, "The game crashed");
        snprintf(what, WHAT_N, "This is a bug in LAS Recomp, not a problem with your game files or PC. %s", report);
    } else {
        snprintf(head, HEAD_N, "LAS Recomp stopped because of an error");
        snprintf(what, WHAT_N, "%s", report);
    }
}

// The fatal line and the crash's host pc (0: a FATAL), and the headline alone: the crash record (crash_report.c)
const char *rt_crash_line(uint64_t *pc) {
    if (pc) *pc = g_pc;
    return g_line;
}
void rt_crash_headline(char *out, size_t n) {
    char head[HEAD_N], what[WHAT_N];
    explain(head, what);
    snprintf(out, n, "%s", head);
}

void rt_crash_explain(char *out, size_t n, const char *log_path) {
    char head[HEAD_N], what[WHAT_N];
    explain(head, what);
    fprintf(stderr, "[crash] %s\n", head);
    if (log_path && *log_path) snprintf(out, n, "%s\n\n%s\n\nLog file:\n%s", head, what, log_path);
    else snprintf(out, n, "%s\n\n%s", head, what);
}
