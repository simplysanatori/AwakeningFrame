# Frame controllers (VR mode)

`runtime/xr/xr_input.c` reads the Steam Frame controllers (`/interaction_profiles/valve/frame_controller`, extension
`XR_VALVE_frame_controller_interaction`; the Touch profile is bound too for other headsets, with a layout of its own:
"Touch controllers" below) once per display refresh and
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
| right grip | R | as the right bumper: shield (hold); either one presses R |
| left bumper | L | Pegasus Boots (once found) |
| right bumper | R | shield (hold) |
| left trigger | ZL | not used in play |
| right trigger | ZR | not used in play |
| right Menu | Plus | pause menu |
| left View | Minus | map |
| left D-pad | D-pad | the d-pad (items, when set to Items) |
| stick clicks | stick clicks | no function in play that this port knows of; a single click still reaches the game as the stick-click buttons |
| right stick left / right | (rig) | turns the table smoothly (`XrRigInput.turn`); with the Turning option on Snap, a flick turns it 45 degrees behind a blink (`xr_input_take_snap`) |
| right stick up / down | (rig) | table tilt (`XrRigInput.tilt`) |
| both stick clicks together (held 0.1 s) | (rig) | reset the view: the table goes back to the game's own angle and in front of you again (tilt, height, distance and size are kept). Held 2 s from the press: the full view reset, which also puts tilt, height, distance and size back to the defaults |
| left grip held | (rig modifier) | the left D-pad becomes height (up / down) and size (right / left); a click of the right stick does the same two resets as both stick clicks (0.1 s, 2 s) |

What the rig does with these (ranges, the blink, side views): docs/af/RIG.md.

**View + Menu.** The overlay menu opens on Back + Start together, and the other press must not reach the game as Minus
or Plus. `chord_filter` (`xr_input.c`) holds a press of either back for `LARECOMP_XR_CHORD_MS` (120 ms) or until it is
released: the other joining in that time is the chord, which goes to the menus only, both in one refresh, and keeps both
from the game until both are up. Otherwise the press goes to the game, a tap (released inside the window) replayed for
the time it was held, 60 ms at least. Once one has gone to the game, the other joining later is no chord: the game gets
it too, and the menus see only the first.

**Menu laser.** While the recomp menu or its name entry has the controls, the pointing controller (the right one, or the
left with the VR tab's Pointer Hand, `LARECOMP_VR_POINTER_HAND`) shows a beam and a dot where its aim ray meets the HUD
panel (`xr_pointer.c`, two quad layers drawn once; `xr_input_pointer` moves them each refresh for that refresh's display
time). A press of its trigger that began while the laser was shown belongs to the laser until it is let go: it clicks
the menu if it began on the panel (`menu_xr_pointer`), and neither the game nor the menu's pad input sees it. The right
stick scrolls while the dot is on the panel, and the controller ticks when the dot reaches a new control.

The right stick turns or tilts, not both: the axis it was first pushed along wins until the other is 1.4 times bigger, and
the other reads zero, so a thumb that wanders does not tilt a turn. Both stick clicks pressed together for 0.1 s are the
rig's: the game gets neither until both are let go. A single click, or both for less than 0.1 s, goes to the game (as far as this port knows the
game has no use for them, but a rebind in the Controls tab could give them one). "View Reset" shows on the HUD for a moment, "Full View Reset" after the 2 s hold, no sound.

The shield is R and the Pegasus Boots are L (they have no slot on the Items page, so they are fixed buttons; the touch
build's prompts say the same: "R appears with the shield, L with the Pegasus Boots"). In the Switch version ZL and ZR have no
function in play that this port knows of (the tracker overlay uses ZR on the map page). At the start of the game Link has only
the shield, so most buttons do nothing until the sword, the Boots and the items are found. Not confirmed in play yet.

Face buttons: the Frame's A B X Y are taken to be laid out as an Xbox pad's (A bottom, B right, X left, Y top) and are matched to the
Switch's diamond by position, like a Switch Pro controller in your hands (the Frame's B is the Switch's A). With the Controls tab's (or the VR tab's)
"Buttons match labels" on (`face_buttons_by_label = 1`) they match by letter instead (the Frame's A is the Switch's A). A rebind in
the Controls tab applies on top of either; the prompts follow it. In the menus the right button (the Frame's B, by position)
confirms, as on a Switch pad, unless the Controls tab's menu confirm is set to the controller's own convention.

## Comfort layout

The VR tab's Controller Layout (`LARECOMP_XR_LAYOUT=comfort` from the launch environment, e.g. `session.env`, also sets
it) switches to a layout built around the Frame's triggers rather than the Switch's: face buttons by letter, otherwise.
The change applies at once; a control held through it stays silent until let go (`xr_input_set_layout`):

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

## Touch controllers

On a PC (docs/af/PCVR.md) a Quest's controllers, or any the runtime offers as `/interaction_profiles/oculus/touch_controller`,
have their own layout. The program takes it as soon as the runtime names Touch as the profile in use
(`XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED`, `xrGetCurrentInteractionProfile`; `[xr] controller profile in use:` in
`lasrecomp.log`), and from the start when the runtime has no Frame profile. The Frame's layout above is not changed.

Touch has fewer controls than the game's Switch pad: no D-pad, no View button, no bumpers, and the Frame's modifier
(the left grip) is needed for a game button. The game needs the stick, A, B, X, Y, L, R, Plus and Minus in play (the touch build's
on-screen buttons are those); ZL and ZR have no use in play, and the D-pad only with LAS Recomp's D-pad Items or D-pad
Move options (off by default). So:

| Touch | Switch | Game |
|---|---|---|
| left stick | left stick, turned by the table's yaw | walk |
| right A (bottom) | B | sword |
| right B (top) | A | talk, read, lift, open, roll |
| left trigger | Y | the item on Y |
| right trigger | X | the item on X |
| left grip | L | Pegasus Boots |
| right grip | R | shield (hold) |
| left Menu | Plus | pause menu |
| left Y | Minus | map |
| stick clicks | stick clicks | as on the Frame |
| right stick | (rig) | turn and tilt, as on the Frame |
| both stick clicks together | (rig) | the view resets, as on the Frame (0.1 s, 2 s) |
| left X held | (rig modifier) | the right stick becomes height (up / down) and size (right / left), and does not turn or tilt; a click of the right stick does the two resets |

Left Y and left Menu together open the overlay menu (the View + Menu chord, with the same filter). With Buttons Match
Labels on, right A is A and right B is B. The right controller's system button is the runtime's.

Comfort layout on Touch: right trigger B (sword), left trigger R (shield), right grip A, left grip L (Boots), right A Y,
right B X; Menu, left Y, left X and the sticks as above.

Not on Touch: ZL, ZR and the D-pad. A player who sets D-pad Items or D-pad Move can bind them to other controls in the
Controls tab. The menu laser's trigger is the item button's: while the laser is shown, its trigger clicks the menu.

Why this layout (the owner's choices are open: docs/af/PCVR.md "Touch"): the sword and the action stay on the right
thumb's two buttons, at the Switch's positions; the two items go to the triggers, which are free (no ZL / ZR in play)
and can be pressed together with the sword; the Boots and the shield are held buttons, on the grips, which are held;
Plus and Minus go to the left hand's Menu and Y, so the overlay menu's chord is two buttons of one hand; left X is the
only button left for the modifier, and with the D-pad missing the right stick takes the D-pad's part under it.

