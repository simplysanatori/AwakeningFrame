// The diorama rig (docs/af/RIG.md). Where the game world sits in the player's room, what the player's controls do to it, and the blink
// that hides every sudden change.
//
//   app = A + s * R * (world - anchor)      (app: the XR app space, metres, y up, -z ahead, origin = the head after
//                                            the last recentre; world: game units)
//   anchor  the world point on the table's centre: per area type (overworld: the game camera's focus at Link's height,
//           filtered; interiors and dungeon rooms: the room's centre, snapped behind a blink on a room change; side
//           views: the focus on the side plane, filtered) or, with LARECOMP_VR_FOLLOW set, the Follow Mode's follow
//   A       the table point in app space: the player's Distance in front of the head (default 1.2 m, LARECOMP_VR_DIST) at
//           LARECOMP_VR_PITCH below the horizon (default 15: Link is about where a relaxed gaze rests; side views:
//           SIDE_PITCH, a wall at about eye height), plus the player's height offset
//   s       LARECOMP_VR_SCALE (0.067 m per unit) times the player's size
//   R       Rx(tilt) * Ry(yaw): the player's tilt (side views: none) and yaw (side views: the side plane faces the player)
//
// Threads: xr_rig_update and xr_rig_fade run on the render thread (the latch, the present); the user get / set API on
// any thread (a mutex). Only new game frames move the table (xr_rig_update is called once per latch).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- stereo.c: once per latched game frame
typedef struct {
    float raw[3];          // the game camera's screen centre at Link's height (cam_override's pivot), world units
    float followed[3];     // the same followed per LARECOMP_VR_FOLLOW (stereo.c anchor_update)
    bool explicit_follow;  // LARECOMP_VR_FOLLOW was set: followed is the anchor (no area logic)
    int overworld;         // the level is the overworld ("Field"): 1; another level: 0; unknown: -1
    bool zone;             // a camera zone (the overworld has them too: its camera areas)
    float zlo[3], zhi[3];  // its box
    float fwd[3];          // the game camera's forward (world)
    uint64_t frame;        // the latched game frame (logs)
} XrRigIn;
enum { XR_RIG_OPEN, XR_RIG_ROOM, XR_RIG_SIDE };
typedef struct {
    float anchor[3];  // world
    float A[3];       // app space, metres
    float s;          // metres per game unit
    float R[9];       // world -> app rotation, row-major
    int area;         // XR_RIG_OPEN / XR_RIG_ROOM / XR_RIG_SIDE
} XrRigPose;
void xr_rig_update(const XrRigIn *in, XrRigPose *out);

// The blink: 0 (none) .. 1 (black), now. The present multiplies the eye images and the HUD by 1 - this
// (runtime/nvn/screen_filter.h).
float xr_rig_fade(void);

// ---- the player's settings (the VR settings tab): read and written on any thread; set clamps to the ranges below and
// saves them (height, size and tilt; the yaw is not saved) to vr_rig.ini in the data root (next to LARECOMP_SETTINGS'
// file when that is set; LARECOMP_VR_RIG=<path> names another file) about a second after the last change.
#define XR_RIG_HEIGHT_MIN (-0.50f)  // metres, added to the table's height (0: the default placement)
#define XR_RIG_HEIGHT_MAX (0.40f)
#define XR_RIG_SIZE_MIN (0.40f)     // times LARECOMP_VR_SCALE (1: one tile about 10 cm)
#define XR_RIG_SIZE_MAX (3.00f)
#define XR_RIG_TILT_MIN (0.0f)      // degrees; positive lifts the table's far edge towards the player
#define XR_RIG_TILT_MAX (50.0f)
#define XR_RIG_DIST_MIN (0.60f)     // metres from the head to the table's centre
#define XR_RIG_DIST_MAX (3.00f)
// the defaults (a new player, and the full view reset): the table's centre 15 degrees below the horizon, 1.2 m away, leaned
// 30 degrees so that the head looks onto it at 15 + 30 = 45 degrees, the game camera's own angle (docs/af/RIG.md)
#define XR_RIG_PITCH_DEFAULT (15.0f)
#define XR_RIG_TILT_DEFAULT (30.0f)
#define XR_RIG_DIST_DEFAULT (1.20f)
typedef struct {
    float height;  // metres
    float size;    // scale factor
    float tilt;    // degrees
    float dist;    // metres, the table's centre in front of the head (XR_RIG_DIST_*)
    float yaw;     // degrees, counter-clockwise seen from above (snap turns step it by 45, smooth turning moves it)
} XrRigUser;
void xr_rig_get_user(XrRigUser *u);
void xr_rig_set_user(const XrRigUser *u);  // applied on the next game frame (a yaw change behind a blink)
void xr_rig_reset_user(void);              // height 0, size 1, tilt and distance the defaults, yaw 0 (saved)
void xr_rig_recentre(void);                // the table in front of the player again, at the head's height (a blink)
void xr_rig_reset_view(void);              // the same, and the yaw back to 0 (the game's own angle); tilt, height, size and distance kept (a blink)
void xr_rig_reset_view_full(void);         // the view reset, and tilt, height, size and distance back to the defaults too (a blink; saved)
void xr_rig_set_turn_mode(int snap);       // 1: right stick sideways snaps 45 degrees behind a blink; 0 (default): turns smoothly

#ifdef __cplusplus
}
#endif
