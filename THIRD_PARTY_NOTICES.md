# Third-party notices

This repository and the program built from it contain or are derived from
the following works, used under their licenses. No Nintendo game files, game
code, assets or fonts are included.

## LAS Recomp

AwakeningFrame is a fork of LAS Recomp by peiikai and its contributors
(https://github.com/peiikai/LASRecomp): the recompiler tools, the runtime,
the Vulkan renderer, the start screen and the randomizer additions come from
it. LAS Recomp is released under the MIT License, as are AwakeningFrame's
changes: see [LICENSE](LICENSE). Upstream's README, with its credits and its
table of the components its own release builds bundle, is kept as
[README.upstream.md](README.upstream.md).

## Vendored in `third_party/`

Each keeps its own license file in the repository.

| Component | Use | License | File |
|---|---|---|---|
| [LAS-Randomizer](https://github.com/Owen-Splat/LAS-Randomizer) by Owen_Splat, with local changes | The randomizer | MIT | `third_party/LAS-Randomizer/LICENSE.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut | The in-game menu, the VR tab and the start screen | MIT | `third_party/imgui/LICENSE.txt` |
| [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez | The anti-aliasing option | MIT | `third_party/smaa/LICENSE.txt` |
| An open-source project's audio renderer and shader instruction decoders | The audio renderer's reverb and mixer, and the bit-layout reference for parts of the shader translator | MIT | `third_party/ryujinx/LICENSE.txt` |
| [Kenney](https://kenney.nl) Input Prompts | The D-pad on the HUD | CC0 1.0 | `third_party/kenney/LICENSE.txt` |
| [Baloo 2](https://github.com/EkType/Baloo2) by Ek Type | The start screen's font | SIL OFL 1.1 | `third_party/fonts/Baloo2/OFL.txt` |
| [BIZ UDPGothic](https://github.com/googlefonts/morisawa-biz-ud-gothic) by Morisawa | The game's text when the Switch fonts are not imported | SIL OFL 1.1 | `third_party/fonts/BIZUDPGothic/OFL.txt` |
| [DejaVu Sans](https://dejavu-fonts.github.io) | Menu text and button letters when no system font fits | Bitstream Vera / DejaVu license | `third_party/fonts/DejaVu/LICENSE` |

The installer copies these fonts next to the program.

## AMD FidelityFX Super Resolution 1

The renderer contains a GLSL port of AMD FidelityFX Super Resolution 1
(https://github.com/GPUOpen-Effects/FidelityFX-FSR), Copyright (c) 2021
Advanced Micro Devices, Inc., under the MIT License.

## Fetched at build time

These are downloaded by the build scripts, pinned and checksummed, into the
build folder. They are not part of this repository.

| Component | Fetched by | License |
|---|---|---|
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) 3.3.0, Advanced Micro Devices, Inc. (header, compiled into the program) | `steam-frame/fetch-vma.sh` | MIT |
| [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) 1.1.63, The Khronos Group Inc. (headers the program is compiled against) | `tools/fetch_openxr.sh` | Apache 2.0 |

At run time the program opens SteamVR's own OpenXR loader. The loader that
`tools/fetch_openxr.sh` builds is used only by the mock runtime in
`tools/xrmock/`.

## Tools used to prepare the game

- [hactool](https://github.com/SciresM/hactool) by SciresM (ISC license)
  extracts your game. `steam-frame/build-hactool.sh` downloads and builds it
  outside the repository, and `steam-frame/prepare-game.sh` runs it.
- The game code generator (`tools/gen.py`) runs in a Python environment you
  create with [Capstone](https://www.capstone-engine.org) (BSD 3-clause),
  [Unicorn](https://www.unicorn-engine.org) (GPL 2.0; Python bindings BSD)
  and [python-lz4](https://github.com/python-lz4/python-lz4) (BSD 3-clause;
  LZ4: BSD 2-clause), installed from PyPI. They are not linked into the
  program.

## Libraries on the Frame

The program links at run time to libraries SteamOS already has:
[SDL 3](https://libsdl.org) (zlib), [FFmpeg](https://ffmpeg.org)
(LGPL 2.1+, linked dynamically), [shaderc](https://github.com/google/shaderc)
(Apache 2.0), the Vulkan loader and Mesa's Turnip driver. None of them is
included in this repository.

The Frame build compiles the game code with the system's clang. It does not
use the bundled compiler (Zig) or the PyInstaller helpers of upstream's
release builds; README.upstream.md lists those with their licenses.
