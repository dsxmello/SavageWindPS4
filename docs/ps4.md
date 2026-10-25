# PS4 (work in progress)

Savage Wind on a jailbroken PS4 with GoldHEN, as a homebrew package. The runtime is the same as on the PC. Below it,
the PS4 build swaps in four things:
- **Graphics:** Vulkan through Mesa's RADV driver running on the console's own GPU driver (GNM), linked into the eboot.
- **Pads and audio:** SDL3 with SceAudioOut audio and the pads as virtual gamepads.
- **Videos:** a minimal FFmpeg.
- **Platform layer:** `runtime/platform/ps4.c`.

It builds on the PS4 Vulkan stack of DolphinPS4 and dusklight-ps4, as TriAevum PS4 set it up and ran it on the console.

**Status:** the eboot builds and packages. Nothing has run on a console yet. A package without the game code runs a
console test instead of the game (see "First test").

## What it needs

- The toolchain from TriAevum PS4 (`PS4_VK_ROOT`, default `~/Projetos/triaevum/triaevum-ps4-vk`):
  - the PacBrew OpenOrbis SDK
  - the libc++ 21 / glslang sysroot
  - the Mesa 26.0.8 RADV build
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

## On the console

The package is installed with GoldHEN's package installer. Everything the title writes goes to `/data/savagewind/`:
- the log: `savagewind.log`, and the previous run's `savagewind.prev.log`
- saves
- caches
- settings

Your game files go there too, sent by FTP (GoldHEN's FTP server, port 2121):

```
/data/savagewind/extracted/exefs/     main, main.npdm, rtld, sdk, subsdk0
/data/savagewind/extracted/romfs.bin  the RomFS image (build/savagewind extracted --build-romfs on the PC)
```

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
- **Host stacks:** 16 MB per guest thread instead of 256 MB, from direct memory (`plat_alloc_stack`): plain pthread
  stacks come from the process's small flexible memory, which ran out at the game's 6th thread. `SAVAGEWIND_HOST_STACK_MB` sets it on the PC, to find what
  the game needs.
- **Paths:** the kernel refuses relative paths, so `ps4/ps4_paths.c` wraps the path calls (`--wrap`).
- **No start screen and no game-code build on the console.** Both happen on the PC.
- **GLSL to SPIR-V** goes through glslang (`ps4/shaderc_glslang.c` implements the part of shaderc's API the runtime
  uses).

## Known gaps

- Vertex formats R8 / R16 USCALED / SSCALED deform models on RADV/GFX7 (found by TriAevum PS4). They may need a CPU
  conversion here too.
- The guest heap is not imported for the GPU yet: `ac_ps4_drm.c` has no userptr. Shader global memory reads return zero
  and vertex data is copied.
- Pipelines compile on the console's CPU (ACO): the first sight of an effect stutters until the cache under
  `/data/savagewind/cache` has it.
