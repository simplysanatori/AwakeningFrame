# Frame controllers (VR mode)

`runtime/xr/xr_input.c` reads the Steam Frame controllers (`/interaction_profiles/valve/frame_controller`, extension
`XR_VALVE_frame_controller_interaction`; the Touch profile is bound too for other headsets) once per display refresh and
hands the result to the game as the buttons and left stick of a gamepad. The buttons go through the player's controller
bindings (Controls tab), so a rebind applies in VR as well; SDL gamepads keep working at the same time (inputs are ORed).
Menus (the start screen, the overlay) get the same presses as SDL gamepad events.

| Frame | Switch | Game |
|---|---|---|
| left stick | left stick, turned by the table's yaw | walk (up = away from the player) |
| right A / B / X / Y | A / B / X / Y | A: talk, read, lift, open; B: sword; X, Y: the equipped items |
| right trigger | B | sword (as the B button) |
| right grip | A | talk, lift, open |
| left trigger | R | shield (hold) |
| right bumper | L | Pegasus Boots |
| left bumper | ZL | (not used in play) |
| right menu | Plus | pause menu |
| left view | Minus | map |
| left D-pad | D-pad | the d-pad (items, when set to Items) |
| stick clicks | stick clicks | |
| right stick left / right | (rig) | snap turn (`xr_input_take_snap`) |
| right stick up / down | (rig) | table tilt (`XrRigInput.tilt`) |
| left grip held | (rig modifier) | the left D-pad becomes height (up / down) and size (right / left); a click of the right stick recentres the table |

The game's meaning of the Switch buttons is the Switch version's layout (the Items page has no Pegasus Boots or shield, so they
are fixed buttons: L and R). Not confirmed in play yet: the test save starts inside a dialogue. Rumble: `input_rumble_level` (the Controls tab's Rumble
and Intensity apply) drives both controllers with 120 ms pulses every 80 ms while it lasts.
