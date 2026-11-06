# VR tab

The settings menu has a VR tab (second, after Controls; shown in VR mode, or with `LARECOMP_SHOW_VR_TAB=1` in a flat run).
Its options are the ones that were environment variables. They are kept in `larecomp.ini`; each one maps onto the variable
the VR code already reads, and an environment variable still wins. Code: `runtime/vr_settings.c/.h` (ini keys, the
mapping, the hooks), `runtime/ui/vr_tab.mm` (the page), `runtime/xr/vrui_hook.c` (the HUD panel's hook).

## How a saved value reaches the VR code

1. At startup (`settings_load`, before anything reads the environment) `vr_settings_apply_env` sets each variable from
   the saved value, when the variable is not already set. A value is left out when it is what the code behind the variable
   does without it (refresh 120, layout switch, turning smooth, follow smooth, edge off), so an untouched ini changes nothing
   there; Foveation and Dynamic Resolution are always passed on, because the tab's defaults (High, On) are not the code's own
   (medium, off).
2. A change made in the menu calls the option's hook in `vr_settings_changed`. The hooks are weak symbols, and VR builds provide them all, so every row but Refresh Rate applies at once. A flat build
   has no rig, input or HUD hooks: its rows say "Applies After Restart", and the saved value takes effect through step 1
   at the next start.
3. A variable already in the environment when the program started greys its row out ("Set by LARECOMP_VR_EDGE in the launch
   environment."). The Refresh Rate row follows `AF_REFRESH_HZ` in `session.env` the same way (the wrapper exports
   `AF_REFRESH_FROM=env|ini`).

The refresh rate is the one option the game cannot apply itself: SteamVR's per-app rate is set by the launch wrapper
(`steam-frame/awakeningframe.sh`) before the game starts. SteamVR on the Frame offers `XR_FB_display_refresh_rate`, but
`xrEnumerateDisplayRefreshRatesFB` lists only the rate it runs at, and `xrRequestDisplayRefreshRateFB` refuses even that
one (`XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB` in every headset log, at 90 and at 120 Hz), so a change of the
display's rate stays "Applies After Relaunch". The game's rate at the display's rate is the game's own: between the two
90 Hz choices (30 FPS and the 60 FPS test) the change is live when the display runs at 90 Hz (`xr_set_game_fps`,
xr_loop.c: the pacer's `pace_xr_lock` / `pace_xr_frame_ms` follow at the next refresh), unless `LARECOMP_XR_FPS` came
from the launch environment. The wrapper reads `vr_refresh` from the ini the game uses
(`LARECOMP_SETTINGS`, else `larecomp.ini` in the data root, `~/.local/share/lasrecomp` by default, else the program folder's)
when `AF_REFRESH_HZ` is not set, so the choice takes effect on the next launch; with no `vr_refresh` line the rate is 120 Hz.
At 120 Hz the game runs at 60 FPS, at 90 Hz at 30 FPS (`pace_xr_lock`).

The third choice, 90 Hz (60 FPS, Test), is for feeling 60 FPS on a display that is not a multiple of it. It saves
`vr_refresh = 90` and `vr_fps = 60`, and at startup sets `LARECOMP_XR_FPS=60` unless the environment already has it
(`AF_FPS=60` in `session.env` does the same without the menu: the wrapper exports `LARECOMP_XR_FPS`). The 30 FPS lock is
then left off and the pacer chooses 60 or 30 as at 120 Hz, with a 16.67 ms game frame (`pace_xr_frame_ms`, the VR GPU
limits of `PACE_VR_GPU`). The XR thread already gives the game its vsync on two of every three refreshes, so each game
frame is shown for one refresh or two, alternating: motion is less even than at 120 Hz. One pose per game frame, the
carried render poses and the table moving only on new game frames are unchanged.

## Options

| Row | Choices (the menu's order; default as stated, else the first) | `larecomp.ini` | Variable | Applied |
|---|---|---|---|---|
| Refresh Rate | 120 Hz (60 FPS), 90 Hz (30 FPS), 90 Hz (60 FPS, Test) | `vr_refresh = 120 \| 90`, `vr_fps = 30 \| 60` | wrapper: `AF_REFRESH_HZ`, `AF_FPS`; `LARECOMP_XR_HZ`, `LARECOMP_XR_FPS` | relaunch; between the 90 Hz choices live (`xr_set_game_fps`) |
| Controller Layout | Switch, Comfort | `vr_layout = switch \| comfort` | `LARECOMP_XR_LAYOUT` | live (`xr_input_set_layout`, xr_input.c; a control held through the change stays silent until let go) |
| Buttons Match Labels | Off, On | `face_buttons_by_label = 0 \| 1` (the Controls tab's setting) | none (`g_settings.face_by_label`, read live by `xr_input.c`) | live (a control held through the change stays silent until let go) |
| Pointer Hand | Right, Left | `vr_pointer_hand = right \| left` | `LARECOMP_VR_POINTER_HAND` | live (`xr_input.c` reads it each refresh) |
| Turning | Smooth, Snap | `vr_turn = smooth \| snap` | `LARECOMP_VR_TURN` | live (`xr_rig_set_turn_mode`, the rig's) |
| Follow Mode | Smooth, Room, Fixed | `vr_follow = smooth \| room \| fixed` | `LARECOMP_VR_FOLLOW` | live (`stereo_set_follow`, stereo.c: Smooth goes back to the rig's area anchoring, as at a start without the variable; Room and Fixed start from the table's current anchor, so it doesn't jump) |
| Height | 0, -50 to +40 cm in steps of 5 | none (the rig's, `vr_rig.ini`) | none | live (`xr_rig_set_height_cm`, `runtime/xr/xr_rig_settings.c`) |
| Distance | 120 cm, 60 to 300 cm in steps of 10 | none (the rig's, `vr_rig.ini`) | `LARECOMP_VR_DIST` (the default, not greyed) | live (`xr_rig_set_dist_cm`) |
| Size | 100%, 40 to 300% in steps of 10 | none (the rig's, `vr_rig.ini`) | none | live (`xr_rig_set_size_pct`) |
| Tilt | 30, 0 to +50 degrees in steps of 5 | none (the rig's, `vr_rig.ini`) | none | live (`xr_rig_set_tilt_deg`) |
| Recentre Table | button | none | none | live (`xr_rig_recentre`; greyed out in a flat build) |
| Table Edge | Off, Fade, Passthrough (default Off) | `vr_edge_style = black \| fade \| passthrough` | `LARECOMP_VR_EDGE` (`black`, `fade`, `passthrough`; the variable still takes `fog` and `gradient`) | live (`edge_set_mode`, edge.c: the next eye is drawn in the new mode; each XR set carries its mode, so the projection layer's alpha flag and the blend mode follow the images; Passthrough needs the runtime's `ALPHA_BLEND`, else the void shows black) |
| Foveation | Off, Low, Medium, High (eye-tracked only; default High) | `vr_foveation = off \| low \| medium \| high` | `LARECOMP_VR_FOVEATION` (always set from the tab: the code's own default is medium) | live (`fov_set_level`; the level is in the shading-rate map's key, so the next pass redraws it). Off at start still enables the extension, so it can be turned on; only `LARECOMP_VR_FOVEATION=off` from the launch environment leaves it out |
| Dynamic Resolution | Off, On (default On; floor 1152 px; 10 ms budget at 120 Hz) | `vr_dynamic_resolution = 1 \| 0` | `LARECOMP_VR_DYNRES` (always set from the tab: the code's own default is off) | live (`diet_set_dynres`) |
| Panel Distance | 2.0 m, 0.8 to 4.0 m | `vr_panel_distance = <metres>` | `LARECOMP_XR_QUAD=<dist>,<width>` | live (`vrui_set_hud_quad`) |
| Panel Width | 2.2 m, 0.6 to 4.0 m | `vr_panel_width = <metres>` | the same | live |

Height, distance, size and tilt belong to the rig (`docs/af/RIG.md`), which keeps them in `vr_rig.ini`; the tab neither saves them in
`larecomp.ini` nor sets `LARECOMP_VR_SCALE` from them. Each time the tab is drawn it reads the rig's live values
(`xr_rig_get_user`, weak), so a change made with the controller combos shows, and a change in the tab goes through the
rig's `xr_rig_set_user`, which clamps to the rig's ranges (height -0.50 to +0.40 m, distance 0.6 to 3 m, size 40 to 300%, tilt 0 to +50
degrees). The rig applies them on top of `LARECOMP_VR_TABLE` / `_PITCH` / `_SCALE`, so those variables do not grey the
rows. The hooks exist in VR builds only; in a flat build the rows keep their defaults and say "Applies After Restart."
`larecomp.ini` files from before may still hold `vr_table_height` / `_size` / `_tilt`: they are read and ignored (the
next save drops them).

## Defaults and the saved file

Defaults: Refresh Rate 120 Hz (60 FPS), Foveation High, Dynamic Resolution On (floor 1152 px; 10 ms budget at 120 Hz), Table Edge Off, Turning
Smooth. An `larecomp.ini` that already holds a value keeps it (the tab writes every row each time it saves, so a file
saved by an older build can still have `vr_refresh = 90`, `vr_foveation = medium` or `vr_dynamic_resolution = 0` until
they are changed in the tab). The edge key is `vr_edge_style`; the older `vr_edge_mode`, `vr_edge` and `vr_culling` are
read and dropped. Culling has no row: `LARECOMP_VR_CULL` works from the environment only (default `fit`).

All the hooks are weak symbols (`vr_opt_live`). Live (VR builds): every row but Refresh Rate, which says "Applies After
Relaunch" unless the change is between its two 90 Hz choices at 90 Hz. A flat build has no rig, input or HUD hooks: those
rows say "Applies After Restart". To add a row, provide its hook (declared at the top of `vr_settings.c`) and add the one
call in `vr_settings_changed`.

## Row text

The menu's rows, values and descriptions, with what set each option before the tab existed. The wording follows
Moonlight and Apollo: Title Case, no filler.

| Set before by | Row | Values | Description |
|---|---|---|---|
| `AF_REFRESH_HZ=90` / `120`, `AF_FPS=60` | Refresh Rate (default 120 Hz) | 120 Hz (60 FPS), 90 Hz (30 FPS), 90 Hz (60 FPS, Test) | The headset's refresh rate, and with it the game's frame rate: 120 Hz plays at 60 FPS. 90 Hz plays at 30 FPS and asks less of the GPU. Applies After Relaunch. For 90 Hz (60 FPS, Test): Plays at 60 FPS on a 90 Hz display. Motion is less even because frames alternate between one and two refreshes. For testing. |
| `LARECOMP_XR_LAYOUT=switch` / `comfort` | Controller Layout | Switch, Comfort | Switch puts every button where a Switch Pro Controller has it. Comfort puts the sword on the right trigger and the shield on the left. |
| `LARECOMP_VR_POINTER_HAND=right` / `left` | Pointer Hand | Right, Left | The controller that points at this menu. Pull its trigger to click, push the right stick to scroll. |
| Buttons match labels (Controls tab) | Buttons Match Labels | Off, On | The Frame controllers' A, B, X and Y work as printed on them. Off matches the Switch's button positions. |
| right stick left / right, `LARECOMP_VR_TURN=smooth` / `snap` | Turning | Smooth, Snap | Smooth turns the table as you push the right stick sideways. Snap turns it in 45° steps behind a blink. Click both sticks to reset the view. Hold them for 2 seconds to reset the table too. |
| `LARECOMP_VR_FOLLOW=smooth` / `room` / `fixed` | Follow Mode | Smooth, Room, Fixed | How the table follows the game's camera. Smooth glides after it, Room holds still until the camera changes room, Fixed never moves. |
| `LARECOMP_VR_TABLE` (y) | Height | -50 cm to +40 cm | How high the table stands. |
| `LARECOMP_VR_SCALE` | Size | 40% to 300% | How big the table is. |
| `LARECOMP_VR_DIST` | Distance (new) | 60 to 300 cm | How far away the table stands. |
| `LARECOMP_VR_PITCH` | Tilt | 0 to +50 degrees (default 30) | How far the table leans toward you. |
| right stick click with left grip held (now the view reset's alias) | Recentre Table | (button) | Puts the table in front of you again. |
| `LARECOMP_VR_EDGE=black` / `fade` / `passthrough` | Table Edge | Off, Fade, Passthrough | What fills the space past the world. Fade softens the world into darkness around Link, Passthrough shows your room. |
| `LARECOMP_VR_FOVEATION=off` / `low` / `medium` / `high` | Foveation (default High) | Off, Low, Medium, High | Follows your eyes and draws what you aren't looking at in less detail. Higher levels save more GPU time. Without eye tracking it draws everything in full detail. |
| `LARECOMP_VR_DYNRES` | Dynamic Resolution (default On) | Off, On | Lowers the resolution when the GPU can't keep up and raises it again when it can. |
| `LARECOMP_XR_QUAD` (distance) | Panel Distance | 0.8 m to 4.0 m | How far away the HUD and menus float. |
| `LARECOMP_XR_QUAD` (width) | Panel Width | 0.6 m to 4.0 m | How wide the HUD and menus are. |
| (new) | tab: VR; sections Display, Controls, Table, Edge, Performance, Menu Panel | | |

Notes added to a description by the tab itself: "Applies After Restart." (a flat build: the option has no live hook),
"Applies After Relaunch." (Refresh Rate), "Set by <VARIABLE> in the launch environment." (the row is greyed out).

## Controller navigation

The tab is an ordinary option-row page: the Frame controllers reach it through `xr_input.c`'s SDL-style events (the layout
maps them to Switch buttons), L / R (the bumpers) switch tabs, Up / Down move between rows, A or Right steps a value, Left
steps back, B goes back to the tab bar. Rows set by the environment and the Recentre Table row without its hook (a flat build) are greyed
out and skipped.

## Testing without a headset

`LARECOMP_VR_SET_AT=<frame>:<key>=<value>[,...]` makes a change at a game frame the way the tab does (the ini saved, the
hook called; `vr_settings_test_tick`, called from hle/warp.c): keys `edge`, `foveation`, `dynres`, `follow`, `layout`,
`turn`, `panel_distance`, `panel_width`, `refresh`, `fps`, `label`, values as in the ini (`edge=fade`, `dynres=0`).

```
LARECOMP_SHOW_VR_TAB=1 LARECOMP_SETTINGS=/tmp/vr.ini LARECOMP_MENU_TAB=10 ...   # the tab, flat; the ini is the scratch one
tools/test_settings.py                                                             # the ini round trip (vr_* keys)
```
