# Savage Wind PS4

> [!IMPORTANT]
> **This project is made almost entirely with AI.** Nearly all of the code, tooling, reverse engineering and
> documentation here is written by an AI (Anthropic's Claude, through Claude Code), directed, tested and reviewed by
> a human. It builds on [Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp) and
> [LAS Recomp](https://github.com/peiikai/LASRecomp), which were made the same way. If you don't trust code written by
> AI, you can simply ignore this project. As with anything on the internet, you run the code at your own discretion.

**Savage Wind PS4** is a port of [Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp) - a static
recompilation of *The Legend of Zelda: Breath of the Wild* (Nintendo Switch) - to a jailbroken **PlayStation 4**
running GoldHEN. The game code is recompiled on your PC from your own copy of the game and linked into a PS4
homebrew package; on the console it runs natively on the Jaguar CPU, with Vulkan (Mesa's RADV) on the PS4's own GPU.

This is an independent port. It is not affiliated with the Savage Wind Recomp project, and bugs found here should be
reported here, not upstream. For the Windows and Linux versions, go to
[Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp).

> [!WARNING]
> **Work in progress, for testers.** It boots, plays the Shrine of Resurrection and the open world with sound,
> saving and loading, but it runs below 30 fps in heavy places and it hasn't been played far. Performance work is
> ongoing (see [Status](#status)).

> [!NOTE]
> **No game files or game code are included, and no package will ever be shared.** You need your own legally
> obtained copy of the game, dumped from **your own** Nintendo Switch. The PS4 package you build contains code
> generated from your game: it is for your own use only. This project does not include, and will not provide, game
> files, keys, firmware, dumping or decryption tools, packages with the game code, or instructions on how to obtain
> any of them.

## Status

Measured on a base PS4 (1.6 GHz Jaguar):

| Place | fps |
|---|---|
| Shrine of Resurrection | ~43 |
| Open world (Great Plateau, standing still) | ~28 |
| Kakariko Village (4,000-5,000 draws a frame) | 21-23 |

The game is CPU-bound on the PS4 (the GPU is about half busy). The goal is a steady 30 fps; 60 isn't realistic on
the Jaguar. What's being worked on, in order:

- Jaguar-specific tuning: 128-bit SIMD only, no 64-bit multiplies in hashing, fewer pointer chases in the
  translation of the game's command lists to Vulkan.
- Less waiting between the game's threads (core scheduling, threads kept on one CPU cluster).
- Profile-guided optimization (PGO) of the final code: +12% in earlier tests.

The numbers above were measured on the earlier port, based on Savage Wind Recomp's Alpha 3 with game version 1.8.2.
The port is now being moved onto Savage Wind Recomp 0.5.2, which needs game version **1.9.0**: work in progress, not
yet built with the game code nor run on the console.

## What you need

- A PS4 with [GoldHEN](https://github.com/GoldHEN/GoldHEN) (package installer and FTP server on port 2121).
- A Linux PC (x86-64) to build: the game code is generated and compiled there, not on the console.
- Your own *Breath of the Wild* and its update, extracted from your own console:
  - the **ExeFS** (`main`, `main.npdm`, `rtld`, `sdk`, `subsdk0`)
  - the **RomFS** with the update applied (base game and update merged)
- About 16 GB free on the console for the game files (the RomFS image is ~15 GB).

## Building

The full guide is [docs/ps4.md](docs/ps4.md). In short:

1. The PS4 toolchain (OpenOrbis SDK, libc++ / glslang sysroot, Mesa RADV, DolphinPS4's Vulkan stack) is the one
   [TriAevum PS4](https://github.com/dsxmello/TriAevumPS4) sets up; `ps4/env.sh` points at it (`PS4_VK_ROOT`).
2. Put your files in `extracted/` (`exefs/` and `romfs/`), then on the PC:

   ```sh
   SW_PROMOTE_VMAX=6 make gen GEN=generated-v GENARGS=--promote   # recompile the game code (C, for the Jaguar)
   build/savagewind extracted --build-romfs                       # the RomFS image (extracted/romfs.bin, ~15 GB)
   SW_GENERATED=$PWD/generated-v ps4/build.sh                     # dependencies, then the eboot (~13 min)
   ps4/package.sh 00.01                                           # -> $SW_PS4_BUILD/out/IV0000-SVWD00001_00-....pkg
   ```

   (`build/savagewind` is the PC build, needed once for `--build-romfs`: see
   [Savage Wind Recomp's Linux guide](https://github.com/Revivedx/SavageWindRecomp/blob/main/docs/linux.md).)
3. Optional, for speed: a profile-guided build (`PGO=generate`, play, `ps4/pgo-merge.sh`, `PGO=use`), described in
   [docs/ps4.md](docs/ps4.md).

## Installing and playing

1. Send your game files to the console by FTP:

   ```
   /data/savagewind/extracted/exefs/     main, main.npdm, rtld, sdk, subsdk0
   /data/savagewind/extracted/romfs.bin  the RomFS image
   ```

2. Send the package (for example to `/data/pkg/`) and install it with GoldHEN's package installer.
3. Start **Savage Wind** from the home screen. It goes straight into the game (there's no start screen on the PS4).

Everything the game writes - saves, caches, settings and the log (`savagewind.log`, and `savagewind.prev.log` for the
run before) - is in `/data/savagewind/`. Debug and test switches go in `/data/savagewind/env.txt`, one `KEY=VALUE` a
line, read at start (see [docs/ps4.md](docs/ps4.md)).

The PS4 controller works as the game's controller, by button position. The first time an effect is seen it can
stutter while its pipeline compiles; the cache in `/data/savagewind/cache` keeps it for later.

## Testing

- `tools/test_vkrec.c` checks the Vulkan recording thread's ring on the PC (order, copies, wrap-around, full ring),
  under ThreadSanitizer or AddressSanitizer; the build command is at its top.
- `tools/test_softfma.c` checks the software FMA the Jaguar uses (it has no FMA instructions) bit for bit.

## Respect copyright

- *The Legend of Zelda: Breath of the Wild* is © Nintendo. This is a fan project. It is not affiliated with,
  endorsed by or sponsored by Nintendo or Sony. *Nintendo Switch* and *The Legend of Zelda* are trademarks of
  Nintendo; *PlayStation* is a trademark of Sony Interactive Entertainment.
- This repository contains no game files, no game code (also not the C code the recompiler generates from it), no
  assets, no fonts from the game, no keys, and no packages.
- The game code you generate and the package made from it contain Nintendo's code. They are for your own use only:
  don't share them, upload them or ship them.
- Issues, pull requests or discussions that contain game files, keys, generated code, packages or links to pirated
  copies will be removed. Piracy is not supported in any way.
- Follow the copyright laws of your country. Dumping a game you own for personal use, and running homebrew on a
  console you own, are legal in some countries and not in others; that is your responsibility.

## Credits

- **[Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp)** by Revivedx and its contributors - the
  recompiled game, its runtime and everything on the PC side (MIT).
- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by peiikai - the recompiler and runtime Savage Wind is built
  on (MIT).
- PS4 side: the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain), the PS4 Vulkan stack
  of DolphinPS4 and dusklight-ps4 (Mesa's RADV on GNM), [GoldHEN](https://github.com/GoldHEN/GoldHEN), and
  [TriAevum PS4](https://github.com/dsxmello/TriAevumPS4), where that toolchain and the PS4 work were first set up.
- "Taming the Jaguar" (Andreas Fredriksson, Insomniac Games, GDC 2016) - the guide to the PS4's CPU used for tuning.
- Upstream's credits apply too: [Ryujinx](https://github.com/Ryujinx/Ryujinx), [envytools](https://github.com/envytools/envytools),
  [Switchbrew](https://switchbrew.org), [Ghidra](https://github.com/NationalSecurityAgency/ghidra) and the
  [Switch Loader](https://github.com/Adubbz/Ghidra-Switch-Loader).
- Libraries: [Mesa](https://mesa3d.org) (MIT), [SDL 3](https://libsdl.org) (zlib), [FFmpeg](https://ffmpeg.org)
  (LGPL), [glslang](https://github.com/KhronosGroup/glslang) (BSD-3 and others), [volk](https://github.com/zeux/volk)
  (MIT), [Dear ImGui](https://github.com/ocornut/imgui) (MIT),
  [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) (MIT),
  [SMAA](https://github.com/iryoku/smaa) (MIT), [AMD FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
  and [CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) (MIT), [Fira Sans](https://github.com/mozilla/Fira) and
  [Noto Sans Thai](https://github.com/notofonts/thai) (SIL OFL 1.1), [stb](https://github.com/nothings/stb)
  (public domain / MIT), [Capstone](https://www.capstone-engine.org) (BSD), [Unicorn](https://www.unicorn-engine.org)
  (GPL 2.0), [python-lz4](https://github.com/python-lz4/python-lz4) (BSD).

## License

The code is open source under the [MIT License](LICENSE), as are Savage Wind Recomp and LAS Recomp, on which it
builds. Third-party components in `third_party/` keep their own licenses. The license covers this project's code
only: it grants nothing for Nintendo's game, which you must supply yourself.
