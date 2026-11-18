// Frame audit and copy readers (passlog.h). Debug only: nothing here runs unless LARECOMP_PASSLOG or
// LARECOMP_COPY_READERS is set.
#include "passlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL_timer.h>

#include "../../rt.h"
#include "vk.h"

extern uint64_t renderer_exec_frame(void);

// ================================================================== LARECOMP_PASSLOG
static uint64_t pl_at[32];
static int pl_nat = -1, pl_nframes = 2;
static int pl_eye = -1;
static int pl_seq;            // passes so far in this frame
static uint64_t pl_seq_frame = ~0ull;
static bool pl_open;          // a logged pass is open
static int pl_draws, pl_clears;

bool pl_on(void) {
    if (pl_nat < 0) {
        pl_nat = 0;
        const char *e = getenv("LARECOMP_PASSLOG"), *n = getenv("LARECOMP_PASSLOG_FRAMES");
        for (const char *p = e; p && *p && pl_nat < 32;) {
            char *q;
            uint64_t f = strtoull(p, &q, 10);
            if (q == p) break;
            pl_at[pl_nat++] = f;
            p = *q == ',' ? q + 1 : q;
        }
        if (n && atoi(n) > 0) pl_nframes = atoi(n);
    }
    if (!pl_nat) return false;
    static uint64_t memo_f = ~0ull;
    static bool memo;
    uint64_t f = renderer_exec_frame();
    if (f == memo_f) return memo;
    memo_f = f, memo = false;
    for (int i = 0; i < pl_nat; i++) memo |= f >= pl_at[i] && f < pl_at[i] + (uint64_t)pl_nframes;
    if (memo) LOG("[plog] f=%llu frame\n", (unsigned long long)f);
    return memo;
}

// a texture's id in the log (its address's low bits, as the GPU profile's copy rows), and its line once per frame
static unsigned tid(const NvnTexture *t) { return (unsigned)((uintptr_t)t & 0xffffff); }
static void pl_tex_line(NvnTexture *t) {
    static struct { const NvnTexture *t; uint64_t f; uint32_t w, h; } seen[512];
    static int nseen;
    if (!t) return;
    uint64_t f = renderer_exec_frame();
    const TexBackend *b = t->backend;
    for (int i = 0; i < nseen; i++)
        if (seen[i].t == t) {
            if (seen[i].f == f && (!b || (seen[i].w == b->width && seen[i].h == b->height))) return;
            seen[i].f = f, seen[i].w = b ? b->width : 0, seen[i].h = b ? b->height : 0;
            goto print;
        }
    if (nseen == 512) nseen = 0;
    seen[nseen].t = t, seen[nseen].f = f, seen[nseen].w = b ? b->width : 0, seen[nseen].h = b ? b->height : 0, nseen++;
print:
    LOG("[plog] I %06x fmt=%#x g=%ux%u img=%ux%u vk=%d lv=%u ly=%d tw=%d sc=%d ao=%d sh=%d pres=%d \"%s\"\n", tid(t), t->format, t->width,
        t->height, b ? b->width : 0, b ? b->height : 0, b ? (int)b->format : 0, b ? b->levels : 0, vk_b_layered(b) ? 2 : 1,
        vk_tex_twinned(t), b && b->scalable, b && b->ao, b && b->shadow, b && b->presented, t->label);
}
#define PLF "[plog] f=%llu e=%d "
#define PLA (unsigned long long)renderer_exec_frame(), pl_eye

void pl_note(const char *fmt, ...) {
    if (!pl_on()) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG(PLF "N %s\n", PLA, buf);
}

// ================================================================== LARECOMP_COPY_READERS
// Each copy's destination is tracked until its next use. A pass that loads it counts as a read at its first draw (a
// whole clear before any draw overwrites it instead; an empty pass is neither). Kinds of copy are told apart by the
// formats and sizes; readers by what they are and the shader hash.
typedef struct { char who[40]; uint64_t n; } Reader;
typedef struct {
    char kind[12];
    uint32_t sfmt, sw, sh, dfmt, dw, dh;
    int layered;
    uint64_t copies, same, next, later, over, again, pending;
    Reader rd[6];
} CrSig;
static CrSig sigs[48];
static int nsigs;
typedef struct { const NvnTexture *t; int sig; uint64_t frame; } CrDst;
static CrDst dsts[96];
static int ndsts;
static const NvnTexture *cr_load[9];  // the open pass's loaded attachments, read at its first draw
static int ncr_load;

bool cr_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_COPY_READERS"); on = e && atoi(e) != 0; }
    return on;
}

static int cr_find(const NvnTexture *t) {
    for (int i = 0; i < ndsts; i++) if (dsts[i].t == t) return i;
    return -1;
}
static void cr_drop(int i) { dsts[i] = dsts[--ndsts]; }
static void cr_read_who(const NvnTexture *t, const char *who) {
    int i = t ? cr_find(t) : -1;
    if (i < 0) return;
    CrSig *s = &sigs[dsts[i].sig];
    uint64_t d = renderer_exec_frame() - dsts[i].frame;
    if (d == 0) s->same++; else if (d == 1) s->next++; else s->later++;
    int k = 0;
    while (k < 6 && s->rd[k].n && strcmp(s->rd[k].who, who)) k++;
    if (k == 6) {  // (full: the rarest one goes)
        k = 0;
        for (int j = 1; j < 6; j++) if (s->rd[j].n < s->rd[k].n) k = j;
        s->rd[k].n = 0;
    }
    if (!s->rd[k].n) snprintf(s->rd[k].who, sizeof s->rd[k].who, "%s", who);
    s->rd[k].n++;
    cr_drop(i);
}
static void cr_over(const NvnTexture *t) {
    int i = t ? cr_find(t) : -1;
    if (i < 0) return;
    sigs[dsts[i].sig].over++;
    cr_drop(i);
}

void cr_stats(void) {
    if (!cr_on()) return;
    static uint64_t last;
    uint64_t now = SDL_GetTicks();
    if (now - last < 10000) return;
    last = now;
    for (int i = 0; i < ndsts; i++) sigs[dsts[i].sig].pending++;
    for (int i = 0; i < nsigs; i++) {
        CrSig *s = &sigs[i];
        if (!s->copies) continue;
        char rd[300] = "";
        for (int k = 0; k < 6; k++)
            if (s->rd[k].n) snprintf(rd + strlen(rd), sizeof rd - strlen(rd), "%s%s %llu", rd[0] ? ", " : "", s->rd[k].who, (unsigned long long)s->rd[k].n);
        LOG("[copyread] %s %#x %ux%u -> %#x %ux%u%s: %llu copies; next use: read in the frame %llu, the next frame %llu, later %llu; "
            "overwritten unread %llu (by another copy %llu); not yet %llu; readers: %s\n",
            s->kind, s->sfmt, s->sw, s->sh, s->dfmt, s->dw, s->dh, s->layered ? " (layered)" : "", (unsigned long long)s->copies,
            (unsigned long long)s->same, (unsigned long long)s->next, (unsigned long long)s->later, (unsigned long long)s->over,
            (unsigned long long)s->again, (unsigned long long)s->pending, rd[0] ? rd : "none");
        memset(&s->copies, 0, sizeof *s - offsetof(CrSig, copies));
    }
}

// ================================================================== hooks
void pl_pass(int eye, uint32_t view_mask, bool merged, int ncolor, NvnTexture *const *color, const int *load, NvnTexture *depth,
             uint32_t w, uint32_t h) {
    pl_eye = eye;
    if (cr_on() && !merged) {
        ncr_load = 0;
        for (int i = 0; i < ncolor; i++)
            if (color[i]) { if (load[i]) cr_over(color[i]); else cr_load[ncr_load++] = color[i]; }
        if (depth) cr_load[ncr_load++] = depth;
    }
    if (!pl_on()) return;
    uint64_t f = renderer_exec_frame();
    if (f != pl_seq_frame) pl_seq_frame = f, pl_seq = 0;
    if (merged) { LOG(PLF "J %d\n", PLA, pl_seq - 1); return; }
    for (int i = 0; i < ncolor; i++) pl_tex_line(color[i]);
    pl_tex_line(depth);
    char att[256] = "";
    for (int i = 0; i < ncolor; i++)
        if (color[i]) snprintf(att + strlen(att), sizeof att - strlen(att), " c%d=%06x:%s", i, tid(color[i]), load[i] ? "C" : "L");
    if (depth) snprintf(att + strlen(att), sizeof att - strlen(att), " d=%06x:L", tid(depth));
    LOG(PLF "B %d %ux%u vm=%u%s\n", PLA, pl_seq++, w, h, view_mask, att);
    pl_open = true, pl_draws = pl_clears = 0;
}

void pl_pass_end(int held) {
    ncr_load = 0;
    if (!pl_open) return;
    pl_open = false;
    if (pl_on()) LOG(PLF "E %d draws=%d clears=%d rec=%d\n", PLA, pl_seq - 1, pl_draws, pl_clears, held != 2 && held != 3);
}

void pl_draw(uint64_t fs, int ntex, NvnTexture *const *tex) {
    if (cr_on()) {
        char who[40];
        snprintf(who, sizeof who, "draw %016llx", (unsigned long long)fs);
        for (int i = 0; i < ncr_load; i++) cr_read_who(cr_load[i], "pass load");
        ncr_load = 0;
        for (int i = 0; i < ntex; i++) cr_read_who(tex[i], who);
    }
    if (!pl_on()) return;
    pl_draws++;
    char t[400] = "";
    for (int i = 0; i < ntex; i++)
        if (tex[i]) pl_tex_line(tex[i]), snprintf(t + strlen(t), sizeof t - strlen(t), "%s%06x", t[0] ? "," : "", tid(tex[i]));
    LOG(PLF "D fs=%016llx t=%s\n", PLA, (unsigned long long)fs, t[0] ? t : "-");
}

void pl_clear(int index, bool depth, bool stencil, bool whole, bool draw, bool fold) {
    if (!pl_on()) return;
    pl_clears++;
    LOG(PLF "K %s%s%s whole=%d draw=%d after=%d fold=%d\n", PLA, index >= 0 ? "c" : "", index >= 0 ? (char[2]){(char)('0' + index), 0} : "",
        index < 0 ? (depth && stencil ? "ds" : depth ? "d" : "s") : "", whole, draw, pl_draws, fold);
}

void pl_copy(const char *kind, int eye, NvnTexture *src, NvnTexture *dst, int sw, int sh, int dw, int dh, bool whole, int layer) {
    pl_eye = eye;
    if (cr_on() && dst && !layer) {
        cr_read_who(src, "copy source");
        int i = cr_find(dst);
        if (i >= 0) sigs[dsts[i].sig].over++, sigs[dsts[i].sig].again++, cr_drop(i);
        if (!whole) goto log;  // (a part: what it is copied over may still be read)
        const TexBackend *sb = src ? src->backend : NULL, *db = dst->backend;
        uint32_t sf = src ? src->format : 0;
        int layered = vk_b_layered(db);
        int k = 0;
        for (; k < nsigs; k++) {
            CrSig *s = &sigs[k];
            if (!strcmp(s->kind, kind) && s->sfmt == sf && s->sw == (sb ? sb->width : 0) && s->sh == (sb ? sb->height : 0) &&
                s->dfmt == dst->format && s->dw == (db ? db->width : 0) && s->dh == (db ? db->height : 0) && s->layered == layered)
                break;
        }
        if (k == nsigs && nsigs < 48) {
            CrSig *s = &sigs[nsigs++];
            memset(s, 0, sizeof *s);
            snprintf(s->kind, sizeof s->kind, "%s", kind);
            s->sfmt = sf, s->sw = sb ? sb->width : 0, s->sh = sb ? sb->height : 0;
            s->dfmt = dst->format, s->dw = db ? db->width : 0, s->dh = db ? db->height : 0, s->layered = layered;
        }
        if (k < nsigs) {
            sigs[k].copies++;
            if (ndsts == 96) cr_drop(0);
            dsts[ndsts++] = (CrDst){dst, k, renderer_exec_frame()};
        }
    }
log:
    if (!pl_on()) return;
    pl_tex_line(src);
    pl_tex_line(dst);
    LOG(PLF "X %s %06x>%06x %dx%d>%dx%d whole=%d ly=%d\n", PLA, kind, tid(src), tid(dst), sw, sh, dw, dh, whole, layer);
}

void pl_dispatch(uint64_t cs, uint32_t gx, uint32_t gy, uint32_t gz, int ntex, NvnTexture *const *tex, const bool *image) {
    if (cr_on()) {
        char who[40];
        snprintf(who, sizeof who, "dispatch %llx", (unsigned long long)cs);
        for (int i = 0; i < ntex; i++) cr_read_who(tex[i], who);
    }
    if (!pl_on()) return;
    char t[400] = "";
    for (int i = 0; i < ntex; i++)
        if (tex[i]) pl_tex_line(tex[i]), snprintf(t + strlen(t), sizeof t - strlen(t), "%s%s%06x", t[0] ? "," : "", image[i] ? "I:" : "", tid(tex[i]));
    LOG(PLF "C cs=%llx %ux%ux%u t=%s\n", PLA, (unsigned long long)cs, gx, gy, gz, t[0] ? t : "-");
}

void pl_read(NvnTexture *t, const char *who) {
    if (!t) return;
    if (cr_on()) cr_read_who(t, who);
    if (pl_on()) pl_tex_line(t), LOG(PLF "R %06x %s\n", PLA, tid(t), who);
}

void pl_overwrite(NvnTexture *t, const char *who) {
    if (!t) return;
    if (cr_on()) {
        for (int i = 0; i < ncr_load; i++)
            if (cr_load[i] == t) cr_load[i] = cr_load[--ncr_load];  // (cleared in its pass before a draw: not loaded)
        cr_over(t);
    }
    if (pl_on()) pl_tex_line(t), LOG(PLF "W %06x %s\n", PLA, tid(t), who);
}
