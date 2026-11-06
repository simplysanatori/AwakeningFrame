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
  outside the repository (pinned to commit 1d64a83), and
  `steam-frame/prepare-game.sh` runs it. It has mbedtls (Apache 2.0), LZ4
  (BSD 2-clause) and cJSON (MIT) built in.
- The game code generator (`tools/gen.py`) runs in a Python environment you
  create with [Capstone](https://www.capstone-engine.org) (BSD 3-clause),
  [Unicorn](https://www.unicorn-engine.org) (GPL 2.0; Python bindings BSD)
  and [python-lz4](https://github.com/python-lz4/python-lz4) (BSD 3-clause;
  LZ4: BSD 2-clause), installed from PyPI. They are not linked into the
  program.

## In the release archive

The Steam Frame release (`steam-frame/build.sh`,
`awakeningframe-<version>-linux-arm64.tar.gz` and the FrameDrop zip) holds the
program, without any game code, and what it needs to build the game code
from your own game files on your Frame. Each is a separate program the
installer or the start screen runs; none is linked into the program.

| Component | Version | Use | License | Source |
|---|---|---|---|---|
| [Zig](https://ziglang.org) (`zig/`), trimmed to what compiling C for Linux needs | 0.17.0 | Compiles the game code on the Frame (`zig cc`) | MIT; its LLVM, Clang and LLD: Apache 2.0 with LLVM exceptions; the C library headers and glibc sources it carries keep their own licenses (glibc: LGPL 2.1+). `zig-LICENSE.txt` | https://ziglang.org/download/0.17.0/zig-0.17.0.tar.xz |
| The game code generator (`lasgen/`): `tools/gen.py` frozen by [PyInstaller](https://pyinstaller.org) | PyInstaller 6.22.3 | Turns your game's code into C | PyInstaller: GPL 2.0+ with the bootloader exception, which allows distributing the frozen program | https://github.com/pyinstaller/pyinstaller/releases/tag/v6.22.3 |
| CPython, inside `lasgen/` | 3.14.3 | Runs the generator | PSF License | https://github.com/python/cpython/releases/tag/v3.14.3 |
| [Capstone](https://www.capstone-engine.org), inside `lasgen/` | 5.0.9 | Disassembly | BSD 3-clause | https://pypi.org/project/capstone/5.0.9/ |
| [Unicorn](https://www.unicorn-engine.org), inside `lasgen/` | 2.1.4 | Runs small pieces of code to resolve jump tables | GPL 2.0 (Python bindings: BSD) | https://pypi.org/project/unicorn/2.1.4/, https://github.com/unicorn-engine/unicorn |
| [python-lz4](https://github.com/python-lz4/python-lz4), inside `lasgen/` | 4.4.5 | Unpacks the game's code | BSD 3-clause; LZ4: BSD 2-clause | https://pypi.org/project/lz4/4.4.5/ |
| zlib, bzip2, xz (liblzma), zstd and libffi, inside `lasgen/` (SteamOS's copies, picked up by PyInstaller) | as on SteamOS | Python's compression and C-call modules | zlib; bzip2 (BSD-style); 0BSD / public domain; BSD 3-clause; MIT | https://archlinux.org/packages/ |
| [hactool](https://github.com/SciresM/hactool), with mbedtls | commit 1d64a83 | Extracts your NSP or XCI with your keys | ISC; mbedtls: Apache 2.0. `hactool-LICENSE.txt`, `mbedtls-LICENSE.txt` | https://github.com/SciresM/hactool/tree/1d64a83450e025622f3468c28fc4164dad2c5ef6 |

The generator's own source is `tools/gen.py` and the modules next to it in
this repository; the exact way it is frozen is `tools/lasgen.spec` and
`steam-frame/build.sh`. The fonts above are in the archive too, with their
license files.

## Libraries on the Frame

The program links at run time to libraries SteamOS already has:
[SDL 3](https://libsdl.org) (zlib), [FFmpeg](https://ffmpeg.org)
(LGPL 2.1+, linked dynamically), [shaderc](https://github.com/google/shaderc)
(Apache 2.0), the Vulkan loader and Mesa's Turnip driver. None of them is
included in this repository.

A build from source compiles the game code with the system's clang. A
release compiles it with the Zig it carries, as upstream's release builds do
(README.upstream.md lists those with their licenses).
