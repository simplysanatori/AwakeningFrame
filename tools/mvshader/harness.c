// (tools/mvshader, offline) The multiview shader variants (ShaderKey.mv, tex_amask) checked on the recorded variant
// corpus: the shipped list, the player's cache/shader_seen.txt and "[shader] record" lines of logs. The shaders' code
// comes from the game files through shader_prep.c's own index (shader_prep_run with this file's renderer_prepare), so
// nothing of the game is kept here. For each record, with the keys the renderer makes for it (vulkan/exec.c
// renderer_prepare, Steam Frame defaults: structured, push constants, shader global memory as copies):
//   - mv = 0: this tree's translator gives byte for byte the reference translator's GLSL (both stages);
//   - multiview (ShaderKey.mv: each stage's per-eye cbufs 3..10, tex_amask: array twins), in the combinations a camera
//     block and twins can give - every cbuf read per eye or only the lowest, no / every / one 2D sampled fragment texture
//     a twin (exec.c mv_program's rule): it translates, compiles with shaderc as the renderer does (Vulkan 1.3, -O0)
//     and the SPIR-V validates (SPIRV-Tools, Vulkan 1.3); every cbuf array index is a constant (eye 1's per-eye reads
//     "<row>u + MVR<k>"); no word of a per-eye block is promoted to push constants; the row strides are those of the
//     mv = 0 build;
//   - the record's text form with SR_MV / SR_MVP / v= / e= round-trips (shader_rec_format / _parse).
// Usage: harness <romfs> <ref.so> <new.so> <list or log>... (run.sh builds and runs it)
#include <dlfcn.h>
#include <lz4.h>
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <spirv-tools/libspirv.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maxwell/shader.h"
#include "shader_prep.h"

// ---- what shader_prep.c needs from the rest of the runtime
size_t rt_lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    int n = LZ4_decompress_safe((const char *)src, (char *)dst, (int)srclen, (int)dstlen);
    return n < 0 ? 0 : (size_t)n;
}
const char *larecomp_version(void) { return "mvshader-harness"; }
bool shader_seen_on(void) { return false; }
void shader_seen_note(const ShaderRec *r) { (void)r; }
bool shader_seen_shipped(const char *line) { (void)line; return false; }
char *shader_seen_read(uint64_t *file_game) { *file_game = 0; return NULL; }
void shader_seen_prep_done(uint64_t game, const char *keep) { (void)game; (void)keep; }
bool renderer_prepare_begin(void) { return true; }
bool renderer_prepare_multiview(void) { return false; }
int renderer_prepare_pending(void) { return 0; }
void renderer_prepare_end(void) {}

// ---- the two translators (wrap.c)
typedef struct {
    char *msl;
    char error[256];
    uint32_t out_mask, cbuf_mask, cbuf_dynamic, cbuf_size[18];
    int ntex, npush;
    uint8_t tex_type[64], tex_shadow[64], tex_image[64];
    uint16_t mv_rows[18];
    uint8_t push_cb[64];
} HxOut;
typedef struct {
    void (*config)(uint64_t, uint64_t, int, int, int, int, int);
    int (*translate)(const uint8_t *, size_t, int, const void *, const uint8_t *, uint32_t, uint32_t, uint32_t, HxOut *);
    int (*key_size)(void);
} Tr;
static Tr tr_ref, tr_new;
static void tr_load(Tr *t, const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); exit(2); }
    t->config = dlsym(h, "hx_config");
    t->translate = dlsym(h, "hx_translate");
    t->key_size = dlsym(h, "hx_key_size");
    if (!t->config || !t->translate || !t->key_size) { fprintf(stderr, "%s: not a harness translator\n", path); exit(2); }
    // (Steam Frame, Turnip: vulkan/exec.c vk_exec_init and the run logs)
    t->config(0x7240000000ull, 0x00D0000000ull, 1, 0, 1, 1, 1);
}

// ---- exec.c's key helpers (copies: attr_kind, rt_kind)
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}
static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

// ---- seen sets (open addressing; 0 = empty)
typedef struct { uint64_t *t; size_t cap, n; } Set;
static bool set_add(Set *s, uint64_t h) {
    if (!h) h = 1;
    if (s->n * 2 >= s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        for (size_t i = 0; i < s->cap; i++)
            if (s->t[i]) { size_t j = s->t[i] & (cap - 1); while (t[j]) j = (j + 1) & (cap - 1); t[j] = s->t[i]; }
        free(s->t);
        s->t = t, s->cap = cap;
    }
    size_t j = h & (s->cap - 1);
    while (s->t[j]) { if (s->t[j] == h) return false; j = (j + 1) & (s->cap - 1); }
    s->t[j] = h, s->n++;
    return true;
}

// ---- compile + validate jobs (worker threads; one shaderc compiler each)
typedef struct Job { char *src; int stage; char what[96]; struct Job *next; } Job;
static Job *jobs, **jobs_tail = &jobs;
static int njobs;
static Set job_seen;
static void job_add(const char *src, int stage, const char *what) {
    if (!set_add(&job_seen, fnv(0xcbf29ce484222325ull ^ (uint64_t)stage, src, strlen(src)))) return;
    Job *j = calloc(1, sizeof *j);
    j->src = strdup(src), j->stage = stage;
    snprintf(j->what, sizeof j->what, "%s", what);
    *jobs_tail = j, jobs_tail = &j->next;
    njobs++;
}

static _Atomic int st_compiled, st_compile_fail, st_valid_fail;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER, job_lock = PTHREAD_MUTEX_INITIALIZER;
static Job *job_next;
static const char *fail_dir;
static FILE *fail_log;

static void save_fail(const Job *j, const char *kind, const char *msg) {
    pthread_mutex_lock(&out_lock);
    static int n;
    fprintf(fail_log, "%s %s: %s\n", kind, j->what, msg);
    if (fail_dir && n < 200) {
        char p[512];
        snprintf(p, sizeof p, "%s/%03d_%s.%s", fail_dir, n, kind, j->stage == STAGE_VERTEX ? "vert" : "frag");
        FILE *f = fopen(p, "w");
        if (f) { fprintf(f, "// %s\n// %s\n%s", j->what, msg, j->src); fclose(f); }
    }
    n++;
    pthread_mutex_unlock(&out_lock);
}

static void *worker(void *arg) {
    (void)arg;
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    spv_context vctx = spvContextCreate(SPV_ENV_VULKAN_1_3);
    spv_validator_options vo = spvValidatorOptionsCreate();
    for (;;) {
        pthread_mutex_lock(&job_lock);
        Job *j = job_next;
        if (j) job_next = j->next;
        pthread_mutex_unlock(&job_lock);
        if (!j) break;
        shaderc_compile_options_t o = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        shaderc_compile_options_set_optimization_level(o, shaderc_optimization_level_zero);  // (the renderer's, Adreno)
        shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, j->src, strlen(j->src),
            j->stage == STAGE_VERTEX ? shaderc_vertex_shader : shaderc_fragment_shader, j->what, "main", o);
        shaderc_compile_options_release(o);
        atomic_fetch_add(&st_compiled, 1);
        if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
            atomic_fetch_add(&st_compile_fail, 1);
            save_fail(j, "compile", shaderc_result_get_error_message(r));
        } else {
            spv_const_binary_t bin = {(const uint32_t *)shaderc_result_get_bytes(r), shaderc_result_get_length(r) / 4};
            spv_diagnostic diag = NULL;
            if (spvValidateWithOptions(vctx, vo, &bin, &diag) != SPV_SUCCESS) {
                atomic_fetch_add(&st_valid_fail, 1);
                save_fail(j, "validate", diag && diag->error ? diag->error : "?");
            }
            spvDiagnosticDestroy(diag);
        }
        shaderc_result_release(r);
    }
    spvValidatorOptionsDestroy(vo);
    spvContextDestroy(vctx);
    shaderc_compiler_release(comp);
    return NULL;
}

// ---- the checks, per record
static struct {
    int recs, graphics, compute, programs, ref_fail, new_fail, diff, mv_progs, mv_fail, mv_amask_progs, mv_slots,
        rows_diff, push_left, no_ext, rec_rt, norm_cmp, norm_diff, const_bad, push_words;
} S;
static Set prog_seen;
static int ndiff_shown;

static void diff_note(const char *what, uint64_t vh, uint64_t fh, const char *a, const char *b) {
    S.diff++;
    if (ndiff_shown++ >= 8) return;
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    size_t s = i > 80 ? i - 80 : 0;
    fprintf(fail_log, "mv0-diff %s %016llx %016llx at byte %zu:\n  ref: %.200s\n  new: %.200s\n", what, (unsigned long long)vh,
            (unsigned long long)fh, i, a + s, b + s);
}


// (MVH_NOPUSH=1: no promoted push words in either build) a multiview GLSL with no array twins, its multiview parts
// taken out - the extension, the MVR / MVB defines, each per-eye read "(gl_ViewIndex == 0 ? <eye 0's> : <eye 1's>)"
// down to eye 0's, " + (gl_ViewIndex == 0 ? 0u : MVB<k>)" in each dynamic one, the block sizes - must be the mv = 0
// GLSL with its block sizes taken out too: nothing else changed
static char *norm_glsl(const char *src) {
    size_t n = strlen(src);
    char *o = malloc(n + 1), *w = o;
    static const char sel[] = "(gl_ViewIndex == 0 ? ";
    for (const char *p = src; *p;) {
        const char *eol = strchr(p, '\n');
        size_t l = eol ? (size_t)(eol - p + 1) : strlen(p);
        if (!strncmp(p, "#extension GL_EXT_multiview", 27) || !strncmp(p, "#define MVR", 11) || !strncmp(p, "#define MVB", 11)) {
            p += l;
            continue;
        }
        const char *e = p + l;
        while (p < e) {
            int k, len = 0;
            if (*p == ' ' && sscanf(p, " + (gl_ViewIndex == 0 ? 0u : MVB%d)%n", &k, &len) == 1 && len) { p += len; continue; }
            if (!strncmp(p, sel, sizeof sel - 1)) {  // (eye 0's read: up to the " : " at this depth; then past the ')')
                const char *a = p + sizeof sel - 1, *q = a;
                int d = 0;
                while (q < e && !(d == 0 && !strncmp(q, " : ", 3))) { if (*q == '(' || *q == '[') d++; else if (*q == ')' || *q == ']') d--; q++; }
                const char *r = q;
                d = 1;
                while (r < e && d) { if (*r == '(') d++; else if (*r == ')') d--; r++; }
                if (q < e && !d) { memcpy(w, a, (size_t)(q - a)), w += q - a; p = r; continue; }
            }
            if (!strncmp(p, "uniform CB", 10)) {  // (uniform CB<k> { uvec4 cb<k>[N]; }: N dropped)
                const char *b = strchr(p, '['), *c = b ? strchr(b, ']') : NULL;
                if (b && c && c < e) { memcpy(w, p, (size_t)(b - p + 1)), w += b - p + 1; p = c; continue; }
            }
            *w++ = *p++;
        }
    }
    *w = 0;
    return o;
}
static void norm_check(const char *what, const char *base, const char *mv) {
    char *a = norm_glsl(base), *b = norm_glsl(mv);
    S.norm_cmp++;
    if (strcmp(a, b)) {
        S.norm_diff++;
        size_t i = 0;
        while (a[i] && a[i] == b[i]) i++;
        size_t s0 = i > 60 ? i - 60 : 0;
        if (S.norm_diff <= 8) fprintf(fail_log, "mv-norm %s at %zu:\n  base: %.160s\n  mv:   %.160s\n", what, i, a + s0, b + s0);
    }
    free(a), free(b);
}

// Every constant buffer array access (cb<k>[...], outside the clamped dynamic cbr()) indexes with a constant: digits, or
// for a per-eye block eye 1's "<digits>u + MVR<k>". Per-eye reads come in the select form only, no other gl_ViewIndex
// in an index, and nothing of the old push block. Returns the non-constant accesses.
static int const_index_check(const char *what, const char *src, uint8_t mask) {
    int bad = 0;
    if (strstr(src, "mvm")) bad++, fprintf(fail_log, "mv-const %s: an mvm block\n", what);
    for (const char *p = src; (p = strstr(p, "cb")); p += 2) {
        if (p > src && ((p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_')) continue;
        int k, len = 0;
        if (sscanf(p, "cb%d[%n", &k, &len) != 1 || !len || k < 3) continue;
        if (p >= src + 8 && !strncmp(p - 7, "uvec4 ", 6)) continue;  // (the declaration)
        const char *ix = p + len, *q = strchr(ix, ']');
        if (!q) { bad++; continue; }
        char t[128];
        snprintf(t, sizeof t, "%.*s", (int)(q - ix < 127 ? q - ix : 127), ix);
        bool digits = t[0] != 0;
        for (const char *c = t; *c; c++) digits &= *c >= '0' && *c <= '9';
        int r, k2, l2 = 0;
        bool eye1 = sscanf(t, "%du + MVR%d%n", &r, &k2, &l2) == 2 && l2 == (int)strlen(t) && k2 == k && k >= 3 && k <= 10 && ((mask >> (k - 3)) & 1);
        if (!digits && !eye1) {
            if (bad++ < 4) fprintf(fail_log, "mv-const %s: cb%d[%s]\n", what, k, t);
        }
    }
    return bad;
}

static void mv_variant(const ShaderBlob *vsb, const ShaderBlob *fsb, const ShaderKey *vkey0, const ShaderKey *fkey0,
                       const HxOut *bv, const HxOut *bf, uint32_t amask, uint8_t vmask, uint8_t fmask, uint64_t vh, uint64_t fh) {
    if (!amask && !vmask && !fmask) return;
    ShaderKey vk = *vkey0, fk = *fkey0;
    vk.mv = vmask, fk.mv = fmask;
    for (int b = 0; b < 3; b++) vk.tex_amask[b] = fk.tex_amask[b] = (uint8_t)(amask >> (b * 8));
    HxOut v, f;
    uint32_t cap = 64;
    char what[112];
    snprintf(what, sizeof what, "%016llx_%016llx_a%x_e%x,%x", (unsigned long long)vh, (unsigned long long)fh, amask, vmask, fmask);
    S.mv_progs++;
    if (!tr_new.translate(vsb->code, vsb->const_offset ? vsb->const_offset : 0x10000, STAGE_VERTEX, &vk,
                          vsb->code + vsb->const_offset, vsb->const_size, 0, cap, &v)) {
        S.mv_fail++;
        fprintf(fail_log, "mv-translate vs %s: %s\n", what, v.error);
        return;
    }
    fk.vs_out_mask = v.out_mask;
    if (!tr_new.translate(fsb->code, fsb->const_offset ? fsb->const_offset : 0x10000, STAGE_FRAGMENT, &fk,
                          fsb->code + fsb->const_offset, fsb->const_size, (uint32_t)v.npush * 4u, cap, &f)) {
        S.mv_fail++;
        fprintf(fail_log, "mv-translate fs %s: %s\n", what, f.error);
        free(v.msl);
        return;
    }
    if (!amask && getenv("MVH_NOPUSH")) { norm_check(what, bv->msl, v.msl); norm_check(what, bf->msl, f.msl); }
    // promoted words: none from a per-eye block (the others as at mv = 0)
    for (int sg = 0; sg < 2; sg++) {
        const HxOut *o = sg ? &f : &v;
        uint8_t m = sg ? fmask : vmask;
        for (int i = 0; i < o->npush; i++)
            if (o->push_cb[i] >= 3 && o->push_cb[i] <= 10 && ((m >> (o->push_cb[i] - 3)) & 1)) {
                S.push_left++;
                fprintf(fail_log, "mv-push %s: a word of per-eye cbuf %d promoted (%s)\n", what, o->push_cb[i], sg ? "fs" : "vs");
                break;
            }
    }
    S.push_words += v.npush + f.npush;
    // the extension where the key makes a multiview shader (the stage's mask, or array twins: the key carries them in both)
    if ((vmask || amask) && !strstr(v.msl, "GL_EXT_multiview")) S.no_ext++;
    if ((fmask || amask) && !strstr(f.msl, "GL_EXT_multiview")) S.no_ext++;
    S.const_bad += const_index_check(what, v.msl, vmask) + const_index_check(what, f.msl, fmask);
    for (int k = 3; k < 18; k++) {
        if (((bv->cbuf_mask >> k) & 1) && bv->mv_rows[k] != v.mv_rows[k]) S.rows_diff++, fprintf(fail_log, "mv-rows vs %s cbuf %d: %u / %u\n", what, k, bv->mv_rows[k], v.mv_rows[k]);
        if (((bf->cbuf_mask >> k) & 1) && bf->mv_rows[k] != f.mv_rows[k]) S.rows_diff++, fprintf(fail_log, "mv-rows fs %s cbuf %d: %u / %u\n", what, k, bf->mv_rows[k], f.mv_rows[k]);
    }
    static int ndump;
    if (getenv("MVH_DUMP") && ndump < (getenv("MVH_DUMP_MAX") ? atoi(getenv("MVH_DUMP_MAX")) : 40)) {  // (MVH_DUMP=<dir>: the first
        // multiview variants, for reading; MVH_DUMP_MAX of them, 40)
        for (int sg = 0; sg < 2; sg++) {
            char p[600];
            snprintf(p, sizeof p, "%s/%s.%s", getenv("MVH_DUMP"), what, sg ? "frag" : "vert");
            FILE *df = fopen(p, "w");
            if (df) fputs(sg ? f.msl : v.msl, df), fclose(df), ndump++;
        }
    }
    char w2[128];
    snprintf(w2, sizeof w2, "%s_vs", what);
    job_add(v.msl, STAGE_VERTEX, w2);
    snprintf(w2, sizeof w2, "%s_fs", what);
    job_add(f.msl, STAGE_FRAGMENT, w2);
    free(v.msl);
    free(f.msl);
}

static void rec_roundtrip(const ShaderRec *r) {
    char a[2048], b[2048];
    ShaderRec q = *r, p;
    q.flags |= SR_MV | SR_MVP;
    q.mv_amask = 0xA5C3u & 0xFFFFFFu, q.mv_cb[0] = 0x05, q.mv_cb[1] = 0xC0;
    for (int pass = 0; pass < 2; pass++) {
        shader_rec_format(&q, a, sizeof a);
        if (!shader_rec_parse(a, &p)) { S.rec_rt++; fprintf(fail_log, "record parse: %s\n", a); return; }
        shader_rec_format(&p, b, sizeof b);
        if (strcmp(a, b) || p.flags != q.flags || p.mv_amask != q.mv_amask || p.mv_cb[0] != q.mv_cb[0] || p.mv_cb[1] != q.mv_cb[1] || p.vs != q.vs || p.fs != q.fs) {
            S.rec_rt++;
            fprintf(fail_log, "record round trip:\n  %s\n  %s\n", a, b);
            return;
        }
        q.flags &= (uint8_t)~(SR_MV | SR_MVP), q.mv_amask = 0, q.mv_cb[0] = q.mv_cb[1] = 0;  // (and the plain record)
    }
}

bool renderer_prepare(const ShaderRec *r, const ShaderBlob *vsb, const ShaderBlob *fsb) {
    S.recs++;
    if (r->kind == 'C') {  // (compute: mv = 0 only - multiview is graphics)
        S.compute++;
        uint64_t h = fnv(0xcbf29ce484222325ull, &r->vs, 8) | 1;
        if (!set_add(&prog_seen, h)) return true;
        S.programs++;
        ShaderKey k = {0};
        k.glsl = 1;
        HxOut a, b;
        size_t max = vsb->const_offset ? vsb->const_offset : 0x10000;
        int ra = tr_ref.translate(vsb->code, max, STAGE_COMPUTE, &k, NULL, 0, 0, 64, &a);
        int rb = tr_new.translate(vsb->code, max, STAGE_COMPUTE, &k, NULL, 0, 0, 64, &b);
        if (!ra) S.ref_fail++;
        if (!rb) S.new_fail++;
        if (ra != rb || (ra && strcmp(a.msl, b.msl)) || strcmp(a.error, b.error))
            diff_note("cs", r->vs, 0, a.msl ? a.msl : a.error, b.msl ? b.msl : b.error);
        free(a.msl), free(b.msl);
        return true;
    }
    S.graphics++;
    rec_roundtrip(r);
    ShaderKey vkey = {0}, fkey = {0};  // (exec.c renderer_prepare)
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = (r->flags & SR_FLIP_Y) != 0;
    vkey.depth_minus_one = (r->flags & SR_DEPTH_MINUS_ONE) != 0;
    vkey.flip_x = fkey.flip_x = (r->flags & SR_FLIP_X) != 0;
    for (int i = 0; i < r->nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(r->attrib[i] & 0xFF);
    for (int i = 0; i < r->ncolor && i < 8; i++) if (r->color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(r->color[i]);
    uint64_t h = fnv(fnv(fnv(0xcbf29ce484222325ull, &r->vs, 8), &r->fs, 8), &vkey, sizeof vkey);
    h = fnv(h, fkey.rt_kind, 8);
    if (!set_add(&prog_seen, h)) return true;
    S.programs++;
    // mv = 0, both translators (translate_program_now: the vertex shader first, its push words before the fragment's)
    size_t vmax = vsb->const_offset ? vsb->const_offset : 0x10000, fmax = fsb->const_offset ? fsb->const_offset : 0x10000;
    HxOut av, bv, af, bf;
    int rav = tr_ref.translate(vsb->code, vmax, STAGE_VERTEX, &vkey, vsb->code + vsb->const_offset, vsb->const_size, 0, 64, &av);
    int rbv = tr_new.translate(vsb->code, vmax, STAGE_VERTEX, &vkey, vsb->code + vsb->const_offset, vsb->const_size, 0, 64, &bv);
    if (rav != rbv || (rav && strcmp(av.msl, bv.msl)) || strcmp(av.error, bv.error))
        diff_note("vs", r->vs, r->fs, av.msl ? av.msl : av.error, bv.msl ? bv.msl : bv.error);
    if (!rav) S.ref_fail++;
    if (!rbv) { S.new_fail++; free(av.msl); return true; }
    ShaderKey fa = fkey, fb = fkey;
    fa.vs_out_mask = av.out_mask, fb.vs_out_mask = bv.out_mask;
    int raf = rav && tr_ref.translate(fsb->code, fmax, STAGE_FRAGMENT, &fa, fsb->code + fsb->const_offset, fsb->const_size, (uint32_t)av.npush * 4u, 64, &af);
    int rbf = tr_new.translate(fsb->code, fmax, STAGE_FRAGMENT, &fb, fsb->code + fsb->const_offset, fsb->const_size, (uint32_t)bv.npush * 4u, 64, &bf);
    if (rav && (raf != rbf || (raf && strcmp(af.msl, bf.msl)) || strcmp(af.error, bf.error)))
        diff_note("fs", r->vs, r->fs, raf ? af.msl : af.error, rbf ? bf.msl : bf.error);
    if (rav && !raf) S.ref_fail++;
    if (!rbf) { S.new_fail++; free(av.msl), free(bv.msl), free(raf ? af.msl : NULL); return true; }
    uint32_t amask = 0;
    for (int i = 0; i < bf.ntex && i < 24; i++)
        if (bf.tex_type[i] == TEXTYPE_2D && !bf.tex_image[i]) amask |= 1u << i;
    // mv: per-eye blocks - every cbuf 3..10 each stage reads, or only its lowest; array twins - none, every 2D sampled
    // fragment texture (exec.c mv_program's rule), or only the lowest; the combinations a camera block / twin can give
    uint8_t vall = (uint8_t)(bv.cbuf_mask >> 3), fall = (uint8_t)(bf.cbuf_mask >> 3);
    if (amask) S.mv_amask_progs++, S.mv_slots += __builtin_popcount(amask);
    mv_variant(vsb, fsb, &vkey, &fkey, &bv, &bf, 0, vall, fall, r->vs, r->fs);
    mv_variant(vsb, fsb, &vkey, &fkey, &bv, &bf, 0, vall & -vall, fall & -fall, r->vs, r->fs);
    mv_variant(vsb, fsb, &vkey, &fkey, &bv, &bf, 0, 0, fall, r->vs, r->fs);
    if (amask) {
        mv_variant(vsb, fsb, &vkey, &fkey, &bv, &bf, amask, 0, 0, r->vs, r->fs);
        mv_variant(vsb, fsb, &vkey, &fkey, &bv, &bf, amask, vall, fall, r->vs, r->fs);
        if (amask & (amask - 1)) mv_variant(vsb, fsb, &vkey, &fkey, &bv, &bf, amask & -amask, vall & -vall, 0, r->vs, r->fs);
    }
    if (getenv("MVH_BASE_COMPILE")) {  // (control: the mv = 0 shaders through the same compile and validation)
        job_add(bv.msl, STAGE_VERTEX, "base_vs");
        job_add(bf.msl, STAGE_FRAGMENT, "base_fs");
    }
    free(av.msl), free(bv.msl), free(raf ? af.msl : NULL), free(bf.msl);
    return true;
}

// ---- the corpus: record lines of lists, shader_seen.txt files and logs ("[shader] record G ..."), deduplicated
static char *corpus;
static size_t corpus_n, corpus_cap;
static Set line_seen;
static void corpus_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "can't read %s\n", path); return; }
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *p = strstr(line, "[shader] record ");
        p = p ? p + strlen("[shader] record ") : line;
        while (*p == ' ') p++;
        if ((p[0] != 'G' && p[0] != 'C') || p[1] != ' ') continue;
        ShaderRec r;
        if (!shader_rec_parse(p, &r) && !(p[0] == 'C' && strstr(p, "cs="))) continue;
        if (!set_add(&line_seen, fnv(0xcbf29ce484222325ull, p, strlen(p)))) continue;
        size_t l = strlen(p);
        if (corpus_n + l + 2 > corpus_cap) corpus_cap = (corpus_cap + l + 2) * 2, corpus = realloc(corpus, corpus_cap);
        memcpy(corpus + corpus_n, p, l), corpus_n += l, corpus[corpus_n++] = '\n';
        n++;
    }
    fclose(f);
    fprintf(stderr, "  %s: %d new records\n", path, n);
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s <romfs> <ref.so> <new.so> <list|log>... [-o faildir]\n", argv[0]); return 2; }
    fail_log = stdout;
    if (getenv("MVH_NOPUSH")) setenv("LARECOMP_VK_PUSH_CB", "0", 1);  // (both translators: read at their first use)
    tr_load(&tr_ref, argv[2]);
    tr_load(&tr_new, argv[3]);
    if (tr_ref.key_size() != tr_new.key_size() || tr_new.key_size() != (int)sizeof(ShaderKey)) {
        fprintf(stderr, "ShaderKey sizes differ: ref %d, new %d, harness %zu\n", tr_ref.key_size(), tr_new.key_size(), sizeof(ShaderKey));
        return 2;
    }
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { fail_dir = argv[++i]; continue; }
        corpus_file(argv[i]);
    }
    if (!corpus_n) { fprintf(stderr, "no records\n"); return 2; }
    char tmp[] = "/tmp/mvshader_list_XXXXXX";
    int fd = mkstemp(tmp);
    FILE *f = fdopen(fd, "w");
    fwrite(corpus, 1, corpus_n, f);
    fclose(f);
    setenv("LARECOMP_SHADER_LIST", tmp, 1);
    shader_prep_run(argv[1], NULL, NULL);
    remove(tmp);
    fflush(stdout);
    // compile + validate
    int nthreads = getenv("MVH_THREADS") ? atoi(getenv("MVH_THREADS")) : 3;
    if (nthreads < 1) nthreads = 1;
    fprintf(stderr, "compiling %d shaders on %d threads\n", njobs, nthreads);
    job_next = getenv("MVH_NOCOMPILE") ? NULL : jobs;
    pthread_t th[16];
    for (int i = 0; i < nthreads && i < 16; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < nthreads && i < 16; i++) pthread_join(th[i], NULL);
    printf("records %d (graphics %d, compute %d), programs %d\n", S.recs, S.graphics, S.compute, S.programs);
    printf("mv=0: ref failed %d, new failed %d, differ from ref %d\n", S.ref_fail, S.new_fail, S.diff);
    printf("mv: variants %d (with array twins: %d programs, %d twin slots), translate failed %d, per-eye words promoted %d "
           "(push words kept %d), no multiview extension %d, row strides differ %d, non-constant cbuf indices %d\n", S.mv_progs,
           S.mv_amask_progs, S.mv_slots, S.mv_fail, S.push_left, S.push_words, S.no_ext, S.rows_diff, S.const_bad);
    printf("compile+validate: %d unique shaders, compile failed %d, validation failed %d\n", st_compiled, st_compile_fail, st_valid_fail);
    printf("records with SR_MV|SR_MVP|v= round trip failed %d\n", S.rec_rt);
    if (S.norm_cmp) printf("multiview minus its multiview parts = mv=0: %d compared, %d differ\n", S.norm_cmp, S.norm_diff);
    bool ok = !S.diff && !S.mv_fail && !S.push_left && !S.no_ext && !S.rows_diff && !st_compile_fail && !st_valid_fail && !S.rec_rt && !S.norm_diff && !S.const_bad;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
