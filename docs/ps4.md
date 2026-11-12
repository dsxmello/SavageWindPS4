# PS4 (work in progress)

Savage Wind on a jailbroken PS4 with GoldHEN, as a homebrew package. The runtime is the same as on the PC. Below it,
the PS4 build swaps in four things:
- **Graphics:** Vulkan through Mesa's RADV driver running on the console's own GPU driver (GNM), linked into the eboot.
- **Pads and audio:** SDL3 with SceAudioOut audio and the pads as virtual gamepads.
- **Videos:** a minimal FFmpeg.
- **Platform layer:** `runtime/platform/ps4.c`.

It builds on the PS4 Vulkan stack of DolphinPS4 and dusklight-ps4, as TriAevum PS4 set it up and ran it on the console.

**Status:** it runs on the console: the Shrine of Resurrection and the open world play with sound, saving and loading
(base PS4: ~43 fps in the shrine, ~28 in the open world, 21-23 in Kakariko). A package without the game code runs a
console test instead of the game (see "First test").

## What it needs

- The toolchain from TriAevum PS4 (`PS4_VK_ROOT`, default `~/Projetos/triaevum/triaevum-ps4-vk`):
  - the PacBrew OpenOrbis SDK
  - the libc++ 21 / glslang sysroot
  - the Mesa 26.0.8 RADV tree (copied and built here with Savage Wind's patch: `ps4/build-mesa.sh`)
  - the patched DolphinPS4 tree

  These are used read-only.
- `cmake`, `ninja`, `git`, `curl`.
- `nasm` (optional): without it, FFmpeg is built without its x86 assembly.

`ps4/env.sh` sets everything. Savage Wind's own PS4 sources and builds go to `SW_PS4_BUILD` (default
`~/savagewind-ps4-build`):
- SDL3: the PS5 port's tag `deps/sdl3-ps5` plus dusklight-ps4's `sdl3-ps4.patch`.
- FFmpeg 9.0.2.
- volk.

## Building

```sh
ps4/build.sh                 # dependencies when missing, then the eboot (runtime only)
ps4/package.sh 00.01         # -> $SW_PS4_BUILD/out/IV0000-SVWD00001_00-SAVAGEWIND000000.pkg
```

With the game code, build on the PC from your own game files:

```sh
make gen                                     # extracted/exefs -> generated/ (x86-64: the portable C SIMD mode)
SW_GENERATED=$PWD/generated ps4/build.sh clean
ps4/package.sh 00.01
```

A package built with the game code contains code made from your own copy of the game. It is yours alone: never share
it.

### Profile-guided optimization

As TriAevum PS4 does it: an instrumented eboot records what runs while you play, and the next build lays out and
inlines the runtime and the game code from that record. Both PGO builds compile the game code with clang -O2 without
the vectorizers (`SW_GEN_OPT`; the plain build keeps -O1), each in its own tree.

```sh
PGO=generate SW_GENERATED=$PWD/generated ps4/build.sh   # 1. instrumented (build/savagewind-pgo-generate)
PGO=generate ps4/package.sh 00.01                       #    play it: /data/savagewind/pgo/savagewind.profraw,
                                                        #    rewritten every 30 s ("[pgo]" lines in the log)
PS4_FTP=ftp://<console ip>:2121 ps4/pgo-merge.sh        # 2. -> $SW_PS4_BUILD/pgo/savagewind.profdata
PGO=use SW_GENERATED=$PWD/generated ps4/build.sh        # 3. optimized (build/savagewind-pgo-use)
PGO=use ps4/package.sh 00.01
```

The instrumented eboot is slower: play it where you want speed (the open world), not the menus. A profile goes stale
as the runtime changes (the files that changed lose their part of it; the game code's stays good): collect again
after big changes.

## On the console

The package is installed with GoldHEN's package installer. Everything the title writes goes to `/data/savagewind/`:
- the log: `savagewind.log`, and the previous run's `savagewind.prev.log`
- saves
- caches
- settings

Your game files go there too, sent by FTP (GoldHEN's FTP server, port 2121):

```
/data/savagewind/extracted/exefs/     main, main.npdm, rtld, sdk, subsdk0 (version 1.9.0)
/data/savagewind/extracted/romfs/     the RomFS folder, base game and update merged (read in place)
/data/savagewind/extracted/aoc1/      optional: the DLC's RomFS folders (docs/game-files.md)
/data/savagewind/extracted/aoc2/
```

A `romfs.bin` image from the earlier port (Alpha 3) is only read when there is no `romfs/` folder.

## First test (package without the game code)

`ps4/no_game.c` runs instead of the game and logs each step to `savagewind.log`:

1. **Memory:** 128 MB of the guest heap get memory on their first touch. This exercises the fault handler in
   `runtime/platform/ps4.c`.
2. **Vulkan:** the device and the VideoOut swapchain come up. The screen is cleared in a cycling color.
3. **Pads:** the left stick moves the color, the face buttons are logged, and Options ends the test.

## How it differs from the PC

- **Guest memory** (`runtime/platform/ps4.c`):
  - The 12.5 GB window at `0x7000000000` is a `sceKernelReserveVirtualRange` reservation.
  - 2 MiB chunks of it get direct memory on first touch (SIGSEGV), or through `plat_commit` before the kernel writes
    there (file reads).
  - The chunks are CPU and GPU read/write, so a GPU address is the CPU address.
  - Only what the game touches costs memory. The plain package identity gives the process about 4.5 GiB of direct
    memory.
- **CPU:**
  - The game code is built for the Jaguar (`-march=btver2`: AVX, no AVX2 / FMA) from the portable C SIMD mode.
  - FMADD and the like go through `runtime/simd_softfma.h` (the PC's compatible target uses it too): exact, but slow.
  - On a PC, `make GENARCH=-march=btver2 B=build/btver2` builds that variant to test it.
- **Host stacks:** 2 MB per guest thread instead of 256 MB, from direct memory (`plat_alloc_stack`): plain pthread
  stacks come from the process's small flexible memory, which ran out at the game's 6th thread. `SAVAGEWIND_HOST_STACK_MB` sets it on the PC, to find what
  the game needs.
- **Paths:** the kernel refuses relative paths, so `ps4/ps4_paths.c` wraps the path calls (`--wrap`).
- **No start screen and no game-code build on the console.** Both happen on the PC.
- **GLSL to SPIR-V** goes through glslang (`ps4/shaderc_glslang.c` implements the part of shaderc's API the runtime
  uses).

## Known gaps

- Vertex formats R8 / R16 USCALED / SSCALED deform models on RADV/GFX7 (found by TriAevum PS4). They may need a CPU
  conversion here too.
- The guest heap is imported for the GPU (shader global memory, vertices read in place) through Savage Wind's own
  Mesa build (`ps4/build-mesa.sh`, `ps4/mesa-savagewind.patch`): user memory buffers in DolphinPS4's winsys, at their
  own address. The heap is backed on first touch, so a GPU access to a chunk the CPU never touched faults the GPU;
  vertex and index ranges are backed before they are used, shader global memory isn't.
- Pipelines compile on the console's CPU (ACO): the first sight of an effect stutters until the cache under
  `/data/savagewind/cache` has it.
