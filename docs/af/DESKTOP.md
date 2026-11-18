# Testing on a desktop

A desktop (x86-64 Linux, any GPU) builds the program with the VR mode in and runs it against the mock OpenXR
runtime ([TOOLING](TOOLING.md)). It does not take the renderer paths the Steam Frame takes, because the renderer picks
many of them by GPU vendor or driver ([vulkan-gpu-defaults](../vulkan-gpu-defaults.md)). Run a check twice: with the
desktop's own defaults, and with the Frame-like profile.

```
set -a; . tools/frame_profile.env; set +a
```

The tools pick `build/linux/$(uname -m)/deps`, so `tools/fetch_openxr.sh`, `tools/xrmock/build.sh` and
`make DEPS=$PWD/build/linux/x86_64/deps` work as written on both machines. `tools/desk_tests.sh` runs the unit tests
that need no game files.

## What the Frame's driver reports

`tools/vkprobe/run.sh` prints what the renderer asks the driver; `--frame` does it for the Frame's Turnip (an Arm64
build under qemu with the driver copied from the Frame, which draws nothing). Turnip 26.3 on the Adreno 750 says:

| Question | Frame (Turnip) | RTX 3080 |
|---|---|---|
| Device type | integrated | discrete |
| `maxStorageBufferRange` | 128 MB | 4 GB |
| `VK_EXT_external_memory_host` | no | yes |
| `VK_EXT_map_memory_placed`, dma-buf export | yes | yes |
| Memory | one device-local heap holding a cached coherent host-visible type | VRAM, and a system heap with one |
| Shading-rate attachment texels | 8x8 | 16x16 |
| `layeredShadingRateAttachments` | no (the code does not read it) | yes |
| Subgroup size, fragment-stage shuffles | 128, yes | 32, yes |
| D24S8 as attachment and sampled | yes | yes |
| ASTC, BC | both | BC only |
| `VK_EXT_device_fault`, `VK_KHR_pipeline_binary` | no | yes (diagnostics, off by default) |

## What the profile forces

`tools/frame_profile.env` takes the Frame's choices as the code makes them (Turnip, 0.1.1):

| Choice | Frame default | Switch |
|---|---|---|
| Structured shaders, static stacks | on (vendor 0x5143) | `LARECOMP_VK_STRUCTURE`, `LARECOMP_VK_STATIC_STACKS` |
| c1 inlined, constant buffer words as push constants | on | `LARECOMP_VK_INLINE_C1`, `LARECOMP_VK_PUSH_CB` |
| Guest heap as driver memory, no host import, no copies | on (driver ID Turnip) | `LARECOMP_VK_HEAP_DMABUF`, `LARECOMP_VK_NO_HOST_IMPORT` |
| Heap in 128 MB chunks by device address | on (storage range below the heap) | `LARECOMP_VK_GMEM_LAZY`, `LARECOMP_VK_GMEM_CHUNK_MB` |
| No page write tracking, overlap by hash probe | on (Turnip) | `LARECOMP_SOFT_WRITE_TRACKING=0`, `LARECOMP_OVERLAP_WATCH=0`, `LARECOMP_GPU_OVERLAP=1` |
| Vertex cache off | off where there is no page write tracking | `LARECOMP_VK_VCACHE=0` |
| Integrated-GPU defaults: lean usage, SMAA stencil, no per-frame clears, bilinear upscaler | on | `LARECOMP_INTEGRATED`, `LARECOMP_VK_LEAN_USAGE`, `LARECOMP_VK_SMAA_STENCIL`, `LARECOMP_NO_FRAME_CLEAR` |

`tools/frame_profile_copies.env` (load it after the first) is the other way to run the Frame: the heap not as driver memory,
shader global memory through copies of the bound buffers, and the VR overlap built on them (`exec.c` `gmir_on`, `overlap_on`).
The code makes driver memory Turnip's default; `docs/TECHNICAL.md` says copies are the default and AGENTS.md says to turn
driver memory off when memory is short. Which one a Frame session runs is set by its `session.env`.

Not forced because the Frame takes the same path already: branch stacks as arrays, fragment-stage subgroup shuffles,
D24S8, dynamic vertex input, the recorder thread, optimised SPIR-V (Mali only), split barriers (Mali only), the
placeholder colour attachment (Intel only). `MOCK_XR_VK_MAX=1.2` is not part of it: the mock then refuses a Vulkan 1.3
instance, and SteamVR does not (`xr_vk.c`: it says 1.2 and makes a 1.3 instance), so the program stops.

The start screen in VR mode logs the choices as `[vk]` lines: `guest heap as driver memory (placed map ...)`,
`shader global memory: the heap as driver memory, no copies` (copies profile: `copies of the bound buffers`), `guest heap
bound to shaders in 128 MB chunks of the driver memory, by device address`, `structured shaders on (LARECOMP_VK_STRUCTURE)`,
`constant-buffer push constants on`, `lean usage on, SMAA stencil on`. The overlap, the write watch and the vertex cache are
decided once the game runs (`GPU overlap on ... hash probe`, `storage write watch off`, `no page write tracking`); those lines
are the ones to compare with a Frame log.

## What the desktop cannot take

No switch forces these; they stay Frame-only checks:

- The heap made lazily in 2 MB granules from a sparse buffer, and the Frame kernel's page-fault behaviour behind "no soft
  tracking": keyed on the driver ID at `device.c:773` (`heap_no_split`) and `device.c:776` (`heap_sparse`), used at
  `exec.c:4289` (`heap_lazy_try`), `exec.c:4574` (`backend_guest_heap_prepare`) and `device.c:950` (the sparse queue). The
  desktop commits the whole heap; the profile's switches give the same decisions after that.
- Foveation: the shading-rate map needs 8 px texels (`foveation.c:61` `VRS_TEXEL`, checked at `foveation.c:142`); the
  desktop GPU has 16, so foveation is "unavailable" there, and the one-map-for-both-views path (`foveation.c:385` `vrs_pass`)
  does not run.
- ASTC textures: Turnip samples them (a quarter of the memory); the desktop decodes them on the CPU to RGBA8
  (`textures.c:390`, `astc.c`). Memory use and texture upload cost differ.
- The shader compiler (ir3), tiled rendering (bins, GMEM, LRZ, UBWC), 16-bit float and 128-wide subgroup behaviour, the
  memory type the heap lands in (`exec.c:3255` `heap_note_type`, `exec.c:3265` `heap_noncoherent`), and every timing.
  Compile checks of the Frame's compiler run under qemu (below); everything else needs the headset.

## The Arm64 program from the desktop

`steam-frame/cross-build.sh` builds `build/aarch64/lasrecomp` (VR mode in, no game code) and SDL3 and FFmpeg for the Frame
with clang's `--target` against an aarch64 glibc 2.39 root filesystem (`SYSROOT`), packs them as `build/aarch64/portable/`
(what `steam-frame/build.sh` takes as `AF_PORTABLE_DIR`), points its RUNPATH at `$ORIGIN/lib`, and runs
`steam-frame/tools/check-bundle.sh` on it and a start under qemu. It takes the build lock `AF_CROSS_LOCK`, nice 19 and at most
8 jobs. `steam-frame/build.sh` itself needs an arm64 machine (its game code generator is frozen by PyInstaller for the
machine it runs on, and it uses patchelf). The root filesystem has no libXss, so SDL3's X11 screensaver inhibit is left out;
every other SDL3 video and audio backend of the arm64 runner's build is in.

`tools/qemu_vr.sh` runs that program in VR mode against an Arm64 build of the mock runtime, under qemu with the Frame's Turnip
driver and Mesa's no-op msm shim, and prints the `[vk]` lines. It logs the Frame's own choices (`structured shaders on (default
for this GPU)`, `lean usage on`, 8 px foveation texels, ...). The shim draws nothing, and qemu has no userfaultfd and no
placed memory mapping, so the guest heap falls back to copies there: the heap paths are not what the Frame runs.

## The Frame's shader compiler from the desktop

`tools/ir3_check.sh [count] [seed]` builds `tools/test_vk_pipelines.c` for Arm64 and runs it under qemu against the Frame's
Turnip: fuzzed Maxwell programs are translated with the Frame's shader switches, in the Frame's two ways to reach the game's
memory (128 MB chunks of driver memory, copies), compiled by shaderc and made into real pipelines by ir3. No game files are
needed. Failures are counted before the driver (the translator's GLSL does not compile) and in the driver (pipeline
creation). The validation layer is not on under qemu and the programs are fuzzed, not the game's. With the structured-shader
switch on, about 1 program in 400 gets GLSL that shaderc refuses (`'pc' : undeclared identifier`, `continue statement only
allowed in loops`); no pipeline has failed in the driver.

The program's own shader preparation (`shader_prep.c`) finds each shader by its hash in the player's game files, so it needs
them; whether it ends under the shim is not known yet.

The Dev kit (`docs/af/DEVKIT.md`) builds the game code for the Frame here too: `steam-frame/dev/make-dev-app.sh` runs `make ARCH=aarch64 gen`
and the Arm64 `lasgame.so`, and `lasrecomp --game-code-status` under qemu accepts it. Under qemu the program loads it and runs the game's
start (the intro movie); the shim draws nothing and qemu is far too slow to reach a level.
