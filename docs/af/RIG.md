# Diorama rig (AwakeningFrame lane "rig")

The rig decides where the game world sits in the player's room, what the player's controls do to it, and hides every
sudden change behind a short blink. Design: `~/.claude/plans/awakeningframe-vr-design.md` section 6. Code:
`runtime/xr/xr_rig.c/.h`; it is called once per game frame from the eye cameras (`runtime/nvn/vulkan/stereo.c`), and
the blink is applied in the present shader (`runtime/nvn/screen_filter.h`, `pf.y`).

## The transform

`app = A + s * R * (world - anchor)`, made once per latched game frame (the table moves only on new game frames, so
it is never shown at two places within one game frame: GalaxyFrame's double-image lesson).

- **app**: the XR app space (metres, y up, -z ahead). Its origin is the head at the session's start or the last
  recentre (`xr_session.c`).
- **A**: the table point the anchor sits on. It is `LARECOMP_VR_DIST` (1.0 m) in front of the head at
  `LARECOMP_VR_PITCH` degrees below the horizon (default: the game camera's own pitch at the first frame, clamped to
  25-65, about 45 at Mabe), plus the player's height offset. `LARECOMP_VR_TABLE=x,y,z` fixes it.
- **s**: `LARECOMP_VR_SCALE` (0.067 m per unit: one tile about 10 cm) times the player's size.
- **R**: `Rx(tilt) * Ry(yaw)`. The yaw turns the table about the vertical (counter-clockwise seen from above). The
  tilt turns it about the player's left-right axis through A; positive lifts the far edge towards the player.

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
there, and A drops to 12 degrees below the horizon (about eye height) instead of the table's 45, so the player looks
at the wall rather than down onto its top edge. The player's height and size still apply. Leaving the side view
restores the player's yaw and tilt (behind the blink of the area change).

`LARECOMP_VR_FOLLOW=smooth|room|fixed` (the comfort lane's follow, `stereo.c anchor_update`) replaces the per-area
anchoring with that follow; camera jumps still blink. Unset or `auto`: the per-area anchoring above.

## Controls

| Frame | Rig |
|---|---|
| right stick left / right (a flick) | snap turn 45 degrees behind a blink. Right: the view goes a step round the table to the right (the table turns clockwise), as in GalaxyFrame. Not in side views |
| right stick up / down | tilt, 45 degrees a second (outside a 0.3 dead zone), from -20 to +50 degrees. Up lifts the far edge. A tilt away is also limited so the head looks at least 20 degrees down onto the table (a grazing look put the eyes under Mabe's ledges). Not in side views |
| left grip + D-pad up / down | table height, 0.3 m a second, from -0.5 to +0.4 m |
| left grip + D-pad right / left | table size, doubling or halving in about a second, from x0.4 to x3 |
| left grip + right stick click | recentre: the table goes in front of the player again, at the head's height, behind a blink |
| SteamVR recentre | the same (the runtime moves LOCAL; `xr_session.c` re-makes the app space at the head) |

The left stick is turned by the table's yaw (`xr_input_set_yaw`, called whenever the yaw changes; in side views with
the side yaw), so "up" walks Link away from the player. The rig's inputs never reach the game (`runtime/xr/xr_input.c`).

## The blink

On screen only, no sound. The present multiplies the eye images and the HUD by `1 - fade`
(`screen_filter_params` sets `pf.y = xr_rig_fade()`).

- Snap turns, recentres and yaw changes from the settings fade out (0.08 s), the change is made at the first game
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
typedef struct { float height, size, tilt, yaw; } XrRigUser;  // m, factor, degrees, degrees
void xr_rig_get_user(XrRigUser *u);
void xr_rig_set_user(const XrRigUser *u);  // clamped; the yaw behind a blink
void xr_rig_reset_user(void);              // 0, 1, 0, 0
void xr_rig_recentre(void);
// ranges: XR_RIG_HEIGHT_MIN/MAX (-0.5, 0.4), XR_RIG_SIZE_MIN/MAX (0.4, 3), XR_RIG_TILT_MIN/MAX (-20, 50)
```

Only Linux builds with the VR mode (the OpenXR headers: `runtime/xr/*.c` is built then) have these: callers guard with
the build's XR flag.

Height, size and tilt are saved (about a second after the last change) to `vr_rig.ini` in the data root, or next to
`LARECOMP_SETTINGS`' file when that is set (test runs), or `LARECOMP_VR_RIG=<path>`; `LARECOMP_VR_RIG_SAVE=0` reads
it but never writes it. The yaw is not saved (each session starts facing the game's own up).

The VR settings tab's Height, Size and Tilt rows and Recentre Table are this API: `runtime/xr/xr_rig_settings.c` provides
the tab's hooks (`xr_rig_set_height_cm`, `_size_pct`, `_tilt_deg`; units converted, the other fields kept) and the tab
reads `xr_rig_get_user` each time it draws, so the rig is the single source of truth (`larecomp.ini` keeps none of them;
docs/af/VR_SETTINGS.md).

## Switches

| Variable | Meaning |
|---|---|
| `LARECOMP_VR_DIST`, `LARECOMP_VR_PITCH`, `LARECOMP_VR_TABLE`, `LARECOMP_VR_SCALE` | placement (above) |
| `LARECOMP_VR_HEIGHT`, `LARECOMP_VR_SIZE`, `LARECOMP_VR_TILT`, `LARECOMP_VR_YAW` | the player's values at the start (tests; override the file) |
| `LARECOMP_VR_FOLLOW=auto\|smooth\|room\|fixed`, `LARECOMP_VR_FOLLOW_TAU` | anchoring (above) |
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
