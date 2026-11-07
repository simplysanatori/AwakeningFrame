# AwakeningFrame user guide

The full detail for playing AwakeningFrame on the Steam Frame. The short version is in the [README](../README.md). Technical detail for developers is in [AGENTS.md](../AGENTS.md).

## What it does

- Puts the whole game on a table in front of you, as a miniature diorama. Link is about 10 cm tall.
- Draws both eyes in one pass, each with its own camera, so the island is in true stereo and follows your head.
- Shows hearts, text and menus on a floating panel.
- Holds the table still on each room in houses and dungeons, and follows Link smoothly in the overworld.
- Stands side-scrolling passages upright, like a wall.
- Turns and tilts the table with the right stick, and lets you set its height and size.
- Blinks to black on a room change.
- Runs at 60 frames a second on the 120 Hz display, with eye-tracked foveated rendering.

LAS Recomp runs in a window on Windows, macOS and Linux. AwakeningFrame is the same game built as a Linux Arm64 program with a VR mode for the Frame's own SteamVR, through OpenXR. There is no emulator and no Proton.

You need your own game files and keys. No game data is in this repository or its releases.

## Known issues

Version 0.1.0 is a pre-release.

- Fast head turns can show a double image.
- Fast-moving objects can look doubled. The game runs at 60 FPS on the 120 Hz display, so each frame shows twice.
- The Fade and Passthrough table edges can flicker.
- Surroundings (Low Quality, Backdrop and Full) is experimental and flickers.
- The busy overworld can drop to 30 FPS.

## Install details

**The game files.** You need the base game, version 1.0.0 (title ID 01006BB00C6F0000), as an NSP or XCI, and your `prod.keys`. About 14 GB must be free while the game is extracted, and about 8 GB after. Updates, DLC and compressed `.nsz` or `.xcz` files are refused. The installer looks in `~/Games/AwakeningFrame/game-files`, `~/Downloads`, `~/Documents`, `~/Desktop` and your home folder, and on SD cards and drives. The extracted `exefs` and `romfs` folders work too, instead of an NSP.

**Step 2, `install.sh`.** The installer copies the program to `~/Games/AwakeningFrame/app`. It adds **AwakeningFrame** to your Steam library with its art, or updates the entry if it is there. It sets SteamVR's per-app settings for it: 120 Hz, no throttling, no motion smoothing. The launch wrapper sets them again at every start. The script talks to Steam through its local debugging port, which SteamOS has on by default.

Then it finds your game, extracts it into `~/.local/share/lasrecomp/extracted` with your keys, and builds the game code from it. This takes up to 15 minutes. If the build isn't done by then, the first launch finishes it on the start screen, with a progress bar. The keys are only read where they are. They are not copied anywhere.

If you install before your game is on the Frame, put it in `~/Downloads` and launch **AwakeningFrame**. It looks in the same places.

**Updating.** Install the new version the same way. Your game files, saves, settings and the library entry stay.

### Install with Frametanium

If you use [Frametanium](https://github.com/Cogential/frametanium), add `simplysanatori/AwakeningFrame` there. It downloads the release, runs its `install.sh` and updates it when I publish a new version. To remove it, remove it in Frametanium. Your data folder stays. Version 0.1.0 is a pre-release, so Frametanium shows it as one.

### Install with FrameDrop

[FrameDrop](https://framedropvr.com) can install this too: [Install with FrameDrop](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2Fsimplysanatori%2FAwakeningFrame%2Freleases%2Fdownload%2Fv0.1.0%2Fawakeningframe.framedrop.json). The release has an `AwakeningFrame-<version>-framedrop-linux-arm64.zip` and an `awakeningframe.framedrop.json` for it. FrameDrop unpacks the zip on the Frame and adds its own library entry, which runs the `run.sh` in the zip. The first time it runs, and again after an update, `run.sh` runs `install.sh` without the Steam shortcut step, so you get FrameDrop's entry and no second one. Then it starts the game through its launch wrapper.

### Uninstall

`~/Games/AwakeningFrame/app/uninstall.sh` removes the Steam shortcut and the program folder. Your data folder (`~/.local/share/lasrecomp`: game files, saves and settings) and the logs stay unless you add `--purge`. Quit the game first. With Frametanium, remove it there instead.

## First run

The game starts straight away, without LAS Recomp's start screen. It prepares its shaders first, which takes longer the first time.

The table appears in front of you, a little below eye height, with Link about where you look when you face forward. If it is not where you want it, click both thumbsticks to reset the view, hold them for 2 seconds to put everything back to the defaults, or open the VR tab and change Height, Distance, Size and Tilt.

## Controls

The game sees a Switch Pro Controller. The Frame controllers map to it by position, so the game's own button prompts name the right control.

| Steam Frame | Switch | In the game |
|---|---|---|
| Left stick | Left stick | Walk. Up walks Link away from you, whichever way the table faces |
| **A** (bottom) | B | Sword |
| **B** (right) | A | Talk, read, lift, open |
| **X** (left) | Y | The item on Y |
| **Y** (top) | X | The item on X |
| Left bumper | L | Pegasus Boots |
| Right bumper or right grip | R | Shield (hold) |
| Left trigger, right trigger | ZL, ZR | Not used in play |
| Right **Menu** | Plus | Pause menu |
| Left **View** | Minus | Map |
| **View** and **Menu** together | | LAS Recomp's menu, with the VR tab |
| D-pad | D-pad | D-pad items |
| Right stick left / right | | Turn the table |
| Right stick up / down | | Tilt the table |
| Left grip + D-pad up / down | | Table height |
| Left grip + D-pad right / left | | Table size |
| Click both thumbsticks | | Reset the view. Hold for 2 seconds for a full reset |

Turning is smooth by default. With Turning set to Snap, a flick of the right stick turns the table 45 degrees behind a blink.

Resetting the view turns the table back to the game's own angle and puts it in front of you again. "View Reset" shows on the panel. Holding both thumbsticks for 2 seconds also puts the height, distance, size and tilt back to the defaults. "Full View Reset" shows on the panel. Left grip + a right stick click does the same.

In LAS Recomp's menu the right controller points with a green laser. Pull its trigger to click, and push the right stick to scroll. Pointer Hand on the VR tab moves the laser to the left controller.

Menu and View act a moment after you press them, so that pressing both opens LAS Recomp's menu without also opening the pause menu or the map.

With Buttons Match Labels on, A, B, X and Y work as printed on the controllers instead of by position. The Comfort layout puts the sword on the right trigger and the shield on the left trigger.

Rumble: when the game rumbles, both controllers vibrate.

More detail is in [CONTROLS.md](CONTROLS.md).

## Settings

Press **View** and **Menu** together to open LAS Recomp's menu. The VR tab is next to Controls. The bumpers switch tabs, up and down move between rows, and A or right changes a value. Every row applies at once, except Refresh Rate, which applies after a relaunch.

| Row | Choices | Default |
|---|---|---|
| Refresh Rate | 120 Hz (60 FPS), 90 Hz (30 FPS), 90 Hz (60 FPS, Test) | 120 Hz (60 FPS) |
| Controller Layout | Switch, Comfort | Switch |
| Turning | Smooth, Snap | Smooth |
| Buttons Match Labels | Off, On | Off |
| Pointer Hand | Right, Left | Right |
| Follow Mode | Smooth, Room, Fixed | Smooth |
| Height | -50 to +40 cm | 0 cm |
| Distance | 60 to 300 cm | 80 cm |
| Size | 40% to 300% | 100% |
| Tilt | 0 to +50 degrees | 30 degrees |
| Recentre Table | | |
| Table Edge | Off, Fade, Passthrough | Off |
| Surroundings | Off, Low Quality, Backdrop, Full | Off |
| Foveation | Off, Low, Medium, High | High |
| Dynamic Resolution | Off, On | On |
| Panel Distance | 0.8 to 4.0 m | 2.0 m |
| Panel Width | 0.6 to 4.0 m | 2.2 m |

- **Refresh Rate.** 120 Hz plays at 60 FPS, each frame shown for two refreshes. 90 Hz plays at 30 FPS and asks less of the GPU. The Test choice plays at 60 FPS on 90 Hz, with less even motion.
- **Follow Mode.** Smooth holds each room still and follows Link in the overworld. Room holds still until the camera changes room. Fixed never moves.
- **Table Edge.** Off leaves black past the world. Fade ends the world in a soft round edge, in every room. Passthrough shows your room past the edge of the overworld.
- **Surroundings.** Draws the overworld past the table. Low Quality and Backdrop are cheaper than Full.
- **Foveation.** Follows your eyes and draws what you aren't looking at in less detail. Without eye tracking, everything is drawn in full detail.
- **Dynamic Resolution.** Lowers the resolution when the GPU can't keep up and raises it again when it can.

The settings are saved in `larecomp.ini`, and the table's height, distance, size and tilt in `vr_rig.ini`, both in `~/.local/share/lasrecomp`.

## Performance

- At 120 Hz the game runs at 60 frames a second, and each frame is shown for two refreshes. Both eyes of a frame are rendered from one head pose, in one pass: the game's commands are recorded and submitted once for both eyes.
- Foveation uses the Frame's eye tracker: only where you look gets full detail.
- Dynamic Resolution lowers the eye resolution in the heaviest outdoor areas, and raises it again indoors. It keeps time free for SteamVR's compositor, which keeps head turns clean.
- In VR the game skips passes the headset doesn't need, such as the tilt-shift blur and the anti-aliasing on menus. The CPU and the GPU work on consecutive frames side by side.
- Shaders are prepared at start-up, so new areas don't stutter. Anything new the game meets is prepared at the next start.
- Keep builds and other heavy work off the Frame while you play.

How it works inside is in [TECHNICAL.md](TECHNICAL.md).

## Files

| What | Where |
|---|---|
| Program and launch wrapper | `~/Games/AwakeningFrame/app` |
| Game files, game code, saves, settings, shader caches | `~/.local/share/lasrecomp` (`extracted/`, `extracted/lasgame.so`, `larecomp.ini`, `vr_rig.ini`, `cache/`) |
| Logs | `~/Games/AwakeningFrame/logs`: `run-<time>.log` (the launch and the game's output), `lasrecomp-<time>.log` (the game's log) and timing logs, for the last 12 launches |
| One launch's settings | `~/Games/AwakeningFrame/session.env`, `NAME=value` lines read at start |
| hactool | `~/Games/AwakeningFrame/app/hactool` (a release), `~/Games/AwakeningFrame/tools-bin/hactool` (from source) |

## Troubleshooting

- **Frametanium says no game files were found.** Put your NSP or XCI and `prod.keys` in `~/Downloads` and launch **AwakeningFrame**. `run-<time>.log` lists where it looked and what it found.
- **`install.sh` says Steam's debugging port is not reachable.** Start Steam and run it again.
- **The start screen shows instead of the game.** It is either finishing the game code, with a progress bar, or it did not find your game files. In the second case, put them in `~/Downloads` and launch again.
- **Your game files are refused.** It needs the base game, version 1.0.0. Updates, DLC and compressed files are refused.
- **Motion is uneven, or the picture ghosts when you turn your head.** Check that SteamVR runs the game at the refresh rate you chose, with motion smoothing off. The launch wrapper sets these at every start, but only while SteamVR is running. `run-<time>.log` shows what it set.
- **The table is in an odd place.** Click both thumbsticks to reset the view, or hold them for 2 seconds to put the height, distance, size and tilt back to the defaults.
- **To check something without the headset,** run `~/Games/AwakeningFrame/app/awakeningframe.sh --flat`. It opens the game in an ordinary window.

## Reporting a problem

Open an [issue](https://github.com/simplysanatori/AwakeningFrame/issues) with what you did, what you expected and what happened. Attach the newest `run-<time>.log` and `lasrecomp-<time>.log` from `~/Games/AwakeningFrame/logs`.

These logs hold file paths under your home folder and the Steam shortcut's app id. Read them before you attach them. Never attach game files or keys.

If the problem also happens in LAS Recomp's own build, report it [upstream](https://github.com/peiikai/LASRecomp/issues) instead.
