// VR pass diet: post passes VR mode leaves out (docs/af/PASSES.md). Flat mode is unchanged.
//
// LARECOMP_VR_DIET (VR mode with the stereo executor only): unset / 1 = all parts, 0 = none, or a list of the parts:
//   dof    the tilt-shift depth of field. Blur removal (runtime/hle/qol.c, always on in VR) only takes its parameters
//          away: its downsample chain (fs 7b537cfc..., the scene at 1/2 and 1/4) and its full-size composite
//          (fs 86a3fa6a..., which then gives back its sharp input) still ran, ~0.7 ms per eye at 1728². Now:
//          - the chain's draws whose targets only feed the composite are left out (learned from the composite's other
//            inputs; the post targets alternate between two sets from frame to frame, so both sets are kept);
//          - the composite is left out, and the draws that wrote its sharp input S earlier in the frame (the AO apply)
//            write its target O instead, which is then what the composite would have given. A draw that reads S
//            while it is moved gets S back first (a copy O -> S); a frame where S's writes weren't moved gets a copy
//            S -> O in the composite's place.
//   merge  draws on the same attachments with nothing in between share one render pass (the HUD's draws, each
//          redirected into the HUD clone, were a pass each: 18 passes at Mabe).
//   smaa   no SMAA on the window image (menus shown on the HUD quad; the eyes never had it).
// LARECOMP_LOG_DIET=1: the diet's decisions for the first 400 draws it moves or leaves out.
#include "vr_diet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "stereo.h"

#define FS_DOF_DOWN 0x7b537cfc9b0c1937ull  // tilt-shift: 13-tap downsample (scene -> 1/2 -> 1/2 -> 1/4 -> 1/4)
#define FS_DOF_COMP 0x86a3fa6a2a33df54ull  // tilt-shift: composite (depth, 1/2 blur, sharp scene, 1/4 blur)
#define KEEP_FRAMES 4                      // learned textures not seen again for this many frames are forgotten

extern uint64_t renderer_exec_frame(void);

bool vr_diet(int what) {
    static int mask = -1;
    if (mask < 0) {
        const char *e = getenv("LARECOMP_VR_DIET");
        if (!e || !strcmp(e, "1")) mask = DIET_DOF | DIET_MERGE | DIET_SMAA;
        else mask = (strstr(e, "dof") ? DIET_DOF : 0) | (strstr(e, "merge") ? DIET_MERGE : 0) | (strstr(e, "smaa") ? DIET_SMAA : 0);
        if (!stereo_wanted()) mask = 0;
    }
    if (!(mask & what) || !stereo_active()) return false;  // (active once the session gave the eye size)
    static bool logged;
    if (!logged) {
        logged = true;
        LOG("[diet] VR pass diet:%s%s%s (LARECOMP_VR_DIET)\n", mask & DIET_DOF ? " dof" : "", mask & DIET_MERGE ? " merge" : "",
            mask & DIET_SMAA ? " smaa" : "");
    }
    return true;
}

// the chain: downsample targets that only feed a composite (eye 0 learns it; both eyes leave their draws out)
static struct { NvnTexture *t; uint64_t f; } chain[16];
static int nchain;
static struct { NvnTexture *rt, *src; } dw[16];  // this frame's downsample draws (eye 0)
static int ndw;
static uint64_t dw_frame = ~0ull;
// the composites: sharp input S, target O
static struct { NvnTexture *s, *o; uint64_t f; } pair[4];
static int npair;
// per eye, this frame: the pair whose S writes go to O (-1: none), whether S was copied back, the composite done
static struct { uint64_t f; int moved, restored, done; } eyest[2] = {{~0ull, -1, 0, 0}, {~0ull, -1, 0, 0}};
static int dbg = -1;

static int chain_find(const NvnTexture *t) {
    for (int i = 0; i < nchain; i++) if (chain[i].t == t) return i;
    return -1;
}
static void chain_note(NvnTexture *t, uint64_t f) {
    if (!t) return;
    int i = chain_find(t);
    if (i < 0 && nchain < 16) i = nchain++, chain[i].t = t;
    if (i >= 0) chain[i].f = f;
}
static int pair_of_s(const NvnTexture *s) {
    for (int i = 0; i < npair; i++) if (pair[i].s == s) return i;
    return -1;
}
static void forget_old(uint64_t f) {
    for (int i = 0; i < nchain;) if (f - chain[i].f > KEEP_FRAMES) chain[i] = chain[--nchain]; else i++;
    for (int i = 0; i < npair;) if (f - pair[i].f > KEEP_FRAMES) pair[i] = pair[--npair]; else i++;
}

bool vr_diet_wants(uint64_t h, const NvnTexture *rt) {
    if (h == FS_DOF_DOWN || h == FS_DOF_COMP) return vr_diet(DIET_DOF);
    if (!npair) return false;
    for (int e = 0; e < 2; e++) if (eyest[e].moved >= 0 && !eyest[e].restored && !eyest[e].done) return true;  // (reads of S)
    return rt && pair_of_s(rt) >= 0;
}

DietAct vr_diet_draw(uint64_t h, int eye, NvnTexture *rt, bool fixed, int ntex, NvnTexture *const *tex) {
    DietAct a = {DIET_DRAW, NULL, NULL, NULL};
    if (eye < 0 || eye > 1 || !rt) return a;
    uint64_t f = renderer_exec_frame();
    if (dbg < 0) dbg = getenv("LARECOMP_LOG_DIET") ? 400 : 0;
    if (f != dw_frame && eye == 0) dw_frame = f, ndw = 0, forget_old(f);
    if (eyest[eye].f != f) eyest[eye].f = f, eyest[eye].moved = -1, eyest[eye].restored = eyest[eye].done = 0;
    int m = eyest[eye].moved;
    if (h == FS_DOF_DOWN) {
        if (eye == 0 && ndw < 16 && ntex >= 1) dw[ndw].rt = rt, dw[ndw].src = tex[0], ndw++;
        if (chain_find(rt) >= 0) a.act = DIET_SKIP;
    } else if (h == FS_DOF_COMP) {
        // its sharp input must match its target in size and format, or it is drawn as it is
        NvnTexture *s = ntex >= 4 ? tex[2] : NULL;
        if (!s || s == rt || s->width != rt->width || s->height != rt->height || s->format != rt->format) {
            static int logged;
            if (!logged++) LOG("[diet] tilt-shift composite with unexpected inputs (%d textures): drawn\n", ntex);
            return a;
        }
        if (eye == 0) {
            chain_note(tex[1], f);
            chain_note(tex[3], f);
            for (int i = ndw - 1; i >= 0; i--)  // (in reverse: a draw's source is an earlier draw's target)
                if (chain_find(dw[i].rt) >= 0 && dw[i].src != s) chain_note(dw[i].src, f);
            int p = pair_of_s(s);
            if (p < 0 && npair < 4) p = npair++, pair[p].s = s;
            if (p >= 0) pair[p].o = rt, pair[p].f = f;
            static int logged;
            if (!logged++) LOG("[diet] tilt-shift composite left out; %d downsample targets learned\n", nchain);
        }
        // (S's writes weren't moved, or S was copied back and may have been written since: S is the one to show)
        if (!(m >= 0 && pair[m].s == s && pair[m].o == rt && !eyest[eye].restored)) a.copy_from = s, a.copy_to = rt;
        a.act = DIET_SKIP;
        eyest[eye].done = 1;
    } else if (m >= 0 && !eyest[eye].restored && !eyest[eye].done) {
        bool reads = false;
        for (int i = 0; i < ntex && i < 4; i++) reads |= tex[i] == pair[m].s;
        if (reads) a.copy_from = pair[m].o, a.copy_to = pair[m].s, eyest[eye].restored = 1;  // (S back before it is read)
        else if (rt == pair[m].s && !fixed) a.act = DIET_REDIRECT, a.into = pair[m].o;      // (a further write of S)
        else if (rt == pair[m].s) a.copy_from = pair[m].o, a.copy_to = pair[m].s, eyest[eye].restored = 1;  // (one that can't move)
    } else if (m < 0 && !eyest[eye].done && !fixed) {
        int p = pair_of_s(rt);
        if (p >= 0) a.act = DIET_REDIRECT, a.into = pair[p].o, eyest[eye].moved = p;  // (the first write of S: into O)
    }
    if (dbg > 0 && a.act == DIET_DRAW && !a.copy_from && pair_of_s(rt) >= 0) {
        dbg--;
        LOG("[diet] f %llu eye %d fs %016llx writes S %p, drawn there (moved %d done %d fixed %d)\n", (unsigned long long)f, eye,
            (unsigned long long)h, (void *)rt, m, eyest[eye].done, fixed);
    }
    if (dbg > 0 && (a.act != DIET_DRAW || a.copy_from)) {
        dbg--;
        LOG("[diet] f %llu eye %d fs %016llx rt %p: %s %p%s\n", (unsigned long long)f, eye, (unsigned long long)h, (void *)rt,
            a.act == DIET_SKIP ? "skip" : a.act == DIET_REDIRECT ? "into" : "draw", a.act == DIET_REDIRECT ? (void *)a.into : NULL,
            a.copy_from ? " (after a copy)" : "");
    }
    return a;
}
