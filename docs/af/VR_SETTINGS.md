# VR tab (AwakeningFrame "vrui" lane)

The settings menu has a VR tab (second, after Controls; shown in VR mode, or with `LARECOMP_SHOW_VR_TAB=1` in a flat run).
Its options are the ones that were environment variables. They are kept in `larecomp.ini`; each one maps onto the variable
the VR code already reads, and an environment variable still wins. Code: `runtime/vr_settings.c/.h` (ini keys, the
mapping, the hooks), `runtime/ui/vr_tab.mm` (the page), `runtime/xr/vrui_hook.c` (the HUD panel's hook).

## How a saved value reaches the VR code

1. At startup (`settings_load`, before anything reads the environment) `vr_settings_apply_env` sets each variable from
   the saved value, but only for a value that is not the default and only when the variable is not already set. The
   default is whatever the code behind the variable does without it, so an untouched ini changes nothing.
2. A change made in the menu calls the option's hook in `vr_settings_changed`. The hooks are weak symbols: where a lane
   has not provided one yet the row's description says "Applies After Restart" (a relaunch, for the refresh rate), and the
   saved value takes effect through step 1 at the next start.
3. A variable already in the environment when the program started greys its row out ("Set by LARECOMP_VR_EDGE in the launch
   environment."). The Refresh Rate row follows `AF_REFRESH_HZ` in `session.env` the same way (the wrapper exports
   `AF_REFRESH_FROM=env|ini`).

The refresh rate is the one option the game cannot apply itself: SteamVR's per-app rate is set by the launch wrapper
(`steam-frame/awakeningframe.sh`) before the game starts. The wrapper now reads `vr_refresh` from `larecomp.ini` when
`AF_REFRESH_HZ` is not set, so the choice takes effect on the next launch. At 90 Hz the game runs at 30 FPS
(`pace_xr_lock`), at 120 Hz at 60 FPS.

## Options

| Row | Choices (default first) | `larecomp.ini` | Variable | Applied |
|---|---|---|---|---|
| Refresh Rate | 120 Hz (60 FPS), 90 Hz (30 FPS) | `vr_refresh = 120 \| 90` | wrapper: `AF_REFRESH_HZ`; `LARECOMP_XR_HZ` | relaunch |
| Controller Layout | Switch, Comfort | `vr_layout = switch \| comfort` | `LARECOMP_XR_LAYOUT` | hook `xr_input_set_layout` (stub) |
| Buttons Match Labels | Off, On | `face_buttons_by_label = 0 \| 1` (the Controls tab's setting) | none (`g_settings.face_by_label`, read live by `xr_input.c`) | live |
| Follow Mode | Smooth, Room, Fixed | `vr_follow = smooth \| room \| fixed` | `LARECOMP_VR_FOLLOW` | hook `stereo_set_follow` (stub) |
| Height | 0, -50 to +40 cm in steps of 5 | none (the rig's, `vr_rig.ini`) | none | live (`xr_rig_set_height_cm`, `runtime/xr/xr_rig_settings.c`) |
| Size | 100%, 40 to 300% in steps of 10 | none (the rig's, `vr_rig.ini`) | none | live (`xr_rig_set_size_pct`) |
| Tilt | 0, -20 to +50 degrees in steps of 5 | none (the rig's, `vr_rig.ini`) | none | live (`xr_rig_set_tilt_deg`) |
| Recentre Table | button | none | none | live (`xr_rig_recentre`; greyed out in a flat build) |
| Table Edge | Fog, Gradient, Black, Passthrough | `vr_edge = fog \| gradient \| black \| passthrough` | `LARECOMP_VR_EDGE` | hook `edge_set_mode` (stub), else restart |
| Foveation | Off, Low, Medium, High (eye-tracked only) | `vr_foveation = off \| low \| medium \| high` | `LARECOMP_VR_FOVEATION` (default medium) | live (`fov_set_level`); Off at start: restart to turn it on |
| Dynamic Resolution | Off, On | `vr_dynamic_resolution = 1 \| 0` | `LARECOMP_VR_DYNRES=0` | hook `diet_set_dynres` (stub) |
| Culling | Fit, Wide, Stock | `vr_culling = fit \| wide \| stock` | `LARECOMP_VR_CULL` | hook `vr_cull_set_mode` (stub) |
| Panel Distance | 2.0 m, 0.8 to 4.0 m | `vr_panel_distance = <metres>` | `LARECOMP_XR_QUAD=<dist>,<width>` | live (`vrui_set_hud_quad`) |
| Panel Width | 2.2 m, 0.6 to 4.0 m | `vr_panel_width = <metres>` | the same | live |

Height, size and tilt belong to the rig (`docs/af/RIG.md`), which keeps them in `vr_rig.ini`; the tab neither saves them in
`larecomp.ini` nor sets `LARECOMP_VR_SCALE` from them. Each time the tab is drawn it reads the rig's live values
(`xr_rig_get_user`, weak), so a change made with the controller combos shows, and a change in the tab goes through the
rig's `xr_rig_set_user`, which clamps to the rig's ranges (height -0.50 to +0.40 m, size 40 to 300%, tilt -20 to +50
degrees). The rig applies them on top of `LARECOMP_VR_TABLE` / `_PITCH` / `_SCALE`, so those variables do not grey the
rows. The hooks exist in VR builds only; in a flat build the rows keep their defaults and say "Applies After Restart."
`larecomp.ini` files from before may still hold `vr_table_height` / `_size` / `_tilt`: they are read and ignored (the
next save drops them).

## State of the lanes when this was written (all hooks are stubs, except the rig's)

None of the rig, fov and diet lanes had committed anything (no `xr_rig.h`, no `LARECOMP_VR_FOVEATION` or
`LARECOMP_VR_DYNRES` reader), edge had `edge.c` and `EDGE.md` (`LARECOMP_VR_EDGE=fog|gradient|black|passthrough`, default
fog; also `black` = `off`, `0`, `none`), and `vr_cull.c` / `stereo.c` / `xr_input.c` read their variables once at start.
So only Buttons Match Labels and the panel are live; the rest are saved, applied through the environment at the next
start, and flagged in the menu. To connect a lane: provide the hook's name (declared at the top of `vr_settings.c`, each
with a comment saying what it takes), or change the one call in `vr_settings_changed` to the lane's own function. The row
then loses its "Applies After Restart" note by itself (`vr_opt_live` tests the weak symbol).

Guesses to confirm when the lanes land: Foveation's default (the tab shows "Default" and leaves the variable unset, so the
lane's own choice stands), Dynamic Resolution's default (taken as On), and whether `LARECOMP_VR_FOVEATION` / `_DYNRES` take
the words above.

## Labels for review (original -> proposed)

The rows show in capitals; the case below is what the source and translation tables carry. "Original" is what each option
was: the variable, or the existing menu text. Style as Moonlight / Apollo: Title Case, no filler.

| Original | Proposed row | Proposed values | Description |
|---|---|---|---|
| `AF_REFRESH_HZ=90` / `120` | Refresh Rate | 120 Hz (60 FPS), 90 Hz (30 FPS) | The headset's refresh rate, and with it the game's frame rate: 90 Hz plays at 30 FPS and asks less of the GPU. Applies After Relaunch. |
| `LARECOMP_XR_LAYOUT=switch` / `comfort` | Controller Layout | Switch, Comfort | Switch puts every button where a Switch Pro Controller has it. Comfort puts the sword on the right trigger and the shield on the left. |
| Buttons match labels (Controls tab) | Buttons Match Labels | Off, On | The Frame controllers' A, B, X and Y work as printed on them. Off matches the Switch's button positions. |
| `LARECOMP_VR_FOLLOW=smooth` / `room` / `fixed` | Follow Mode | Smooth, Room, Fixed | How the table follows the game's camera. Smooth glides after it, Room holds still until the camera changes room, Fixed never moves. |
| `LARECOMP_VR_TABLE` (y) | Height | -50 cm to +40 cm | How high the table stands. |
| `LARECOMP_VR_SCALE` | Size | 40% to 300% | How big the table is. |
| `LARECOMP_VR_PITCH` | Tilt | -20 to +50 degrees | How far the table leans toward you. |
| right stick click with left grip held | Recentre Table | (button) | Puts the table in front of you again. |
| `LARECOMP_VR_EDGE=fog` / `gradient` / `black` / `passthrough` | Table Edge | Fog, Gradient, Black, Passthrough | What surrounds the table. Fog fades the world into the distance, Gradient fills the empty space with a soft backdrop, Black leaves it empty, Passthrough shows the room. |
| `LARECOMP_VR_FOVEATION=off` / `low` / `medium` / `high` | Foveation | Off, Low, Medium, High | Follows your eyes and draws what you aren't looking at in less detail. Higher levels save more GPU time. Without eye tracking it draws everything in full detail. |
| `LARECOMP_VR_DYNRES` | Dynamic Resolution | Off, On | Lowers the resolution when the GPU can't keep up and raises it again when it can. |
| `LARECOMP_VR_CULL=fit` / `wide` / `stock` | Culling | Fit, Wide, Stock | How much of the world outside the table is skipped. Fit follows the table's edges, Wide keeps more, Stock keeps everything the game would draw. |
| `LARECOMP_XR_QUAD` (distance) | Panel Distance | 0.8 m to 4.0 m | How far away the HUD and menus float. |
| `LARECOMP_XR_QUAD` (width) | Panel Width | 0.6 m to 4.0 m | How wide the HUD and menus are. |
| (new) | tab: VR; sections Display, Controls, Table, Edge, Performance, Menu Panel | | |

Notes added to a description by the tab itself: "Applies After Restart." (the option's lane has no live hook),
"Applies After Relaunch." (Refresh Rate), "Set by <VARIABLE> in the launch environment." (the row is greyed out).

## Controller navigation

The tab is an ordinary option-row page: the Frame controllers reach it through `xr_input.c`'s SDL-style events (the layout
maps them to Switch buttons), L / R (the bumpers) switch tabs, Up / Down move between rows, A or Right steps a value, Left
steps back, B goes back to the tab bar. Rows set by the environment and the Recentre Table row without its hook (a flat build) are greyed
out and skipped.

## Testing without a headset

```
LARECOMP_SHOW_VR_TAB=1 LARECOMP_SETTINGS=/tmp/vr.ini LARECOMP_MENU_TAB=10 ...   # the tab, flat; the ini is the scratch one
tools/test_settings.py                                                             # the ini round trip (vr_* keys)
```
