// Frame audit (VR and flat): what a game frame asks of the GPU, as the executor issues it. Debug only, off by default.
//
// LARECOMP_PASSLOG=<frame>[,<frame>...] (executor frames, renderer_exec_frame): "[plog]" lines in the log for
// LARECOMP_PASSLOG_FRAMES (default 2) frames from each: every render pass with its attachments, load ops, view mask and
// draw count, every clear, copy, feedback snapshot and dispatch, the textures each draw / dispatch samples, and one
// line per texture (size, format, layered, twinned). tools/passlog.py makes the per-pass table and the counts of it.
//
// LARECOMP_COPY_READERS=1: for every texture copy, what happens to the destination next - read (sampled, copied from,
// loaded by a pass, the eye post hook's depth), or overwritten whole first - over the whole run, and by whom; every
// 10 s a "[copyread]" summary per kind of copy (source and destination format and size). It finds copies nobody reads.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../nvn.h"

bool pl_on(void);       // this executor frame is logged (LARECOMP_PASSLOG)
bool cr_on(void);       // LARECOMP_COPY_READERS
static inline bool pl_any(void) { return pl_on() || cr_on(); }

// a render pass begins (merged: a draw joined the open pass instead); load: per colour attachment 1 = CLEAR, 0 = LOAD
void pl_pass(int eye, uint32_t view_mask, bool merged, int ncolor, NvnTexture *const *color, const int *load, NvnTexture *depth,
             uint32_t w, uint32_t h);
void pl_pass_end(void);
// a draw in the open pass: its fragment hash and the textures its stages sample
void pl_draw(uint64_t fs, int ntex, NvnTexture *const *tex);
// a clear in the open pass (index -1: depth / stencil); whole: it covers the pass's area; draw: done with a pipeline
void pl_clear(int index, bool depth, bool stencil, bool whole, bool draw);
// a copy (kind: copy, blit, resolve, alias, snapshot) of src into dst; whole: dst is overwritten whole; layer: the layer
// of a layered destination (a multiview run copies both, one call each: layer 1 is the same copy's second half)
void pl_copy(const char *kind, int eye, NvnTexture *src, NvnTexture *dst, int sw, int sh, int dw, int dh, bool whole, int layer);
// a dispatch and the textures (images: written, maybe read) it binds
void pl_dispatch(uint64_t cs, uint32_t gx, uint32_t gy, uint32_t gz, int ntex, NvnTexture *const *tex, const bool *image);
// another read of t outside the game's commands (the eye post hook's depth), or a write over it whole (a clear)
void pl_read(NvnTexture *t, const char *who);
void pl_overwrite(NvnTexture *t, const char *who);
void pl_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void cr_stats(void);    // (renderer_stats) the [copyread] summary, every 10 s
