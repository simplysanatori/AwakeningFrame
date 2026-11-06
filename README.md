# AwakeningFrame

The Legend of Zelda: Link's Awakening in VR, as a tabletop diorama, running natively on the Valve Steam Frame.

AwakeningFrame is a fork of peiikai's [LAS Recomp](https://github.com/peiikai/LASRecomp), the static recompilation of Link's Awakening (Nintendo Switch, 2019), which draws the Switch's NVN graphics with Vulkan. AwakeningFrame builds it as a Linux Arm64 program with a VR mode for the Frame's own SteamVR, through OpenXR. There is no emulator and no Proton.

You need your own copy of the game and the keys from your own Switch. No game data is in this repository.

What it does:

- Puts the whole game on a table in front of you, as a miniature diorama. Link is about 10 cm tall. The game's art style already looks like toys on a table.
- Renders each eye with its own camera, so the island is in true stereo, and follows your head.
- Shows hearts, text and menus on a floating panel.
- Holds the table still on each room in houses and dungeons, and follows Link smoothly in the overworld.
- Stands side-scrolling passages upright, like a wall.
- Turns and tilts the table with the right stick, and lets you set its height and size.
- Blinks to black on a room change, with no sound.
- Runs at 60 frames a second on the 120 Hz display, with eye-tracked foveated rendering.

## What this adds to LAS Recomp

LAS Recomp runs in a window on Windows, macOS and Linux. I added a VR mode and the Frame side:

- A Linux Arm64 build for the Frame, with an OpenXR session in SteamVR on the game's own Vulkan renderer.
- Stereo: each of the game's frames is drawn twice, once per eye, with its own camera. Shadow maps and other work that doesn't depend on the eye are drawn once and shared.
- The HUD and menus on a separate floating panel, with a transparent background.
- The diorama rig: where the table sits, how it follows Link, room by room or smoothly, side views stood upright, and a blink for every sudden change.
- The Frame controllers mapped to a Switch Pro Controller by position, with rumble on both controllers.
- Eye-tracked foveated rendering, dynamic eye resolution, and a VR mode that skips passes the headset doesn't need.
- Shader preparation at every start, not only from the start screen.
- A VR tab in the game's menu.
- An optional edge for the overworld: a soft fade, or passthrough to your room.
- Scripts to extract your game, build and install it, add it to Steam with SteamVR's per-app settings, and remove it again.

## Status

AwakeningFrame is an early preview.

Known issues:

- The left eye can briefly show a flat view during fast camera moves.
- The first launch takes longer while the shaders are prepared.

## Requirements

- A Steam Frame with SteamOS and SteamVR, and about 14 GB free while the game is extracted (about 8 GB after).
- Your own copy of Link's Awakening for the Switch: the base game, version 1.0.0 (title ID 01006BB00C6F0000), as an NSP or XCI.
- `prod.keys` from your own Switch.
- [Frametanium](https://github.com/Cogential/frametanium) or [FrameDrop](https://framedropvr.com) to install a release. To build from source instead: a terminal on the Frame (SSH, or Konsole from desktop mode).

The game files are never included. You extract them from your own copy.

## Install

### Install with Frametanium

1. Put your game on the Frame: your NSP or XCI of the base game and your `prod.keys`, both in `~/Downloads`. `~/Documents` and `~/Games/AwakeningFrame/game-files` work too, and so do the extracted `exefs` and `romfs` folders instead of an NSP.
2. In [Frametanium](https://github.com/Cogential/frametanium), add `simplysanatori/AwakeningFrame` and install it.
3. Put the headset on and launch **AwakeningFrame** from your Steam library.

The installer copies the program to `~/Games/AwakeningFrame/app` and adds **AwakeningFrame** to your library, with its art and SteamVR's per-app settings: 120 Hz, no throttling, no motion smoothing. It finds your game, extracts it into `~/.local/share/lasrecomp/extracted` with your keys, and builds the game code from it. This takes up to 15 minutes. If the build isn't done by then, the first launch finishes it on the start screen, with a progress bar.

The keys are only read where they are. They are not copied anywhere.

If you install before your game is on the Frame, put it in `~/Downloads` and launch **AwakeningFrame**. It looks in the same places.

To update, install the new version in Frametanium. Your game files, saves, settings and the library entry stay. To remove it, remove it in Frametanium. Your data folder stays.

### Install with FrameDrop

[Install with FrameDrop](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2Fsimplysanatori%2FAwakeningFrame%2Freleases%2Flatest%2Fdownload%2Fawakeningframe.framedrop.json), and put your game in `~/Downloads` as in step 1 above. FrameDrop adds its own library entry. The first launch installs the program, extracts your game and builds the game code on the start screen, with a progress bar.

### Build from source

You need a terminal on the Frame (SSH, or Konsole in desktop mode) and Steam running.

1. Get the repository:

   ```sh
   git clone -b steam-frame https://github.com/simplysanatori/AwakeningFrame.git ~/Games/AwakeningFrame/src
   cd ~/Games/AwakeningFrame/src
   ```

2. Extract your game, once. `build-hactool.sh` builds [hactool](https://github.com/SciresM/hactool) into `~/Games/AwakeningFrame/tools-bin`. `prepare-game.sh` checks the package and writes `exefs` and `romfs` to `~/.local/share/lasrecomp/extracted`:

   ```sh
   steam-frame/build-hactool.sh
   steam-frame/prepare-game.sh /path/to/LinksAwakening.nsp /path/to/prod.keys
   ```

   It refuses updates, DLC and compressed `.nsz` or `.xcz` files. A retail NSP carries its own ticket; for a dump without one, add your `title.keys` as a third argument. If you already have the extracted `exefs` and `romfs` folders, give their folder instead: `steam-frame/prepare-game.sh /path/to/folder`.

3. Set up the Python tools the game code generator uses:

   ```sh
   python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn
   ```

4. Build and install:

   ```sh
   ./install.sh
   ```

   The first time, it recompiles the game code from your files, which takes a few minutes, and then builds the program. It installs it to `~/Games/AwakeningFrame/app` and adds **AwakeningFrame** to your library, as above. The script talks to Steam through its local debugging port, which SteamOS has on by default.

To update, pull the repository and run `./install.sh` again. To remove it, run `./uninstall.sh`. `./uninstall.sh --purge` also removes the logs and the data folder, which holds your game files and saves.

A release never replaces a build from source. If `~/Games/AwakeningFrame/app` holds one, Frametanium puts the release in `~/Games/AwakeningFrame/release` and leaves the library entry alone. To switch the entry to the release, run `install.sh --replace-source` from `~/.local/share/frametanium/apps/awakeningframe`.

To build by hand, see [docs/af/BUILD-frame.md](docs/af/BUILD-frame.md): `steam-frame/fetch-vma.sh` and `tools/fetch_openxr.sh` fetch the headers SteamOS doesn't have, then `make gen` recompiles the game code from your files and `make` builds the program, both with `DEPS=$PWD/build/linux/aarch64/deps`. `steam-frame/build.sh` makes the release archives.

## First run

The game starts straight away, without LAS Recomp's start screen. It prepares its shaders first, which takes longer the first time.

The table appears in front of you, below eye height. If it is not where you want it, click both thumbsticks to reset the view, or open the VR tab and change Height, Size and Tilt.

If you see LAS Recomp's start screen instead of the game, it is either finishing the game code, with a progress bar, or it did not find your game. In the second case, put your NSP or XCI and `prod.keys` in `~/Downloads` and launch **AwakeningFrame** again.

## Controls

The game sees a Switch Pro Controller. The Frame controllers map to it by position, so the game's own button prompts name the right control.

| Steam Frame | Switch | In the game |
|---|---|---|
| Left stick | Left stick | Walk. Up walks Link away from you, whichever way the table faces |
| **A** (bottom) | B | Sword |
| **B** (right) | A | Talk, read, lift, open |
| **X** (left) | Y | The item on Y |
| **Y** (top) | X | The item on X |
| Right grip | A | Talk, read, lift, open |
| Left bumper | L | Pegasus Boots |
| Right bumper | R | Shield (hold) |
| Left trigger, right trigger | ZL, ZR | Not used in play |
| Right **Menu** | Plus | Pause menu |
| Left **View** | Minus | Map |
| **View** and **Menu** together | Minus and Plus | The game's menu, with the VR tab |
| D-pad | D-pad | D-pad items |
| Right stick left / right | | Turn the table |
| Right stick up / down | | Tilt the table |
| Left grip + D-pad up / down | | Table height |
| Left grip + D-pad right / left | | Table size |
| Click both thumbsticks | | Reset the view |

Turning is smooth by default. With Turning set to Snap, a flick of the right stick turns the table 45 degrees behind a blink.

Resetting the view turns the table back to the game's own angle and puts it in front of you again. Height, size and tilt stay as you set them. "View Reset" shows on the panel for a moment.

With Buttons Match Labels on, A, B, X and Y work as printed on the Frame controllers instead of by position. The Comfort layout puts the sword on the right trigger and the shield on the left trigger.

More detail is in [docs/CONTROLS.md](docs/CONTROLS.md).

## Settings

Press **View** and **Menu** together to open the game's menu. The VR tab is next to Controls. The bumpers switch tabs, up and down move between rows, and A or right changes a value.

| Row | Choices | What it does |
|---|---|---|
| Refresh Rate | 120 Hz (60 FPS), 90 Hz (30 FPS) | The headset's refresh rate, and with it the game's frame rate. 90 Hz asks less of the GPU. Applies after relaunch |
| Controller Layout | Switch, Comfort | Switch puts every button where a Switch Pro Controller has it. Comfort puts the sword on the right trigger and the shield on the left |
| Turning | Smooth, Snap | How the right stick turns the table |
| Buttons Match Labels | Off, On | On: A, B, X and Y work as printed on the controllers |
| Follow Mode | Smooth, Room, Fixed | How the table follows the game. Smooth holds each room still and follows Link in the overworld. Room holds still until the camera changes room. Fixed never moves |
| Height | -50 to +40 cm | How high the table stands |
| Size | 40% to 300% | How big the table is |
| Tilt | -20 to +50 degrees | How far the table leans toward you |
| Recentre Table | | Puts the table in front of you again |
| Table Edge | Off, Fade, Passthrough | What fills the space past the world in the overworld. Fade softens the edge of the world, Passthrough shows your room |
| Foveation | Off, Low, Medium, High | Follows your eyes and draws what you aren't looking at in less detail. Higher levels save more GPU time |
| Dynamic Resolution | Off, On | Lowers the resolution when the GPU can't keep up and raises it again when it can |
| Panel Distance | 0.8 to 4.0 m | How far away the HUD and menus float |
| Panel Width | 0.6 to 4.0 m | How wide the HUD and menus are |

The first choice is the default. Height, Size and Tilt start at 0, 100% and 0. Panel Distance starts at 2.0 m and Panel Width at 2.2 m. Foveation is High and Dynamic Resolution is On.

Every row but Refresh Rate applies at once. The settings are saved in `larecomp.ini`, and the table's height, size and tilt in `vr_rig.ini`, both in `~/.local/share/lasrecomp`.

## Performance

- At 120 Hz the game runs at 60 frames a second, and each frame is shown for two refreshes. Both eyes of a frame are rendered from one head pose.
- Foveation uses the Frame's eye tracker: only where you look gets full detail. Without eye tracking, everything is drawn in full detail.
- Dynamic Resolution lowers the eye resolution in the heaviest outdoor areas to hold 60, and raises it again indoors.
- In VR the game skips passes the headset doesn't need, such as the tilt-shift blur and the anti-aliasing on menus. The CPU and the GPU work on consecutive frames side by side.
- Shaders are prepared at start-up, so new areas don't stutter. Anything new the game meets is recorded and prepared at the next start.
- Keep builds and other heavy work off the Frame while you play.

The technical side is in [docs/TECHNICAL.md](docs/TECHNICAL.md).

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
- **`install.sh` says there is no game code and no game files.** Run `steam-frame/prepare-game.sh` first (Build from source, step 2).
- **`install.sh` says `tools/venv` is missing.** Run the command in Build from source, step 3.
- **`prepare-game.sh` refuses the package.** It needs the base game. Updates, DLC and compressed files are refused.
- **The start screen shows instead of the game.** The game files or the game code are missing. See [First run](#first-run).
- **Motion is uneven, or the picture ghosts when you turn your head.** Check that SteamVR runs the game at the refresh rate you chose, with motion smoothing off. The launch wrapper sets these at every start, but only while SteamVR is running. `run-<time>.log` shows what it set.
- **The table is in an odd place.** Click both thumbsticks to reset the view.
- **To check something without the headset**, run `~/Games/AwakeningFrame/app/awakeningframe.sh --flat`. It opens the game in an ordinary window.

## Reporting a problem

Open an [issue](https://github.com/simplysanatori/AwakeningFrame/issues) with what you did, what you expected and what happened, and attach the newest `run-<time>.log` and `lasrecomp-<time>.log` from `~/Games/AwakeningFrame/logs`.

These logs hold file paths under your home folder and the Steam shortcut's app id. Read them before you attach them.

If the problem also happens in LAS Recomp's own build, report it [upstream](https://github.com/peiikai/LASRecomp/issues) instead. Never attach game files or keys.

## Support

AwakeningFrame exists because of peiikai's [LAS Recomp](https://github.com/peiikai/LASRecomp). If you want to support someone, support them first: star and follow it, report what you find there, and send improvements upstream.

If you would like more Steam Frame ports, you can also [buy me a coffee](https://ko-fi.com/sana748653).

## Credits

- Built by [simplysanatori](https://github.com/simplysanatori).
- [LAS Recomp](https://github.com/peiikai/LASRecomp) by peiikai and its contributors: the recompilation, the Vulkan renderer, the start screen, the randomizer and everything else the game runs on.
- [LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus) by Nycolazs, for its Turnip and Adreno fixes.
- [GalaxyFrame](https://github.com/simplysanatori/GalaxyFrame), my Super Mario Galaxy VR port for the Frame, for the OpenXR approach, the frame pacing and the foveation.
- [LAS-Randomizer](https://github.com/Owen-Splat/LAS-Randomizer) by Owen_Splat, Glan and ProfessorLaw, which LAS Recomp's randomizer is built on.
- [hactool](https://github.com/SciresM/hactool) by SciresM, which extracts the game.
- The [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) from Khronos, [Dear ImGui](https://github.com/ocornut/imgui), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [SMAA](https://github.com/iryoku/smaa), AMD FidelityFX Super Resolution 1, [SDL 3](https://libsdl.org), [FFmpeg](https://ffmpeg.org), [Kenney](https://kenney.nl)'s input prompts and the Baloo 2, BIZ UDPGothic and DejaVu Sans fonts.
- Nintendo, for the game. Own your copy.

Upstream's full credits, with every contributor, are in its README, kept here as [README.upstream.md](README.upstream.md). [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) has the full list with licences.

The Legend of Zelda, Link's Awakening and their logos are trademarks of Nintendo. This is an unofficial fan project. It is not affiliated with or endorsed by Nintendo, Valve or the LAS Recomp authors.

## Licence

MIT, same as upstream. See LICENSE.

## How this was made

I made this with partial assistance from Claude.
