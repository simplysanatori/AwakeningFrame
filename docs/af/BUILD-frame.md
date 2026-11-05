# Building on the Steam Frame (SteamOS, aarch64)

Base: `upstream/beta_6` (LAS Recomp beta_6, 6f76ef8), branch `steam-frame`. beta_6 is the first line that carries the
aarch64 / Turnip-relevant renderer work of #483 / #556 (shader global memory through copies where the heap can't be
imported), #562 (structured shaders), #563 (push constants) and #564 (guest heap as driver memory with soft write
tracking); `main` (beta_5.4) has none of it. The one commit `test-beta6-preview` has on top is a launcher string.

Tested on: Snapdragon 8 Gen 3 / Adreno 750, Mesa Turnip 26.3.0-devel, kernel 7.2, 4K pages, clang / lld 21, SDL3 3.4.2,
shaderc 2026.1, Vulkan headers 1.4.309. Only the start screen has run; the game itself is untested (no game files yet).

## Setup
```
tools/venv:        python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn
VMA header:        steam-frame/fetch-vma.sh                (Vulkan Memory Allocator 3.3.0 into build/linux/aarch64/deps)
build:             make -j5 DEPS=$PWD/build/linux/aarch64/deps            # the start screen only
with ccache:       make -j5 CC="ccache clang" CXX="ccache clang++" DEPS=...
hactool:           steam-frame/build-hactool.sh            (into ../tools-bin/hactool, outside the repo)
game files:        steam-frame/prepare-game.sh game.nsp prod.keys [title.keys] --dest $PWD/extracted
game code:         make gen DEPS=...  &&  make -j5 CC=... DEPS=...
```
`~/Games/AwakeningFrame/afbuild.sh` (outside the repo) does the build part: it refuses to run while
`~/.cache/headset-in-use` exists, runs `make gen` when `extracted/exefs/main` is there and `generated/` is not (or with
`--regen`), then `make` through ccache, and logs to `~/Games/AwakeningFrame/logs/`.

Run from the repo root with `extracted/` present (dev mode: everything stays in the checkout), or without it to use
`~/.local/share/lasrecomp`. Pointing `LARECOMP_DATA_ROOT` at another folder works as on the other systems. Under
gamescope the window shows on the Frame's own display; for a check without one, `SDL_VIDEODRIVER=offscreen`.

## What was needed for aarch64
- Nothing in the runtime failed to compile or link natively with clang 21: 0 errors (the only warnings are VMA's own
  nullability notes).
- SteamOS has no `libvulkan-memory-allocator-dev`: `steam-frame/fetch-vma.sh` puts the pinned 3.3.0 header where
  `tools/linux/build_deps.sh` would, and `DEPS=` makes the Makefile look there.
- The generated game code runs the guest's CRC32 / AES / SHA / PMULL as inline asm on aarch64 hosts
  (`tools/lifter.py`, `tools/simd_c.py`); clang's baseline armv8-a refuses them ("instruction requires: crc"). The
  Makefile now builds the generated code (and the start screen's own build of it) with `-mcpu=generic+crc+crypto`
  (`GENARCH` still overrides). Checked with a test file only: the real code isn't generated without the game.

## Ported from the Android fork (Nycolazs/LASRecomp-Android-Plus)
The fork is built on beta_5 plus a phone port; most of what it fixed for Turnip is already in beta_6. Taken:
- Quad shuffles in fragment shaders emulated through derivatives where the GPU reports subgroup shuffles for compute
  only (older Turnip hung the GPU otherwise). The Frame's driver reports fragment support, so nothing changes here;
  `LARECOMP_VK_FRAG_SUBGROUPS=0` forces the emulation for a test.
- `LARECOMP_VK_ACQUIRE_FENCE=1`: wait for the window image with a fence instead of a semaphore (a device-lost lever;
  off by default).
Left out: Turnip's LRZ off (`TU_DEBUG=nolrz`: an Adreno 650 water artefact, Android only; the env variable works by
itself if the Frame shows it), the format-list for UBWC (beta_6's lean usage already lists every colour image's formats
on integrated GPUs, which the Frame is), fences instead of timeline semaphores (Android's KGSL kernel driver only), the
shader-global-memory mirror (beta_6's copies path, #556, replaces it), per-stage pipeline feedback records (beta_6 only
attaches feedback to graphics pipelines), the phone performance rounds (native fp16, ieee min / max, precise positions,
early submits, GPU performance mode, 1080p dynamic resolution; measured on Adreno 643 / 650, they need the game to judge),
and framegen, touch controls, the app and the translations.

## Game files
`steam-frame/prepare-game.sh <game.nsp|game.xci> <prod.keys> [title.keys] [--dest DIR] [--force]` uses hactool. It needs
your own `prod.keys` (from your console, e.g. Lockpick_RCM); retail NSPs carry their ticket, so `title.keys` is only for
stripped dumps. It refuses update-only, DLC, compressed (.nsz / .xcz) and non-base packages, checks the build ID at
offset 0x40 of `exefs/main` against `runtime/game_addresses/versions.h` (1.0.0 AE16F71E... preferred; 1.0.1 / 1.1.0 are
supported too), and only then writes `extracted/exefs` and `extracted/romfs`. Default destination is
`${LARECOMP_DATA_ROOT:-~/.local/share/lasrecomp}/extracted`. The keys live in a private temporary folder for the run.

## Untested
- The game itself: `make gen`, the game code build, the first frames, performance, controls.
- `prepare-game.sh` past its checks (argument handling and every failure path are exercised; extraction of a real
  NSP / XCI and the title-key path are not).
- hactool with this game's key generation (hactool's last release predates the newest firmware key sets; the 2019
  NCAs should use an early generation).
