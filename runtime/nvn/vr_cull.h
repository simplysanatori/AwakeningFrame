// VR culling fitted to the table (AwakeningFrame perf lane; vr_cull.c). Switches:
//   LARECOMP_VR_CULL=fit (default) | wide (the fixed 3.2:1.5) | stock (the game's own frustum)
//   LARECOMP_VR_CULL_RECT=<kx>,<kz>      the floor rectangle: the stock screen footprint scaled about the table centre (1.2,1.0)
//   LARECOMP_VR_CULL_HEIGHT=<lo>,<hi>    game units below / above the floor (1.5,3)
//   LARECOMP_VR_CULL_MARGIN=<m>          the eye frusta widened by this in NDC (0.3), for head motion
//   LARECOMP_VR_CULL_EYES=0              no eye clipping: the whole rectangle
//   LARECOMP_LOG_CULL=1                  the fit every 120 frames ([vrcull])
//   LARECOMP_VR_RING=<margin>            (lane ring, off) draws outside the fit's footprint grown by this are drawn in
//                                        the eyes' colour passes only: no shadow casting, no depth pre-pass (exec.c)
//   LARECOMP_VR_RING_CAST=<margin>       ring draws within this of the footprint still cast shadows (0: none)
// LARECOMP_FREECAM_CULL=<aspect>:<fovy scale> still overrides all of this.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cam_override.h"

enum { VR_CULL_FIT, VR_CULL_WIDE, VR_CULL_STOCK };
int vr_cull_mode(void);
// (widescreen.c) the game's own fovy (+0x564) and culling aspect (+0x570) fields, before any widening
void vr_cull_note_game(float fovy_field, float aspect_field);
// (cam_override.c, at each latch with eye cameras) fit the culling frustum to the table as both eyes see it
// pivot: the game camera's screen centre on the floor; anchor: where the table's centre is (the followed pivot)
void vr_cull_fit(const CamLatch *l, const float pivot[3], const float anchor[3], const CamRef *eye0, const CamRef *eye1);
// the fitted culling aspect (absolute) and fovy scale; false until the first fit
bool vr_cull_get(float *aspect, float *fovy_scale);
// (lane ring) the table fit's footprint as a world-axis box (y unbounded), before LARECOMP_VR_CULL_EXTENT; false: none
bool vr_cull_core_box(float lo[3], float hi[3]);
bool vr_ring_on(void);  // LARECOMP_VR_RING set
// a draw with this world box: 0 core, 1 ring, 2 ring that still casts (LARECOMP_VR_RING_CAST); against the footprint
// as it was at the first call of this frame (the executor's renderer_exec_frame)
int vr_ring_class(const float lo[3], const float hi[3], uint64_t frame);
