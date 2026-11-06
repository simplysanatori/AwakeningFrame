// VR culling fitted to the table (AwakeningFrame perf lane; vr_cull.c). Switches:
//   LARECOMP_VR_CULL=fit (default) | wide (the fixed 3.2:1.5) | stock (the game's own frustum)
//   LARECOMP_VR_CULL_RECT=<kx>,<kz>      the floor rectangle: the stock screen footprint scaled about the pivot (1.3,1.2)
//   LARECOMP_VR_CULL_HEIGHT=<lo>,<hi>    game units below / above the floor (1.5,3)
//   LARECOMP_VR_CULL_MARGIN=<m>          the eye frusta widened by this in NDC (0.3), for head motion
//   LARECOMP_VR_CULL_EYES=0              no eye clipping: the whole rectangle
//   LARECOMP_LOG_CULL=1                  the fit every 120 frames ([vrcull])
// LARECOMP_FREECAM_CULL=<aspect>:<fovy scale> still overrides all of this.
#pragma once
#include <stdbool.h>

#include "cam_override.h"

enum { VR_CULL_FIT, VR_CULL_WIDE, VR_CULL_STOCK };
int vr_cull_mode(void);
// (widescreen.c) the game's own fovy (+0x564) and culling aspect (+0x570) fields, before any widening
void vr_cull_note_game(float fovy_field, float aspect_field);
// (cam_override.c, at each latch with eye cameras) fit the culling frustum to the table as both eyes see it
void vr_cull_fit(const CamLatch *l, const float pivot[3], const CamRef *eye0, const CamRef *eye1);
// the fitted culling aspect (absolute) and fovy scale; false until the first fit
bool vr_cull_get(float *aspect, float *fovy_scale);
