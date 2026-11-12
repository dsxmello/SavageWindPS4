<p align="center"><img src="docs/media/banner.png" alt="Savage Wind" width="640"></p>

<p align="center">
  <b>PS4 port</b> ·
  <a href="#get-started">Get started</a> ·
  <a href="#status">Status</a> ·
  <a href="docs/ps4.md">PS4 guide</a> ·
  <a href="https://github.com/dsxmello/SavageWindPS4/issues">Issues</a> ·
  <a href="https://github.com/Revivedx/SavageWindRecomp">Savage Wind Recomp</a>
</p>

**Savage Wind PS4** is a port of [Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp) - a static
recompilation of *The Legend of Zelda: Breath of the Wild* (Nintendo Switch, version 1.9.0) - to a jailbroken
**PlayStation 4** running GoldHEN. The game code is recompiled on your PC from your own copy of the game and linked into
a PS4 homebrew package; on the console it runs natively on the Jaguar CPU, with Vulkan (Mesa's RADV) on the PS4's own
GPU.

> [!IMPORTANT]
> **No game files are included, and no package will ever be shared.** You need your own legally obtained copy of the
> game, updated to **version 1.9.0** and dumped from **your own** Nintendo Switch. This project does not provide game
> files, keys, firmware, dumping or decryption tools, packages with the game code, or instructions on how to obtain any
> of them. The game code is recompiled locally, on your machine, from your files.

> [!NOTE]
> **Made almost entirely with AI.** Nearly all of the code, tooling, reverse engineering and documentation is written
> by an AI (Anthropic's Claude, through Claude Code), directed, tested and reviewed by a human. It builds on
> [Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp) and [LAS Recomp](https://github.com/peiikai/LASRecomp),
> made the same way. If you don't trust code written by AI, you can simply ignore this project.

> [!WARNING]
> **Work in progress, for testers.** This is an independent port: it is not affiliated with Savage Wind Recomp, and
> bugs found here should be reported here, not upstream. The port is being moved from Savage Wind Recomp's Alpha 3
> onto **0.5.2**; that version isn't built with the game code nor run on the console yet.

## Features

- Everything Savage Wind Recomp 0.5.2 runs, on a base PS4 or a PS4 Pro with GoldHEN
- Vulkan on the console's own GPU (Mesa's RADV over GNM), linked into the package
- The DualShock 4 as the game's controller, by button position
- Saves, settings, caches and the log in `/data/savagewind/`
- Profile-guided optimization (PGO) and ThinLTO builds for the Jaguar

## Get started

**You need:** a PS4 with [GoldHEN](https://github.com/GoldHEN/GoldHEN) (package installer and FTP server on port
2121), a Linux PC (x86-64) to build on, your own game files (`exefs/` and `romfs/` of version 1.9.0, see
[Game files](docs/game-files.md)) and about 16 GB free on the console.

1. Set up the PS4 toolchain from [TriAevum PS4](https://github.com/dsxmello/TriAevumPS4) (`PS4_VK_ROOT`, see
   [docs/ps4.md](docs/ps4.md)).
2. Put your files in `extracted/` (`exefs/` and `romfs/`), then on the PC:

   ```sh
   make gen                                        # recompile the game code (generated/)
   SW_GENERATED=$PWD/generated ps4/build.sh        # dependencies, then the eboot
   ps4/package.sh 00.01                            # -> $SW_PS4_BUILD/out/IV0000-SVWD00001_00-....pkg
   ```

3. Send your game files to the console by FTP, as they are: `exefs/`, `romfs/` and the DLC's `aoc1/` and `aoc2/`
   (optional) into `/data/savagewind/extracted/`. The game reads the RomFS straight from its folder.
4. Send the package (for example to `/data/pkg/`), install it with GoldHEN's package installer and start
   **Savage Wind** from the home screen. It goes straight into the game.

A package built with the game code contains code made from your own copy of the game. It is yours alone: never share
it. Something wrong? See [docs/ps4.md](docs/ps4.md).

## Saves

Everything the game writes - saves, caches, settings and the log (`savagewind.log`, and `savagewind.prev.log` for the
run before) - is in `/data/savagewind/` on the console. To back up your saves, copy that folder by FTP. Debug and test
switches go in `/data/savagewind/env.txt`, one `KEY=VALUE` a line, read at start.

## Status

Measured on a base PS4 (1.6 GHz Jaguar) with the earlier port (Alpha 3, game version 1.8.2):

| Place | fps |
|---|---|
| Shrine of Resurrection | ~43 |
| Open world (Great Plateau, standing still) | ~28 |
| Kakariko Village (4,000-5,000 draws a frame) | 21-23 |

The game is CPU-bound on the PS4 (the GPU is about half busy). The goal is a steady 30 fps; 60 isn't realistic on the
Jaguar. What's being worked on:

- The move onto Savage Wind Recomp 0.5.2: its registers in C locals, futex-style kernel locks, recorder thread and
  faster multiply-adds (the Jaguar has no FMA instructions).
- The cost of translating each draw from the game's command lists to Vulkan: from ~5 µs to ~2 µs a draw.
- Profile-guided optimization of the final code (+12% in earlier tests).

## Controls

The PS4 controller works as the game's controller, by button position. The first time an effect is seen it can
stutter while its pipeline compiles; the cache in `/data/savagewind/cache` keeps it for later.

## Found a bug?

Report it [here](https://github.com/dsxmello/SavageWindPS4/issues), not upstream, with your `savagewind.log` from
`/data/savagewind/`. Never attach game files, keys, generated code or packages.

## Building and testing

The full guide is [docs/ps4.md](docs/ps4.md). On the PC, `tools/test_softfma.c` checks the software FMA the Jaguar
uses bit for bit. The PC side is Savage Wind Recomp's: [Windows](docs/BUILD-windows.md) and [Linux](docs/linux.md).

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

## Credits and license

- **[Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp)** by Revivedx and its contributors - the
  recompiled game, its runtime and everything on the PC side (MIT), built on
  **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by peiikai (MIT). Its full list:
  [Credits](docs/third-party.md).
- PS4 side: the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain), the PS4 Vulkan stack
  of DolphinPS4 and dusklight-ps4 (Mesa's RADV on GNM), [GoldHEN](https://github.com/GoldHEN/GoldHEN), and
  [TriAevum PS4](https://github.com/dsxmello/TriAevumPS4), where that toolchain and the PS4 work were first set up.
- "Taming the Jaguar" (Andreas Fredriksson, Insomniac Games, GDC 2016) - the guide to the PS4's CPU used for tuning.
- Libraries on the PS4: [Mesa](https://mesa3d.org) (MIT), [SDL 3](https://libsdl.org) (zlib),
  [FFmpeg](https://ffmpeg.org) (LGPL), [glslang](https://github.com/KhronosGroup/glslang) (BSD-3 and others),
  [volk](https://github.com/zeux/volk) (MIT).

The code is under the [MIT License](LICENSE), as are Savage Wind Recomp and LAS Recomp. Third-party components in
`third_party/` keep their own licenses. The license covers this project's code only: it grants nothing for Nintendo's
game, which you must supply yourself.
