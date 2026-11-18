// VR pass diet (runtime/nvn/vulkan/vr_diet.c): post passes VR mode leaves out. Flat mode is unchanged.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../nvn.h"

enum { DIET_DOF = 1, DIET_MERGE = 2, DIET_SMAA = 4 };
// the part `what` (DIET_*) is on: VR mode with the stereo executor and LARECOMP_VR_DIET (default: all)
bool vr_diet(int what);
// (draw path, cheap) the draw needs vr_diet_draw: its fragment shader is one the diet looks at, or it writes / may read
// a texture the diet moved (rt: its colour target)
bool vr_diet_wants(uint64_t fs_hash, const NvnTexture *rt);
// a draw's verdict. Before anything else: copy_from -> copy_to (whole texture) when copy_from is set. Then act:
// DIET_DRAW as it is, DIET_SKIP leave it out, DIET_REDIRECT draw it into `into` instead of its colour target.
enum { DIET_DRAW, DIET_SKIP, DIET_REDIRECT };
typedef struct { int act; NvnTexture *into, *copy_from, *copy_to; } DietAct;
// fixed: the draw can't be moved to another target (blending on, a view, already redirected); tex: the first
// fragment textures (ntex of them, at most 4 read)
DietAct vr_diet_draw(uint64_t fs_hash, int eye, NvnTexture *rt, bool fixed, int ntex, NvnTexture *const *tex);

// Savings outside the diet list, each its own switch (docs/af/PASSES.md "Clears and copies"), VR mode with the stereo
// executor only; flat mode is unchanged. VR_CLEAR_FOLD: LARECOMP_VR_CLEAR_FOLD (a pass's begin waits for its first draw:
// whole clears before it become load ops, passes with nothing in them are not recorded).
// VR_COPY_ALIAS: LARECOMP_VR_COPY_ALIAS (the flat mode's depth copy aliases, exec.c copy_opt, in multiview runs).
// VR_PRESENT_COPY: LARECOMP_VR_PRESENT_COPY (the window image's copy into the presented texture is left out on frames
// that present the HUD clone).
// VR_CLEAR_DEFER: LARECOMP_VR_CLEAR_DEFER (a pass with only clears in it waits: its clears become the next pass's load ops
// on the same attachments; needs, and turns on, VR_CLEAR_FOLD; LARECOMP_VR_CLEAR_FOLD=0 turns both off).
// On by default: VR_CLEAR_DEFER (with the fold) and VR_COPY_ALIAS.
enum { VR_CLEAR_FOLD = 1, VR_COPY_ALIAS = 2, VR_PRESENT_COPY = 4, VR_CLEAR_DEFER = 8 };
bool vr_saving(int what);
