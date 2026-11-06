// The Frame controllers in VR mode (xr_input.c): the actions, their bindings, and the hooks into xr_session.c / xr_loop.c.
// The public side (the rig's events, the yaw setter) is at the end of xr.h.
#pragma once
#include "xr_internal.h"

#define XR_INPUT_VALVE_EXT "XR_VALVE_frame_controller_interaction"  // /interaction_profiles/valve/frame_controller

// After xrCreateInstance and the function pointers: the action set, the actions and the suggested bindings. valve: the
// instance was made with XR_INPUT_VALVE_EXT.
void xr_input_init(PFN_xrGetInstanceProcAddr gipa, bool valve);
// After xrCreateSession: attaches the action set (nothing can be bound after that).
void xr_input_attach(void);
// The XR thread, once per display refresh (the session is running): xrSyncActions, the state to the game and the rig,
// and the game's rumble to both controllers.
void xr_input_frame(void);
// Before xrDestroySession.
void xr_input_shutdown(void);

// runtime/hle/input.c (built with the VR mode: LARECOMP_HAVE_XR)
// The Frame controllers' state: `host` has bit n set for SDL_GamepadButton n held (bits 26, 27: the left / right
// trigger as ZL / ZR, input_bind.h PADB_*), lx / ly the left stick (game convention: +y up, +-32767), already rotated by
// the diorama's yaw. Game buttons go through the player's controller bindings as a gamepad's would; menus get the same
// as SDL gamepad events.
void input_xr_set(uint32_t host, int32_t lx, int32_t ly);
// The game's rumble now as one strength 0..1 (the same mix and gates as the gamepad's: the Controls tab's Rumble and
// Intensity, the menu open, the game's own vibration permission), 0: none.
float input_rumble_level(void);
