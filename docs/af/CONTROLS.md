# Frame controllers (VR mode)

`runtime/xr/xr_input.c` reads the Steam Frame controllers (`/interaction_profiles/valve/frame_controller`, extension
`XR_VALVE_frame_controller_interaction`; the Touch profile is bound too for other headsets) once per display refresh and
hands the result to the game as the buttons and left stick of a gamepad. The buttons go through the player's controller
bindings (Controls tab), so a rebind applies in VR as well; SDL gamepads keep working at the same time (inputs are ORed).
Menus (the start screen, the overlay) get the same presses as SDL gamepad events.

The default layout is a Switch Pro controller, control for control, so the game's own button prompts (A, B, X, Y, L, R, ZL, ZR,
Plus, Minus) name the Frame control that does it:

| Frame | Switch | Game (Link's Awakening, Switch 2019) |
|---|---|---|
| left stick | left stick, turned by the table's yaw | walk (up = away from the player) |
| right bottom button (A) | B (the Switch's bottom button) | sword |
| right right button (B) | A (the Switch's right button) | talk, read, lift, open, roll |
| right left button (X) | Y (the Switch's left button) | the item on Y |
| right top button (Y) | X (the Switch's top button) | the item on X |
| right grip | A | as A: talk, lift, open |
| left bumper | L | Pegasus Boots (once found) |
| right bumper | R | shield (hold) |
| left trigger | ZL | not used in play |
| right trigger | ZR | not used in play |
| right Menu | Plus | pause menu |
| left View | Minus | map |
| left D-pad | D-pad | the d-pad (items, when set to Items) |
| stick clicks | stick clicks | |
| right stick left / right | (rig) | snap turn (`xr_input_take_snap`) |
| right stick up / down | (rig) | table tilt (`XrRigInput.tilt`) |
| left grip held | (rig modifier) | the left D-pad becomes height (up / down) and size (right / left); a click of the right stick recentres the table |

The shield is R and the Pegasus Boots are L (they have no slot on the Items page, so they are fixed buttons; the touch
build's prompts say the same: "R appears with the shield, L with the Pegasus Boots"). In the Switch version ZL and ZR have no
function in play that this port knows of (the tracker overlay uses ZR on the map page). At the start of the game Link has only
the shield, so most buttons do nothing until the sword, the Boots and the items are found. Not confirmed in play yet.

Face buttons: the Frame's A B X Y are taken to be laid out as an Xbox pad's (A bottom, B right, X left, Y top) and are matched to the
Switch's diamond by position, like a Switch Pro controller in your hands (the Frame's B is the Switch's A). With the Controls tab's
"Buttons match labels" on (`face_buttons_by_label = 1`) they match by letter instead (the Frame's A is the Switch's A). A rebind in
the Controls tab applies on top of either; the prompts follow it. In the menus the right button (the Frame's B, by position)
confirms, as on a Switch pad, unless the Controls tab's menu confirm is set to the controller's own convention.

## Comfort layout

`LARECOMP_XR_LAYOUT=comfort` (the environment of the launch, e.g. in `session.env`) restores the first layout, built around
the Frame's triggers rather than the Switch's: face buttons by letter, otherwise:

| Frame | Switch | Game |
|---|---|---|
| right trigger | B | sword |
| left trigger | R | shield (hold) |
| right bumper | L | Pegasus Boots |
| left bumper | ZL | not used in play |
| right grip | A | talk, lift, open |

Everything else (Menu, View, D-pad, sticks, stick clicks, left grip, the rig) is as above. The layout in use is on the first
`[xr] controller layout:` line of `lasrecomp.log`; `LARECOMP_XR_INPUT_LOG=1` logs every change as the Switch inputs it makes.

Rumble: `input_rumble_level` (the Controls tab's Rumble
and Intensity apply) drives both controllers with 120 ms pulses every 80 ms while it lasts.
