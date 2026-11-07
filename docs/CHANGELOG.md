# Changelog

## 0.1.0 (November 2026)

First pre-release. Link's Awakening as a VR tabletop diorama, running natively on the Steam Frame. Based on peiikai's LAS Recomp.

### Features

- Native Linux Arm64 build in SteamVR. No emulator, Proton or PC.
- The game code is built on the Frame from your own game files.
- The island sits on a table in front of you. Link is about 10 cm tall.
- The table holds still in rooms and follows Link outdoors.
- Set **Height**, **Distance**, **Size** and **Tilt** on the **VR** tab or with the controllers. Hold both stick clicks for 2 seconds to reset everything.
- The Frame controllers work as a Switch Pro Controller, by button position, with rumble.
- Hearts, text and menus show on a floating panel.
- Install with Frametanium, FrameDrop or `install.sh`. It adds **AwakeningFrame** to your library with art.

### Performance

- 120 Hz with the game at 60 FPS. 90 Hz at 30 FPS is the lighter choice.
- Houses and dungeons hold 60 FPS.
- The busy overworld can drop to 30 FPS.
- Dynamic resolution and eye-tracked foveation (High) are on by default.
- The first install builds the game code in about 10 to 15 minutes. The first start also prepares shaders.
- It uses about 3.5 GB of memory.

### Known issues

- Fast head turns can show a double image.
- Fast-moving objects can look doubled, because the game runs at 60 FPS on a 120 Hz display.
- The **Fade** and **Passthrough** table edges can flicker.
- **Surroundings** is experimental and flickers. **Full** runs at about 30 FPS outdoors, and **Low Quality** costs as much as **Full**.
- Some spots show a black cut where the next area isn't drawn, such as the woods north of Mabe Village.
