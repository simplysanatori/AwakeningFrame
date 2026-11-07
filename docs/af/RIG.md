# Diorama rig

The rig decides where the game world sits in the player's room, what the player's controls do to it, and hides every
sudden change behind a short blink. Code:
`runtime/xr/xr_rig.c/.h`; it is called once per game frame from the eye cameras (`runtime/nvn/vulkan/stereo.c`), and
the blink is applied in the present shader (`runtime/nvn/screen_filter.h`, `pf.y`).

## The transform

`app = A + s * R * (world - anchor)`, made once per latched game frame (the table moves only on new game frames, so
it is never shown at two places within one game frame: GalaxyFrame's double-image lesson).

- **app**: the XR app space (metres, y up, -z ahead). Its origin is the head at the session's start or the last
  recentre (`xr_session.c`).
- **A**: the table point the anchor sits on. It is the player's Distance (default 0.8 m, `LARECOMP_VR_DIST`) in front of
  the head at `LARECOMP_VR_PITCH` degrees below the horizon (default 15, see Default placement), plus the player's height
  offset. `LARECOMP_VR_TABLE=x,y,z` fixes it.
- **s**: `LARECOMP_VR_SCALE` (0.067 m per unit: one tile about 10 cm) times the player's size.
- **R**: `Rx(tilt) * Ry(yaw)`. The yaw turns the table about the vertical (counter-clockwise seen from above). The
  tilt turns it about the player's left-right axis through A; positive lifts the far edge towards the player. The
  default tilt is 30 degrees.

## Default placement

With a neutral head (looking at the horizon) the player should see Link: the table's centre, where the anchor is, sits
15 degrees below the horizon, which is where a relaxed gaze rests, 0.8 m away. Before (up to 87b49f9) the centre was
about 45 degrees down at 1.0 m (the game camera's pitch, `R.pitch` from the first frame), so Link was a 45-degree head
nod away whatever the tilt: the tilt turns the table about A, it never moves A.

The look onto the table is the angle between the line of sight and the table's plane: A's angle below the horizon plus
the tilt. The game's own camera looks down at 45, so the default tilt is 30: 15 + 30 = 45, and the table is seen at the
game's angle. At the defaults (0.067 m per unit, the table 1 m x 0.8 m for one screen of 15 x 12 units) the table spans
about 4 to 32 degrees below the horizon (far edge to near edge), and about 45 degrees across. A = (0, -0.31, -1.16) m.
Link's angle below the horizon is `atan(-A.y / -A.z)` plus the height offset: 15 degrees at Height 0, 5 at +20 cm, 25 at
-20 cm. Distance moves A along the line of sight and does not change the angle. The lowest tilt the rig accepts is the
one that keeps the line of sight 20 degrees (MIN_ELEV) above the table's plane: at the defaults 5 degrees, so the tilt
range is 0 to 50 and the effective floor is higher while the table is higher.

The eye cameras (`stereo.c eye_fn`) put each eye at `anchor + R^T (eye - A) / s` in the world, turned by `R^T`. The
anchor also goes to the culling fit (`cam_override_table_anchor`, `vr_cull.c`).

## Anchoring by area

The area comes from the level (`widescreen.c game_level_overworld`: the overworld is the level "Field") and the
camera zone (`widescreen.c current_zone`, published through `cam_override_zone`):

| Area | How it is told | Anchor | On a change |
|---|---|---|---|
| overworld | level "Field" (its camera zones, 240 x 198 units at Mabe, are areas, not rooms) | the game camera's focus at Link's height, filtered (time constant `LARECOMP_VR_FOLLOW_TAU`, 0.5 s; height twice that) | a camera cut (focus 12 units in one frame) or 24 units off: blink, snap |
| interior / dungeon room | another level, a zone at least 6 units deep (a house or a Tail Cave room: 15 x 12) | the room's centre at the height it was entered at (Link's jumps and falls into holes leave it); a room bigger than a screen keeps the screen inside it (the focus clamped half a screen in from its walls), filtered | another room: blink, snap to its centre (at the start of the game's scroll) |
| side view | a zone thinner than 6 units in z (Bottle Grotto 05D: 32 x 12 x 3), or a level camera | the focus on the side plane, filtered | blink, snap |

A new area or room must hold for 3 game frames before the rig takes it (a zone read flickering during a load does
not blink), except on a camera cut. In a dungeon or interior a frame with no zone (between rooms, a fall) keeps the
room.

**Side views** are shown upright like a wall diorama: the table turns so the side plane faces the player (the yaw that
takes the game camera's horizontal forward away from the player), the player's tilt and snap turns are not used
there, and A drops to 12 degrees below the horizon (about eye height) instead of the table's 15, so the player looks
at the wall rather than down onto its top edge. The player's height and size still apply. Leaving the side view
restores the player's yaw and tilt (behind the blink of the area change).

`LARECOMP_VR_FOLLOW=smooth|room|fixed` (the follow of `stereo.c anchor_update`) replaces the per-area
anchoring with that follow; camera jumps still blink. Unset or `auto`: the per-area anchoring above.

## Controls

| Frame | Rig |
|---|---|
| right stick left / right | smooth turning (the VR tab's Turning: Smooth, the default): the table turns about its centre (the anchor) at up to 80 degrees a second, outside a 0.2 dead zone, on an eased curve of the deflection (`t * (0.4 + 0.6 t)`: half a stick is about a third of the speed), and the turn speed follows the stick's with a 0.12 s lag, so it starts and stops without a jerk. Right: the view goes round the table to the right (the table turns clockwise), as in GalaxyFrame. Not in side views |
| right stick left / right (a flick) | with Turning on Snap: a snap turn of 45 degrees behind a blink, the same direction. Not in side views |
| right stick up / down | tilt, 45 degrees a second (outside a 0.3 dead zone), from 0 to +50 degrees. Up lifts the far edge. A tilt away is also limited so the head looks at least 20 degrees down onto the table (a grazing look put the eyes under Mabe's ledges). Not in side views |
| both stick clicks together (0.1 s) | reset the view: behind a blink the yaw goes back to 0, the game's own angle (the table seen from where the game's camera looks), and the table goes in front of the player again, as a recentre. Tilt, height, distance and size are kept. "View Reset" shows on the HUD for about a second and a half. A single click goes to the game |
| both stick clicks held 2 s from the press | the full view reset: a second blink in which the view reset is made again and tilt (30), height (0), distance (0.8 m) and size (x1) go back to the defaults and are saved. "Full View Reset" shows on the HUD. `xr_input.c` sends it once per hold; it replaces the short reset, which is not made twice (both in one frame: only the full one runs) |
| left grip + right stick click | the same two resets (an alias): the short one at 0.1 s, the full one at 2 s from the press |
| left grip + D-pad up / down | table height, 0.3 m a second, from -0.5 to +0.4 m |
| left grip + D-pad right / left | table size, doubling or halving in about a second, from x0.4 to x3 |
| SteamVR recentre | a recentre: the table in front again, the yaw kept (the runtime moves LOCAL; `xr_session.c` re-makes the app space at the head). The VR tab's Recentre Table does the same |

The right stick turns or tilts, never both (`xr_input.c`): the axis it was first pushed along is the one that counts until the
other is 1.4 times bigger, and the other reads zero (a thumb pushed up and a little sideways tilts only).

Smooth turning is applied once per game frame, in `xr_rig_update` like everything the rig does to the table, integrating
with the game frame's `dt`, not once per display refresh. The eye images are drawn once per game frame, with the table's
pose in them, and the compositor shows that frame for one refresh (or two at 120 Hz / 60 FPS); a table that moved between
refreshes would be shown at two places within one game frame (GalaxyFrame's double image), and moving the pose per refresh
would need a re-render. So the turn is as smooth as the game itself: at 120 Hz / 60 FPS the table steps 1.3 degrees a game
frame at the full 80 degrees a second (a tile 10 cm across at 0.5 m from the centre moves 1 cm), at 90 Hz / 30 FPS 2.7
degrees, like Link's own steps. The speed is integrated over the time since the last game frame (capped at 0.1 s), so the
rate does not depend on the frame rate.

The left stick is turned by the table's yaw (`xr_input_set_yaw`, called whenever the yaw changes; in side views with
the side yaw), so "up" walks Link away from the player. The rig's inputs never reach the game (`runtime/xr/xr_input.c`).

## The blink

On screen only, no sound. The present multiplies the eye images and the HUD by `1 - fade`
(`screen_filter_params` sets `pf.y = xr_rig_fade()`).

- Snap turns, recentres, view resets (short and full) and yaw changes from the settings fade out (0.08 s), the change is made at the first game
  frame once black, the image stays black 0.05 s (recentre 0.08 s: the new app space is made on the next refresh),
  then fades in (0.15 s).
- Automatic cuts (another room or area, a warp, a door, a camera cut in a cutscene) go black at once on the frame
  that would have jumped, the table snaps on that frame, then it fades in (0.22 s).
- Black with no game frame coming (a menu) for 0.3 s fades back in anyway; a pending recentre is done then, a pending
  turn at the next game frame.

Cutscene pans move the game camera's focus smoothly, so the overworld follow glides with them; only cuts blink. A
door from the overworld into a house gives two or three cuts within about a second (the door camera, the new level,
its first camera): each blinks; the game's own fade to black is on the HUD quad meanwhile.

## Settings (for the VR settings tab)

`runtime/xr/xr_rig.h`, any thread:

```c
typedef struct { float height, size, tilt, dist, yaw; } XrRigUser;  // m, factor, degrees, m, degrees
void xr_rig_get_user(XrRigUser *u);
void xr_rig_set_user(const XrRigUser *u);  // clamped; the yaw behind a blink
void xr_rig_reset_user(void);              // 0, 1, 30, 0.8, 0
void xr_rig_recentre(void);
void xr_rig_reset_view(void);              // the same, and the yaw back to 0; tilt, height, distance, size kept
void xr_rig_reset_view_full(void);         // and tilt, height, distance and size back to the defaults (saved)
void xr_rig_set_turn_mode(int snap);       // 1: snap turns, 0: smooth turning (the default; LARECOMP_VR_TURN=smooth|snap)
// ranges: XR_RIG_HEIGHT_MIN/MAX (-0.5, 0.4), XR_RIG_DIST_MIN/MAX (0.6, 3), XR_RIG_SIZE_MIN/MAX (0.4, 3), XR_RIG_TILT_MIN/MAX (0, 50)
// defaults: XR_RIG_PITCH_DEFAULT 15, XR_RIG_TILT_DEFAULT 30, XR_RIG_DIST_DEFAULT 0.8
```

Only Linux builds with the VR mode (the OpenXR headers: `runtime/xr/*.c` is built then) have these: callers guard with
the build's XR flag.

Height, distance, size and tilt are saved (about a second after the last change) to `vr_rig.ini` in the data root, or next to
`LARECOMP_SETTINGS`' file when that is set (test runs), or `LARECOMP_VR_RIG=<path>`; `LARECOMP_VR_RIG_SAVE=0` reads
it but never writes it. The yaw is not saved (each session starts facing the game's own up).

The VR settings tab's Height, Distance, Size and Tilt rows and Recentre Table are this API: `runtime/xr/xr_rig_settings.c` provides
the tab's hooks (`xr_rig_set_height_cm`, `_dist_cm`, `_size_pct`, `_tilt_deg`; units converted, the other fields kept) and the tab
reads `xr_rig_get_user` each time it draws, so the rig is the single source of truth (`larecomp.ini` keeps none of them;
docs/af/VR_SETTINGS.md).

## Switches

| Variable | Meaning |
|---|---|
| `LARECOMP_VR_DIST`, `LARECOMP_VR_PITCH`, `LARECOMP_VR_TABLE`, `LARECOMP_VR_SCALE` | placement (above); `_DIST` is also the default Distance the full reset returns to |
| `LARECOMP_VR_HEIGHT`, `LARECOMP_VR_DISTANCE`, `LARECOMP_VR_SIZE`, `LARECOMP_VR_TILT`, `LARECOMP_VR_YAW` | the player's values at the start (tests; override the file) |
| `LARECOMP_VR_FOLLOW=auto\|smooth\|room\|fixed`, `LARECOMP_VR_FOLLOW_TAU` | anchoring (above) |
| `LARECOMP_VR_TURN=smooth\|snap` | the right stick's turning (the VR tab's Turning; default smooth) |
| `LARECOMP_VR_RIG=<path>`, `LARECOMP_VR_RIG_SAVE=0` | the settings file |
| `LARECOMP_LOG_RIG=<frames>` | the rig's state every that many game frames |

## Tests

xrmock with scripted controllers (`MOCK_XR_INPUT`, docs/af/TOOLING.md), `LARECOMP_LOG_RIG=20` and the eye images
(`MOCK_XR_SHOT`). Rebuild the mock (`tools/xrmock/build.sh`) if `MOCK_XR_INPUT` does nothing: older builds lack it.
Each shot stalls the loop about 2 s and the script runs on the wall clock, so hold scripted inputs 3 s and leave 8 s
between actions; run with `LARECOMP_XR_PREDICT=vsync` (the measured latch-to-display time grows with the stalls and
the poses are then located seconds ahead, into the next scripted head move).

Checked under xrmock (2026-11-06): snap turns both ways, tilt (with the 20-degree limit), height and size to their
limits, Link walking on the overworld with the table turned 45 degrees (the stick turned with it, the anchor
following), a door into a house (blinks, the room anchored at its centre), a head turn of 60 degrees and a recentre
(the table back in front), a room change in Tail Cave (08D to the room north, snapped behind a blink at the start of
the scroll), a fall into a hole (the table holds), and Bottle Grotto 05D's side view (an upright wall facing the
player; snap turns and tilt ignored).

Checked under xrmock (scripted both-stick and left-grip clicks, start values set by
`LARECOMP_VR_TILT/HEIGHT/DISTANCE/SIZE/YAW`): a 1 s hold of both sticks makes one short reset; 1.5 s still only one;
3 s makes the short reset at 0.1 s and the full one 2 s from the press, once; left grip + right stick click does the
same (1 s: short only, 3 s: both). The full reset logged `tilt 17 -> 30, height +0.24 -> +0.00, distance 1.20 -> 1.20,
size x1.75 -> x1.00` and `tilt 10 -> 30, height +0.10 -> 0, distance 1.60 -> 1.20, size x1.50 -> x1.00`; `vr_rig.ini` then
held `distance = 1.200`, `tilt = 30.0`.
