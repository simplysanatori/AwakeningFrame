// (tools/mvshader) One translator build behind a small C interface, compiled once per translator under test (this
// tree's, and a reference tree's: run.sh) into its own shared object, so two translators with the same symbol names
// load side by side (dlopen RTLD_LOCAL, -Bsymbolic). Only fields both ShaderInfo layouts have are read by name.
#include <stdlib.h>
#include <string.h>

#include "shader.h"

typedef struct {
    char *msl;  // (malloc: the caller frees) NULL: the translation failed
    char error[256];
    uint32_t out_mask, cbuf_mask, cbuf_dynamic, cbuf_size[18];
    int ntex, npush;
    uint8_t tex_type[64], tex_shadow[64], tex_image[64];
    uint16_t mv_rows[18];  // (HAVE_MV) ShaderInfo.cbuf_mv_rows
    uint8_t push_cb[64];   // ShaderInfo.push_cb (npush of them)
} HxOut;

void hx_config(uint64_t gbase, uint64_t gsize, int frag_sg, int stack_vars, int structured, int push, int mirror) {
    maxwell_gmem_base = gbase, maxwell_gmem_size = gsize;
    maxwell_frag_subgroups = frag_sg;
    maxwell_glsl_stack_vars = stack_vars;
    maxwell_glsl_structured = structured;
    maxwell_glsl_push = push;
    maxwell_gmem_mirror = mirror;
    maxwell_gmem_chunk_shift = 0, maxwell_gmem_lazy = false, maxwell_gmem_fs_miss = false;
}

int hx_translate(const uint8_t *code, size_t max, int stage, const void *key, const uint8_t *cdata, uint32_t csize,
                 uint32_t push_base, uint32_t push_cap, HxOut *o) {
    ShaderKey k;
    memcpy(&k, key, sizeof k);
    maxwell_const_data = cdata, maxwell_const_size = csize;
    maxwell_push_base = push_base, maxwell_push_cap = push_cap;
    ShaderInfo *si = maxwell_translate(code, max, stage, &k);
    maxwell_const_data = NULL, maxwell_const_size = 0;
    memset(o, 0, sizeof *o);
    o->msl = si->msl ? strdup(si->msl) : NULL;
    memcpy(o->error, si->error, sizeof o->error);
    o->out_mask = si->out_mask, o->cbuf_mask = si->cbuf_mask, o->cbuf_dynamic = si->cbuf_dynamic;
    memcpy(o->cbuf_size, si->cbuf_size, sizeof o->cbuf_size);
    o->ntex = si->ntex, o->npush = si->npush;
    memcpy(o->push_cb, si->push_cb, sizeof o->push_cb);
    for (int i = 0; i < si->ntex && i < 64; i++)
        o->tex_type[i] = si->tex[i].type, o->tex_shadow[i] = si->tex[i].shadow, o->tex_image[i] = si->tex[i].is_image;
#ifdef HAVE_MV
    memcpy(o->mv_rows, si->cbuf_mv_rows, sizeof o->mv_rows);
#endif
    maxwell_free(si);
    return o->msl != NULL;
}

int hx_key_size(void) { return (int)sizeof(ShaderKey); }
