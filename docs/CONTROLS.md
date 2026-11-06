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

**Menu and View.** A press of Menu or View is held back for 120 ms, or until you let go if that is sooner, to see
whether the other one joins it. If it does, that is the game's menu, and neither press reaches the game, so the pause
menu and the map don't open under it. If not, the press goes to the game as a press (a quick tap is held for at least
60 ms), so Menu alone still pauses and View alone still shows the map, a moment late.

At the start of the game Link has only the shield, so most buttons do
nothing until the sword, the Boots and the items are found.

## The table

The right stick, the left grip and the stick clicks move the table. None of
these reach the game.

| Steam Frame | The table |
|---|---|
| Right stick left / right | Turn the table. Pushing right takes the view round the table to the right. Turning is smooth and faster the further you push; with *Turning* on Snap, a flick turns it 45 degrees behind a short blink |
| Right stick up / down | Tilt the table. Up lifts the far edge toward you, from 0 to +50 degrees |
| Left grip + D-pad up / down | Raise and lower the table, from 50 cm lower to 40 cm higher |
| Left grip + D-pad right / left | Make the table bigger and smaller, from 40% to 300% |
| Click both thumbsticks | Reset the view. Hold for 2 seconds to reset everything |
| Left grip + right stick click | The same, with the same hold times |
| SteamVR's recentre | Put the table in front of you again, keeping its turn |

The right stick turns or tilts, never both: the direction you first push it
in counts, so a thumb that wanders sideways during a tilt does not turn the
table.

**Reset the view.** Click both thumbsticks together and hold them for a
moment. Behind a blink, the table turns back to the game's own angle (you see
it from where the game's camera looks) and goes in front of you again.
Height, distance, size and tilt stay as you set them. "View Reset" shows on
the panel for a moment. A single stick click still goes to the game.

**Reset everything.** Keep both thumbsticks down for 2 seconds. The view
resets again behind a blink, and the table's height, distance, size and tilt
go back to the defaults: 0.8 m away, 15 degrees below eye level, tilted 30
degrees toward you, so that Link is about where you look when you face
forward. "Full View Reset" shows on the panel. Left grip + a right stick click
has the same two steps.

**Height, distance, size and tilt** are saved to `vr_rig.ini` in the data folder a
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
Frame's A is then the Switch's A. This and the layout apply at once; a button
held through the change stays silent until you let go of it.

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

**Pointer.** While this menu is open, the pointing controller (the right one
by default) casts a green laser with a gold dot where it meets the panel. Point
at a row and pull its trigger to click. Hold it to drag a slider. Push the
right stick up or down to scroll, whichever hand points. The controller ticks
when the dot reaches a new control (off with the Controls tab's Rumble). That
trigger never reaches the game while the menu is open, so a click on the close
button doesn't swing the sword. *Pointer Hand* on the VR tab moves the laser to
the left controller. The game's own menus (pause, inventory) still use the
buttons.

## Rumble

When the game rumbles, both controllers vibrate. The Controls tab's Rumble
and Intensity settings apply.
