// The menu laser's two quad layers (xr_pointer.c): the beam and the dot, over the HUD panel. Their images are made once
// at the start (xr_chains_create); xr_input.c sets the pose each refresh and xr_loop.c adds the layers after the HUD's.
#pragma once
#include "xr_internal.h"

typedef struct {
    bool shown;          // the menu has the controls and the pointing controller is tracked
    bool dot;            // the dot at the end (the ray meets the panel's plane in front)
    float origin[3], dir[3], length;  // the beam, in the app space (metres)
    float head[3];       // between the eyes (the layers face it)
    float swell;         // the dot's swell on reaching a new control, 1 .. 0
} XrPointerPose;

bool xr_pointer_create(void);   // false: no layers (LARECOMP_XR_LASER=0, or the images failed); clicks still work
void xr_pointer_destroy(void);
void xr_pointer_set(const XrPointerPose *p);       // (XR thread)
uint32_t xr_pointer_layers(XrCompositionLayerQuad out[2]);  // (XR thread) 0..2 layers to show this refresh
