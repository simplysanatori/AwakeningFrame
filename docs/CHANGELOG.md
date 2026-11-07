# Changelog

## 0.1.0 (November 2026)

The first pre-release of AwakeningFrame, The Legend of Zelda: Link's Awakening in VR as a tabletop diorama, running natively on the Valve Steam Frame. It is a fork of peiikai's LAS Recomp.

### The game on the Frame

- A Linux Arm64 build that runs in the Frame's SteamVR. No emulator, no Proton, no PC.
- The game code is built on the Frame from your own game files, during the install or on the first launch.
- Saves and settings in plain folders under your home.

### The diorama

- The island on a table in front of you. Link is about 10 cm tall.
- The table holds still in each room of houses and dungeons, and follows Link smoothly outdoors.
- Side-scrolling passages stand upright, like a wall.
- A blink to black on sudden changes, such as a room change.
- Height, Distance, Size and Tilt on the VR tab, or from the controllers.
- Both stick clicks reset the view. Holding them for 2 seconds resets the table too.
- A table edge, off by default: a soft round fade, or passthrough to your room past the edge of the overworld.
- Surroundings, experimental and off by default: the overworld past the table, in Low Quality, Backdrop or Full.

### Rendering

- Both eyes drawn in one pass with Vulkan multiview, each with its own camera. Shadow maps and other work that doesn't depend on the eye are drawn once.
- 120 Hz, with the game at its own 60 FPS. 90 Hz at 30 FPS asks less of the GPU.
- Eye-tracked foveated rendering, set to High.
- Dynamic resolution, which keeps time free for SteamVR's compositor.
- Passes the headset doesn't need, like the tilt-shift blur, are skipped.
- Shaders are prepared at every start, so new areas don't stutter.

### Controls

- The Frame controllers as a Switch Pro Controller, by position, with rumble on both.
- The right grip is R, like the right bumper. The left grip with the D-pad sets the table's height and size.
- The right stick turns the table smoothly and tilts it. Snap turns in 45 degree steps are the other choice.
- A Comfort layout puts the sword on the right trigger and the shield on the left.
- Buttons Match Labels makes A, B, X and Y work as printed on the controllers.
- View and Menu together open LAS Recomp's menu. A laser pointer clicks in it.

### The panel and the VR tab

- Hearts, text and menus on a floating panel with a clear background.
- A VR tab in LAS Recomp's menu. Every row applies while you play, except Refresh Rate.

### Install

- Install with Frametanium, FrameDrop or the tarball's `install.sh`.
- The installer adds **AwakeningFrame** to your library with its art. It finds your game files in `~/Downloads`, extracts them and builds the game code.
- SteamVR's settings for the game (120 Hz, no motion smoothing) are set again at every launch.
- Your keys are read where they are and never copied.

### Known issues

- Fast head turns can show a double image.
- Fast-moving objects can look doubled. The game runs at 60 FPS on the 120 Hz display.
- The Fade and Passthrough table edges can flicker.
- Surroundings is experimental and flickers.
- The busy overworld can drop to 30 FPS.
