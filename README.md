<h1 align="center">OpenTrickyNX</h1>

<p align="center">
  <b>SSX Tricky on the Nintendo Switch, as native homebrew.</b><br>
  A Switch port of <a href="https://github.com/GiZcesi/OpenTricky">OpenTricky</a>: the original Xbox game, translated to C and built into a <code>.nro</code> from your own disc.
</p>

<p align="center">
  <img alt="Status: experimental" src="https://img.shields.io/badge/status-experimental-red">
  <img alt="Platform: Nintendo Switch homebrew" src="https://img.shields.io/badge/platform-Nintendo%20Switch%20(homebrew)-e60012">
  <img alt="Also: Linux" src="https://img.shields.io/badge/also-Linux%20%C2%B7%20Windows-blue">
  <img alt="Graphics: OpenGL" src="https://img.shields.io/badge/graphics-OpenGL%203.3%20(Mesa)-blue">
  <a href="LICENSE"><img alt="License: GPL-3.0-only" src="https://img.shields.io/badge/license-GPL--3.0--only-green"></a>
</p>

> [!IMPORTANT]
> **You need your own game.** No disc image, game code, music or video is distributed, and no ready-made `.nro` ever will be:
> you build it yourself from a disc image (`.iso` or `.xiso`) of ***SSX Tricky* for the original Xbox, USA release**.

> [!WARNING]
> **Experimental.** The Switch build compiles and links, but it has not been played through on Switch hardware yet.
> Expect crashes and missing effects, and please report what you find.

---

## Contents

- [What is this?](#what-is-this)
- [What you need](#what-you-need)
- [Build](#build)
- [Install and play](#install-and-play)
- [Controls and settings](#controls-and-settings)
- [Status and known gaps](#status-and-known-gaps)
- [Other platforms](#other-platforms)
- [Credits](#credits)
- [Legal](#legal)

---

## What is this?

OpenTrickyNX runs *SSX Tricky* (2001) natively on the Switch. It is not an emulator: the game's own Xbox code is translated to C with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and compiled for the Switch's ARM CPU, and the Xbox's graphics, sound and input chips are
reimplemented on OpenGL (Mesa), SDL2 audio and SDL2 controllers through devkitPro's libnx.

It is built on [**OpenTricky**](https://github.com/GiZcesi/OpenTricky), the PC port by GiZcesi, which continues
[SSX Tricky PC](https://github.com/MatiasRiveraC/SSX-Tricky-PC) by MatiasRiveraC. Everything OpenTricky restores (fog, lens flares, colours, the
PS2 controls) comes along; what is new here is the Switch side:

- **Memory**: the Xbox's 64 MB and the GPU's registers live in one block of Switch memory instead of at fixed Xbox addresses.
- **Hardware registers**: the audio, sound-chip and video-DAC registers are trapped through the Switch's exception handler.
- **Renderer**: a new OpenGL 3.3 backend, with the Xbox's colour combiners turned into GLSL shaders.
- **The disc**: the game reads your `.iso` straight from the SD card while you play; nothing is extracted.

## What you need

| | |
|---|---|
| **A computer** | Windows, Linux or macOS, with **Docker** (Docker Desktop on Windows and macOS). It provides the Switch compiler (devkitPro's `devkitpro/devkita64` image, about 3 GB, downloaded on the first build). |
| **On Windows** | [MSYS2](https://www.msys2.org/), with these packages (run in its **UCRT64** shell):<br>`pacman -S --needed mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow mingw-w64-ucrt-x86_64-binutils` |
| **On Linux or macOS** | Python 3 with `capstone`, `numpy` and `pillow` (`python3 -m pip install capstone numpy pillow`), and binutils (`objdump`). |
| **The game** | A disc image of your own ***SSX Tricky* Xbox disc, USA release** (the PS2, GameCube, PAL and Japanese versions won't work). |
| **The console** | A Switch running custom firmware with the Homebrew Menu. |

## Build

```
git clone -b switch-port https://github.com/DevCrashDay/OpenTrickyNX.git
cd OpenTrickyNX
```

Start Docker, then on **Windows** (or drag the `.iso` onto `build_switch.bat`):

```bat
build_switch.bat "C:\path\to\SSX Tricky (USA).iso"
```

On **Linux or macOS**:

```bash
./build_switch.sh "path/to/SSX Tricky (USA).iso"
```

The script reads `default.xbe` from your disc image, translates it to C, applies the port's fix-up passes, and compiles
`dist/switch/opentricky/SSX Tricky.nro`. [docs/switch.md](docs/switch.md) has the details.

> [!CAUTION]
> Your build contains code translated from your copy of the game. Please don't share it.

## Install and play

1. Copy `dist/switch/opentricky/` to `sdmc:/switch/` on the SD card.
2. Put your disc image in the same folder: `sdmc:/switch/opentricky/SSX Tricky (USA).iso`. Keep it as an `.iso`, don't extract it.
   On a FAT32 card, files over 4 GB don't fit: shrink the image first with `extract-xiso -r`, or use exFAT.
3. Hold **R** while starting any installed game to open the Homebrew Menu **over a game**, then start *SSX Tricky (OpenTricky)*.
   Started from the album, homebrew doesn't get enough memory: the game says so and stops.

Settings live in `sdmc:/switch/opentricky/settings.ini` (written on the first start), saves in `sdmc:/switch/opentricky/Saves/`.

## Controls and settings

Joy-Cons (attached or held together), a Pro Controller or a single Joy-Con held sideways all work as an Xbox pad:
**ZL / ZR** are the triggers, **+** is Start and **−** is Back. The game uses OpenTricky's **original PS2 layout**, and every binding and option is in
`settings.ini`, the same file as on PC, described in OpenTricky's [settings guide](https://opentricky.com/settings).

## Status and known gaps

| | |
|---|---|
| **Builds** | The Linux and Switch builds compile and link in CI (with stand-ins for the game code, since that is never in the repository). |
| **Tested** | The OpenGL renderer passed a draw test on a PC (Mesa's software renderer). **Not yet run on Switch hardware or with the real game.** |
| **Not done yet** | Post-processing and SMAA, soft shadows, GPU vertex programs (they run on the CPU), point sprites (drawn 1 pixel wide), HD textures. |
| **Possible hitches** | The first time an effect appears, its shader is compiled, which may cause a short hitch. |

Please report what you see in [Issues](https://github.com/DevCrashDay/OpenTrickyNX/issues): what you did, what happened, and the output of
`nxlink -s "SSX Tricky.nro"` if you can get it (or `LogFile=1` in `settings.ini`).

## Other platforms

- **Windows**: the original Direct3D 11 build is unchanged. `build.bat "C:\path\to\SSX Tricky (USA).iso"`, see [docs/building.md](docs/building.md),
  or get the ready-made PC releases from [OpenTricky](https://github.com/GiZcesi/OpenTricky/releases).
- **Linux (native)**: the same sources build with SDL2 and OpenGL, see [docs/switch.md](docs/switch.md#building-on-linux).

## Credits

| Who | What |
|---|---|
| [**GiZcesi**](https://github.com/GiZcesi) | [OpenTricky](https://github.com/GiZcesi/OpenTricky), the PC port this Switch version is built on ([Discord](https://discord.gg/r38sThsqnj), [Ko-fi](https://ko-fi.com/giz_music/)). |
| [**MatiasRiveraC**](https://github.com/MatiasRiveraC) | Creator of [SSX Tricky PC](https://github.com/MatiasRiveraC/SSX-Tricky-PC), the port OpenTricky continues: the recompilation, runtime, renderer, launcher and the reverse-engineering work. |
| [**sp00nznet**](https://github.com/sp00nznet) | [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), the static recompiler and Xbox runtime everything is built on (MIT). |
| [**xemu**](https://xemu.app) | The GPU and audio emulation that parts of the renderer and the audio are adapted from (LGPL-2.1-or-later), and the accuracy reference. |
| [**devkitPro**](https://devkitpro.org/) | devkitA64, libnx and the Switch ports of SDL2 and Mesa. |
| **Jorge Jimenez et al.** | [SMAA](https://www.iryoku.com/smaa/) (MIT). |
| [RmlUi](https://github.com/mikke89/RmlUi), [SDL](https://www.libsdl.org/), [FreeType](https://freetype.org/) | The PC launcher's interface, window and input, and text (MIT, zlib, FreeType License). |
| [Capstone](https://www.capstone-engine.org/), [Ghidra](https://ghidra-sre.org/) | Disassembly and reverse engineering. |
| **DevCrashDay** | OpenTrickyNX: the Switch port. |

See [CONTRIBUTORS.md](CONTRIBUTORS.md).

## Legal

- OpenTrickyNX is an unofficial fan project. It is not affiliated with or endorsed by Electronic Arts, EA Canada, Microsoft or Nintendo.
  *SSX* and *SSX Tricky* are trademarks of Electronic Arts, Xbox is a trademark of Microsoft, and Nintendo Switch is a trademark of Nintendo.
- **No disc image, game code, music or video is distributed.** You need your own copy of the game.
- Licences: the code is under the **GNU General Public License v3.0 only** (`GPL-3.0-only`, see [LICENSE](LICENSE)), unless a file says otherwise.
  `xboxrecomp/` keeps its MIT licence (`xboxrecomp/LICENSE`). Files adapted from xemu stay under LGPL-2.1-or-later, as their headers say. SMAA is MIT.
  The PC launcher uses RmlUi (MIT), SDL 3 (zlib), FreeType (FreeType License) and fonts under the SIL Open Font License;
  their licences are in `port/third_party/` and `port/launcher/app/ui/fonts/`.
  Portions of this software are copyright © 2026 The FreeType Project (https://freetype.org). All rights reserved.
- Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first.
