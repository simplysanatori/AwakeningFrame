# Notes for agents

- **What this is.** LAS Recomp statically recompiles the Switch release of *Link's Awakening* into a native app.
  - `tools/gen.py` turns the game's executables into C, which is built on the player's machine from their own
    extracted game files.
  - A host stands in for the Switch OS and services (`runtime/kernel.c`, `loader.c`, `services/`).
  - The game's NVN graphics are drawn with Metal on macOS and Vulkan on Windows and Linux (`runtime/nvn/`).
  - The randomizer and most features live in `runtime/hle/`, `runtime/ui/` and `tools/rando/`.
  - [README.md](README.md) is the player-facing page. [CONTRIBUTING.md](CONTRIBUTING.md) has the layout, building,
    tests and pull request rules.

- **Required reading.** Before you change anything, read all of [CONTRIBUTING.md](CONTRIBUTING.md) and
  [docs/architecture.md](docs/architecture.md). Then read the doc for the area you are working in from the table
  below ([debug-hooks](docs/debug-hooks.md), [vulkan-gpu-defaults](docs/vulkan-gpu-defaults.md),
  [save-format](docs/save-format.md), the build guides). Don't skim them or work from guesses about how the project
  is set up. If what you find in the code disagrees with a doc, say so in your pull request.

- **Where to start:**

  | Task | Read first |
  |---|---|
  | How the parts fit together | [docs/architecture.md](docs/architecture.md) |
  | Building | README "Building from source", [docs/BUILD-windows.md](docs/BUILD-windows.md), [docs/linux.md](docs/linux.md) |
  | A crash or hang | the log (`lasrecomp.log` in the data folder): the crash line names the guest address (`main+0x...`). Map it with `tools/sym.py` and read the code there before changing anything. Debug switches: [docs/debug-hooks.md](docs/debug-hooks.md) |
  | Rendering | `runtime/nvn/cmdbuf.c` (recording), `renderer.m` / `metal.m` (Metal), `vulkan/exec.c` (Vulkan), `maxwell/translate.c` (shaders); per-GPU Vulkan defaults: [docs/vulkan-gpu-defaults.md](docs/vulkan-gpu-defaults.md) |
  | Game behaviour or a new feature | `runtime/hle/` and the hooks in `tools/patches.py` and `runtime/hle/hooks.c` |
  | Saves | [docs/save-format.md](docs/save-format.md), `runtime/services/fs.c` |
  | Game addresses | CONTRIBUTING "Project layout" (`runtime/game_addresses/`, `GA(NAME)`, `tools/version_map.py`) |
  | Randomizer logic | `tools/rando/` and `third_party/LAS-Randomizer` (local changes marked "LAS Recomp") |
  | Menu text | `python3 tools/menu_strings.py check` |

- **No Nintendo files, ever.**
  - Never commit, attach or paste game files, extracted data, generated code (`generated/`), fonts, assets or seed
    folders (`rando/`).
  - Game files stay outside the repository.
  - Refer to them only as the player's own extracted game files. Don't write about how they are obtained, in code,
    comments, docs, commits or pull requests.

- **Branch.** `main` holds the last release and is not where work happens. Base your work on the highest-numbered
  `beta_<n>` branch and open the pull request against it. Check that branch first: the change you have in mind may
  already be there.

- **Fix the cause, not the symptom.**
  - Before changing code, find why it happens: the faulting instruction, the value that went wrong and who wrote
    it, and the NVN / Metal / Vulkan rule the host gets wrong. Fix that place.
  - A stub, a skipped call, a clamp, a retry or a longer timeout is a workaround. Use one only when the cause is
    known and can't be fixed where it lies (usually the game's own code). Say so in a comment.
  - Patch the game's own code minimally rather than replacing whole systems.

- **Addresses and patches.**
  - Every game address goes through the version tables and `GA(NAME)`. The game ships as v1.0.0, v1.0.1 and
    v1.1.0, and unknown build IDs are refused.
  - Every patch in `tools/patches.py` asserts the original instruction word, so a wrong address fails loudly.
    Keep it that way.
  - Run `python tools/test_game_addresses.py` after touching addresses.

- **Both graphics backends.**
  - A rendering fix on Metal usually needs the Vulkan counterpart, and the other way round. Say in the pull
    request which backend you tested.
  - Workarounds for one GPU or driver go in the per-GPU defaults in `runtime/nvn/vulkan/exec.c`
    (`vk_exec_init`), keyed by vendor ID. Each one has a `LARECOMP_VK_*` switch to turn it off, a comment saying
    which driver it was tested on, and a row in [docs/vulkan-gpu-defaults.md](docs/vulkan-gpu-defaults.md).
  - macOS is Metal only. Don't add a MoltenVK path.
  - Minimums: macOS 15 on Apple Silicon; Windows and Linux with Vulkan 1.3; x86 CPUs with AVX2 and FMA.

- **Testing.**
  - A fix is verified only when you've seen it work in game. A clean build or a quiet log is not enough.
  - `tools/shot.sh <frame> [seconds]` runs the game with scripted input (`LARECOMP_INPUT`), warps
    (`LARECOMP_WARP_AT`) and seeds (`LARECOMP_RANDO_SEED`), and saves a screenshot. Many more debug hooks are
    documented where they are defined (grep `getenv("LARECOMP_`).
  - For test runs, use a scratch save folder (`LARECOMP_SAVE_DIR`, `tools/fresh_save.sh`) so the user's saves
    and settings stay untouched. Mute them with `LARECOMP_NO_AUDIO=1` and `LARECOMP_NO_MOVIE_AUDIO=1` unless the
    test is about sound.
  - Never close or restart a copy of the game the user is playing. Run one test window at a time (`shot.sh` takes
    a lock).
  - Randomizer changes: generate several seeds with different settings and check each one completes. For dungeon
    room shuffle, run `tools/doorjump_test.sh <seed>`.
  - The unit tests are listed in CONTRIBUTING "Tests".

- **Randomizer rules.**
  - Older seed IDs must keep loading (see `runtime/ui/rando_tab.mm`).
  - State that belongs to a seed resets on an in-place seed switch through `RT_ON_RESET` (`runtime/hle.h`).
  - Item pickup text goes through the game's own message box, never a custom pop-up.

- **Menus and text.**
  - Settings get short labels and vanilla defaults: no presets, no paragraphs.
  - New text is written in English only. Don't edit the translation tables (`runtime/ui/lang/`): we fill them for
    every language before each release, and untranslated text shows in English until then. A fix to an existing
    translation from a native speaker is welcome as its own pull request.
  - Player-facing text (menus, README, release notes, issue replies) is plain and short.

- **Code.**
  - Match the surrounding code: naming, structure, comment density.
  - Comments explain why (what the game does, which bug or seed showed it), not what the next line does.
  - Code copied from other projects must be MIT or CC0, and credited. GPL or unlicensed code can be read for
    ideas but not copied.

- **Pull requests.**
  - One change per pull request.
  - Say what changed, why, and how it was tested (platform, GPU, seeds, screenshots).
  - No personal paths, names or other personal information in code, comments, commits or pull requests.

## AwakeningFrame (Steam Frame VR port)

This section holds the technical detail moved out of the README, for developers and coding agents. It covers the
`steam-frame` branch, which adds a VR mode for the Valve Steam Frame. LAS Recomp's notes above still apply.

Further reading:

- [docs/TECHNICAL.md](docs/TECHNICAL.md): how the Steam Frame port works.
- [docs/af/](docs/af/): the design notes for each part (stereo, rig, edge, surroundings, passes, settings, tooling).
- [docs/af/BUILD-frame.md](docs/af/BUILD-frame.md) and [docs/af/INSTALL.md](docs/af/INSTALL.md): building and installing on the Frame.
- [docs/CONTROLS.md](docs/CONTROLS.md): every control and layout.
- [docs/USER_GUIDE.md](docs/USER_GUIDE.md): the user-facing detail (install details, full controls, settings, files, troubleshooting).
- [README.upstream.md](README.upstream.md): LAS Recomp's README. Leave it untouched.

### What this adds to LAS Recomp

LAS Recomp runs in a window on Windows, macOS and Linux. AwakeningFrame adds a VR mode and the Frame side, on the
`steam-frame` branch:

- A Linux Arm64 build for the Frame (Adreno 750, Mesa Turnip), with an OpenXR session in SteamVR on the game's own
  Vulkan renderer.
- Stereo in one pass: the game's draws go to both eyes at once with Vulkan multiview, each eye with its own camera.
  Shadow maps and other work that doesn't depend on the eye are drawn once and shared.
- The HUD and menus on a separate floating panel with a transparent background, and a controller laser pointer for
  LAS Recomp's menu.
- The diorama rig: where the table sits, how it follows Link (room by room or smoothly), side views stood upright, and
  a blink for every sudden change.
- The Frame controllers mapped to a Switch Pro Controller by position, with rumble on both controllers.
- Eye-tracked foveated rendering (VRS), dynamic eye resolution, and a VR mode that skips passes the headset doesn't need.
- Shader preparation at every start, not only from the start screen.
- A VR tab in LAS Recomp's menu whose rows apply while the game runs.
- An optional table edge (fade or passthrough) and experimental Surroundings.
- Scripts to extract the player's game, build and install it, add it to Steam with SteamVR's per-app settings, and
  remove it again; a release tarball that Frametanium and FrameDrop install.

### Status

Version 0.1.1 is a pre-release. The player-facing known issues are in
[docs/USER_GUIDE.md](docs/USER_GUIDE.md#known-issues). The open problems behind them:

- **Doubling.** Fast head turns and fast-moving objects can look doubled. Part of it is the cadence: 60 FPS on a
  120 Hz display shows each frame twice, so a tracked object judders. The rest is SteamVR re-showing a frame whose
  `xrEndFrame` missed vsync behind the game's GPU work ([STEREO](docs/af/STEREO.md) "Frame end"). Headroom
  (`LARECOMP_VR_HEADROOM`) cut it a lot; `AF_THROTTLE=1` tests whether SteamVR making the second refresh itself
  removes the rest.
- **Surroundings flicker** on the headset in Low Quality, Backdrop and Full ([SCENE](docs/af/SCENE.md)). It is
  experimental and off by default.
- **Edge flicker.** The Fade and Passthrough edges can flicker ([EDGE](docs/af/EDGE.md)).
- **30 FPS in the busy overworld** when the GPU frame goes over budget.

### Where things live

| Area | Code | Notes |
|---|---|---|
| OpenXR session, frame loop, pacing, input, laser, rig | `runtime/xr/` (`xr_session.c`, `xr_loop.c`, `xr_input.c`, `xr_pointer.c`, `xr_rig.c`, `xr_rig_settings.c`, `hz_filter.h`) | [STEREO](docs/af/STEREO.md), [RIG](docs/af/RIG.md), [CONTROLS](docs/af/CONTROLS.md) |
| Stereo executor, multiview | `runtime/nvn/vulkan/stereo.c`, `stereo.h` | [STEREO](docs/af/STEREO.md), [PASSES](docs/af/PASSES.md) |
| Table edge (fade, passthrough) | `runtime/nvn/vulkan/edge.c` | [EDGE](docs/af/EDGE.md) |
| Surroundings (Low Quality, Backdrop, Full) | `runtime/nvn/vulkan/scene.c` | [SCENE](docs/af/SCENE.md), [STREAM](docs/af/STREAM.md) |
| Eye-tracked foveation (VRS) | `runtime/nvn/vulkan/foveation.c` | [STEREO](docs/af/STEREO.md) "Foveation" |
| VR tab | `runtime/ui/vr_tab.mm`, `runtime/vr_settings.c` | [VR_SETTINGS](docs/af/VR_SETTINGS.md) |
| Shader warm-up | `tools/warm_shaders.sh` | [WARM](docs/af/WARM.md) |
| Free camera probe | `runtime/nvn/cam_override.c` | [FREECAM](docs/af/FREECAM.md) |
| Build, install, release | `Makefile`, `install.sh`, `uninstall.sh`, `steam-frame/` | [BUILD-frame](docs/af/BUILD-frame.md), [INSTALL](docs/af/INSTALL.md) |


### Environment switches

A `NAME=value` line in `~/Games/AwakeningFrame/session.env` is read at launch and wins
over the VR tab (its row greys out). The VR tab's rows map onto `LARECOMP_VR_*` / `LARECOMP_XR_*` variables
([VR_SETTINGS](docs/af/VR_SETTINGS.md) has the list). The ones that matter most:

| Variable | Meaning |
|---|---|
| `LARECOMP_XR=1` | VR mode (the launcher sets it; `awakeningframe.sh --flat` or `AF_FLAT=1` runs the flat window) |
| `LARECOMP_XR_HZ`, `AF_REFRESH_HZ` | refresh rate (120 default; 90) |
| `LARECOMP_MV=0` | per-eye passes instead of multiview |
| `LARECOMP_VK_HEAP_DMABUF=0/1` | the guest heap as driver memory (`LAZY`, `GRANULE_MB` tune it) |
| `LARECOMP_VR_EDGE=off/fade/passthrough` | Table Edge |
| `LARECOMP_VR_SCENE=off/low/backdrop/full` | Surroundings |
| `LARECOMP_VR_FOVEATION`, `LARECOMP_VR_DYNRES`, `LARECOMP_VR_HEADROOM` | foveation level, dynamic resolution, its frame budget |
| `LARECOMP_VR_HEIGHT/DISTANCE/SIZE/TILT/YAW` | the rig's start values |
| `LARECOMP_XR_INFLIGHT=<n>` | cap GPU submissions in flight (experiment) |
| `AF_THROTTLE=1` | sets SteamVR's `framesToThrottle`: SteamVR paces the game to every other refresh and makes the second one itself |
| `AF_VRTIMING=0` | no compositor timing log |
| `LARECOMP_WARP_ON_LOAD`, `LARECOMP_WARP_AT` | warp to a spot for tests |

Every other switch is documented where it is read (grep `getenv("LARECOMP_` and the docs/af tables).


### Test tooling

([TOOLING](docs/af/TOOLING.md), [TOUR](docs/af/TOUR.md)).
- `tools/xrmock/`: a mock OpenXR runtime. Scripted head poses and controllers (`MOCK_XR_INPUT`), eye-image
  captures, an image-vs-pose probe and a compositor GPU model. Point `XR_RUNTIME_JSON` at its `runtime.json`.
- `tools/vr_tour.sh` (`--quick`, `--hz "90 120"`): a tour of the game's spots with timing and eye images.
  `tools/mv_check.sh`: multiview and per-eye eye images agree.
- `tools/vrtiming.py`: reads SteamVR's compositor timing on the headset.
- `steam-frame/test-install.sh`, `steam-frame/test-release.sh`: install, update and uninstall against scratch folders
  and stand-ins for Steam and SteamVR. `steam-frame/test/` holds a Frametanium sandbox and a mock of Steam's CEF port.
- `steam-frame/privacy-check.sh`: scans what this branch added (or a folder or archive) for personal data. Run it
  before every push and release.


### Sharing the headset

The Frame is also the player's console. Never build or run the game while
`~/.cache/headset-in-use` exists (the launcher creates it); the build scripts and tours refuse with exit 75. Run one
game instance at a time, keep runs short, use `nice` and at most two build jobs, and watch free memory: the game and
a build together can be killed by the OOM killer. Use a scratch save (`LARECOMP_SAVE_DIR`).


### Performance internals

- Multiview records and submits the game's commands once for both eyes ([STEREO](docs/af/STEREO.md),
  [PASSES](docs/af/PASSES.md)). Eye-independent passes such as shadow maps run once.
- The guest heap can be driver memory (`LARECOMP_VK_HEAP_DMABUF`, made lazily in 2 MB granules), which saves copies but
  costs memory; turn it off if the Frame runs short.
- Dynamic resolution keeps a 10 ms GPU budget at 120 Hz (`LARECOMP_VR_HEADROOM`) so SteamVR's `xrEndFrame` does not
  wait past vsync. The display period filter (`runtime/xr/hz_filter.h`) ignores one-frame blips, so the game does not
  fall into a 30 FPS hold by mistake.
- Foveation reads `XR_EXT_eye_gaze_interaction` and drives `VK_KHR_fragment_shading_rate`; without gaze it draws at full rate.
- Shaders are prepared at every start from the recorded list; `tools/warm_shaders.sh` fills the caches ahead
  ([WARM](docs/af/WARM.md)).

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

   The first time, it recompiles the game code from your files, which takes a few minutes, and then builds the program. It installs it to `~/Games/AwakeningFrame/app` and adds **AwakeningFrame** to your library, as a release does. The script talks to Steam through its local debugging port, which SteamOS has on by default.

To update, pull the repository and run `./install.sh` again. To remove it, run `./uninstall.sh`. `./uninstall.sh --purge` also removes the logs and the data folder, which holds your game files and saves.

A release never replaces a build from source. If `~/Games/AwakeningFrame/app` holds one, Frametanium puts the release in `~/Games/AwakeningFrame/release` and leaves the library entry alone. To switch the entry to the release, run `install.sh --replace-source` from `~/.local/share/frametanium/apps/awakeningframe`.

To build by hand, see [docs/af/BUILD-frame.md](docs/af/BUILD-frame.md): `steam-frame/fetch-vma.sh` and `tools/fetch_openxr.sh` fetch the headers SteamOS doesn't have, then `make gen` recompiles the game code from your files and `make` builds the program, both with `DEPS=$PWD/build/linux/aarch64/deps`. With ccache: `make -j6 DEPS=$PWD/build/linux/aarch64/deps CC="ccache clang" CXX="ccache clang++"`. Skip `make gen` when `build/lasgame.so` is current.

Errors from a source build:

- **`install.sh` says there is no game code and no game files.** Run `steam-frame/prepare-game.sh` first (step 2).
- **`install.sh` says `tools/venv` is missing.** Run the command in step 3.

`steam-frame/build.sh <version>` makes the release tarball, the FrameDrop zip and manifest, and `SHA256SUMS` in
`steam-frame/release/`. It refuses to pack game data, keys, generated code or paths of the build machine.
`steam-frame/privacy-check.sh` checks the branch's history, a folder or a release tarball for personal data before
publishing.

### Credits in detail

- [LAS Recomp](https://github.com/peiikai/LASRecomp) by peiikai and its contributors: the recompilation, the Vulkan
  renderer, the start screen, the randomizer and everything else the game runs on.
- [LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus) by Nycolazs, for its Turnip and Adreno fixes.
- [GalaxyFrame](https://github.com/simplysanatori/GalaxyFrame), my Super Mario Galaxy VR port for the Frame, for the
  OpenXR approach, the frame pacing and the foveation.
- [LAS-Randomizer](https://github.com/Owen-Splat/LAS-Randomizer) by Owen_Splat, Glan and ProfessorLaw, which LAS Recomp's
  randomizer is built on.
- [hactool](https://github.com/SciresM/hactool) by SciresM, which extracts the game.
- The [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) from Khronos, [Dear ImGui](https://github.com/ocornut/imgui),
  [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [SMAA](https://github.com/iryoku/smaa),
  AMD FidelityFX Super Resolution 1, [SDL 3](https://libsdl.org), [FFmpeg](https://ffmpeg.org), [Kenney](https://kenney.nl)'s
  input prompts and the Baloo 2, BIZ UDPGothic and DejaVu Sans fonts.

- Nintendo, for the game. Own your copy.

Upstream's full credits, with every contributor, are in its README, kept here as [README.upstream.md](README.upstream.md). [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) has the full list with licences.

The Legend of Zelda, Link's Awakening and their logos are trademarks of Nintendo. This is an unofficial fan project. It
is not affiliated with or endorsed by Nintendo, Valve or the LAS Recomp authors.

### Licence in detail

LAS Recomp and AwakeningFrame's changes are under the MIT licence ([LICENSE](LICENSE)). Third-party parts keep their own
licences: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No Nintendo game data, keys or generated game code are
included.
