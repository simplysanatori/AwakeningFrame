# Controls (Steam Frame controllers)

The game still sees a Switch Pro Controller. The Frame controllers are mapped
onto it control for control, by position, so the game's own button prompts
(A, B, X, Y, L, R, ZL, ZR, Plus, Minus) name the Frame control that does it.
The mapping lives in `runtime/xr/xr_input.c`. The presses go through the
Controls tab's bindings, so a rebind there applies in VR too, and an SDL
gamepad keeps working alongside the Frame controllers.

| Steam Frame | Switch | In Link's Awakening |
|---|---|---|
| Left stick | Left stick | Walk. Up walks Link away from you, whichever way the table faces |
| **A** (right controller, bottom) | B | Sword |
| **B** (right controller, right) | A | Talk, read, lift, open |
| **X** (right controller, left) | Y | The item on Y |
| **Y** (right controller, top) | X | The item on X |
| Right grip | A | Talk, read, lift, open |
| Left bumper | L | Pegasus Boots, once found |
| Right bumper | R | Shield (hold) |
| Left trigger | ZL | Not used in play |
| Right trigger | ZR | Not used in play |
| Right **Menu** | Plus | Pause menu |
| Left **View** | Minus | Map |
| **View** and **Menu** together | Minus and Plus | The game's menu, with the VR tab |
| Left D-pad | D-pad | D-pad items, when the D-pad is set to items |
| A single stick click | Stick click | Reaches the game; it has no use in play |

At the start of the game Link has only the shield, so most buttons do
nothing until the sword, the Boots and the items are found.

## The table

The right stick, the left grip and the stick clicks move the table. None of
these reach the game.

| Steam Frame | The table |
|---|---|
| Right stick left / right | Turn the table. Pushing right takes the view round the table to the right. Turning is smooth and faster the further you push; with *Turning* on Snap, a flick turns it 45 degrees behind a short blink |
| Right stick up / down | Tilt the table. Up lifts the far edge toward you, from -20 to +50 degrees |
| Left grip + D-pad up / down | Raise and lower the table, from 50 cm lower to 40 cm higher |
| Left grip + D-pad right / left | Make the table bigger and smaller, from 40% to 300% |
| Click both thumbsticks | Reset the view |
| Left grip + right stick click | Reset the view, the same |
| SteamVR's recentre | Put the table in front of you again, keeping its turn |

The right stick turns or tilts, never both: the direction you first push it
in counts, so a thumb that wanders sideways during a tilt does not turn the
table.

**Reset the view.** Click both thumbsticks together and hold them for a
moment. Behind a blink, the table turns back to the game's own angle (you see
it from where the game's camera looks) and goes in front of you again.
Height, size and tilt stay as you set them. "View Reset" shows on the panel
for a moment. A single stick click still goes to the game.

**Height, size and tilt** are saved to `vr_rig.ini` in the data folder a
moment after you stop, and are the same rows as on the VR tab. The turn is
not saved: each start faces the game's own angle.

**Side views.** In the side-scrolling passages the table stands upright like
a wall and faces you. Turning and tilting do nothing there. Height and size
still apply.

**The blink.** Turns, resets and every sudden change (a room change, a door,
a warp, a camera cut) blink to black and back. There is no sound.

## Layouts

**Buttons Match Labels** (VR tab or Controls tab, off by default): A, B, X
and Y work as printed on the Frame controllers instead of by position. The
Frame's A is then the Switch's A.

**Comfort** (VR tab, *Controller Layout*): built around the Frame's
triggers instead of the Switch's layout, with the face buttons by label.

| Steam Frame | Switch | In Link's Awakening |
|---|---|---|
| Right trigger | B | Sword |
| Left trigger | R | Shield (hold) |
| Right bumper | L | Pegasus Boots |
| Left bumper | ZL | Not used in play |
| Right grip | A | Talk, lift, open |

Everything else (Menu, View, the D-pad, the sticks and the table) is as above.

## Menus

In the game's menu (**View** and **Menu** together) the bumpers switch tabs,
up and down move between rows, A or right steps a value forward, left steps
it back, and B goes back to the tab bar. Rows set by a variable in
`session.env` are greyed out and skipped.

In menus the right face button (the Frame's B, by position) confirms, as on a
Switch, unless the Controls tab sets the controller's own convention.

**Pointer.** While this menu is open, the right controller casts a green ray
with a gold dot where it meets the panel. Point at a row and pull the right
trigger to click. Hold it to drag a slider. Push the right stick up or down
to scroll. The controller ticks when the dot reaches a new control (off with
the Controls tab's Rumble). The trigger never reaches the game while the menu
is open, so a click on the close button doesn't swing the sword. *Pointer
Hand* on the VR tab moves the ray to the left controller. The game's own
menus (pause, inventory) still use the buttons.

## Rumble

When the game rumbles, both controllers vibrate. The Controls tab's Rumble
and Intensity settings apply.
