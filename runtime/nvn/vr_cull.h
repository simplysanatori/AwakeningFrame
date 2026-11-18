// VR culling fitted to the table (vr_cull.c). Switches:
//   LARECOMP_VR_CULL=fit (default: the table's own culling camera) | camera (the game camera's frustum fitted to the
//                    stock screen's footprint, as before) | wide (the fixed 3.2:1.5) | stock (the game's own frustum)
//   LARECOMP_VR_CULL_TABLE=<kx>,<kz>     the table: a rectangle about its centre, the stock screen's widest half-width and far
//                                        reach on the floor times these (1.5,2); LARECOMP_VR_CULL_TABLE_UNITS=<x>,<z>: in units
//   LARECOMP_VR_CULL_FADE=0              (Table Edge Fade made round, LARECOMP_VR_EDGE_ROUND=1) the table's culling camera
//                                        keeps the whole rectangle; by default it is cut to the square about the centre the
//                                        fade can light (vr_cull_note_fade). Without the round fade there is no cut
//   LARECOMP_VR_CULL_RECT=<kx>,<kz>      (camera) the floor rectangle: the stock screen footprint scaled about the table centre (1.2,1.0)
//   LARECOMP_VR_CULL_HEIGHT=<lo>,<hi>    game units below / above the floor (1.5,3)
//   LARECOMP_VR_CULL_MARGIN=<m>          the eye frusta widened by this in NDC (0.3), for head motion
//   LARECOMP_VR_CULL_EYES=0              no eye clipping: the whole rectangle
//   LARECOMP_LOG_CULL=1                  the fit every 120 frames ([vrcull])
//   LARECOMP_VR_RING=<margin>            (off) draws outside the fit's footprint grown by this are drawn in
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
// (widescreen.c rt_cull_frustum) a camera matrix a culling frustum is built from (right, up, back, position: four floats
// each) and its fovy (radians), aspect, near, far: replaced by the table's culling camera when the fit made one (true).
// main: the engine's main camera (taken only when it is the visibility pass's camera), else the visibility pass's
bool vr_cull_camera(float *m, float *p, bool main);
// (ring draws) the table fit's footprint as a world-axis box (y unbounded), before LARECOMP_VR_CULL_EXTENT; false: none
bool vr_cull_core_box(float lo[3], float hi[3]);
bool vr_ring_on(void);  // LARECOMP_VR_RING set
// (scene, docs/af/SCENE.md) the world past the table: LARECOMP_VR_SCENE=off | low | backdrop | full | <rooms>, the VR tab's
// row (live). Low / Full widen the culling to (LARECOMP_VR_SCENE_ROOMS, 2) + 0.5 rooms about the table centre with ring draws.
enum { VR_SCENE_OFF, VR_SCENE_LOW, VR_SCENE_BACKDROP, VR_SCENE_FULL, VR_SCENE_N };
void vr_scene_set_mode(int mode);
int vr_scene_mode(void);
void vr_scene_backdrop_wide(bool on);  // (scene.c) Backdrop's capture frames: culled and drawn as Full
// the drawn box's smaller half-extent (units; the scene's or LARECOMP_VR_CULL_EXTENT's), 0 = off
float vr_cull_scene_reach(void);
// a draw with this world box: 0 core, 1 ring, 2 ring that still casts (LARECOMP_VR_RING_CAST), 3 past the EXTENT box
// (lane scene: drawn nowhere but shared colour passes); against the footprint
// as it was at the first call of this frame (the executor's renderer_exec_frame)
int vr_ring_class(const float lo[3], const float hi[3], uint64_t frame);
// (edge pass, render thread) the latest fit's floor quadrilateral in world (x, z), counter-clockwise from above, the floor
// height (the anchor's y) and the prism's extent below / above it in game units; false: none (stock / wide culling, a side view)
bool vr_cull_rect(float quad[4][2], float *floor_y, float *hlo, float *hhi);
// (edge pass, render thread) where the latest fit's culling frustum meets the floor (the anchor's height): the game draws
// what lies inside it (and whole objects that reach into it), nothing past it. World (x, z), counter-clockwise from above;
// a corner whose ray misses the floor lies at the far plane. False with no floor rectangle or a frustum reaching behind.
bool vr_cull_drawn(float quad[4][2]);
// (edge.c, each eye-0 pass of Table Edge Fade in the overworld; 0 otherwise, and on a switch away from Fade) the
// farthest from the table's centre the fade can show anything, from its settings (not from its fit, which is made inside
// vr_cull_drawn): the table's culling camera takes no more than the square of that half-size (+ 1 unit). Held 0.25 s.
void vr_cull_note_fade(float lit);
// Per edge of that quadrilateral, in its order: where the stock screen's own edge lies, as a share of the distance from the
// quadrilateral's middle to the edge (1 - 1 / k, k of LARECOMP_VR_CULL_RECT: 0 when the rectangle is the stock screen).
bool vr_cull_rect_shrink(float shrink[4]);
