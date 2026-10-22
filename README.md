<h1 align="center"><img width="500" height="500" alt="cover" src="https://github.com/user-attachments/assets/34789fa2-801a-4760-847c-a6e37ff9bf13" /></h1>

<h1 align="center">OpenTricky Switch Edition</h1>

<p align="center">
  <b>The Switch port SSX Tricky never got.</b><br>
  Built from the original xbox game, with its features restored, and everything a modern switch release would have.
  <img width="800" height="450" alt="2025010805455200-A5A89B79BFFA76FE148972D7618499B2-ezgif com-video-to-webp-converter" src="https://github.com/user-attachments/assets/56fe884b-6ed3-4ec9-a8ec-c6baae86410d" />
</p>
<p align="center">
  NEW: Easy to use builder for the .nro file. Go to: https://github.com/DevCrashDay/OpenTrickyNX/blob/main/docs/windows-guide.md
  <img width="500" height="600" alt="2026-10-22 15_10_44-OpenTrickyNX Builder" src="https://github.com/user-attachments/assets/db132672-f53f-4713-b9ee-890416a0e807" />

</p>

<p align="center">
  <img alt="Status: experimental" src="https://img.shields.io/badge/status-experimental-orange">
  <img alt="Platform: Nintendo Switch homebrew" src="https://img.shields.io/badge/platform-Nintendo%20Switch%20(homebrew)-e60012">
  <img alt="Graphics: Vulkan or OpenGL" src="https://img.shields.io/badge/graphics-Vulkan%20(NVK)%20%C2%B7%20OpenGL-blue">
  <img alt="Race: 60 fps" src="https://img.shields.io/badge/races-60%20fps-brightgreen">
  <a href="LICENSE"><img alt="License: GPL-3.0-only" src="https://img.shields.io/badge/license-GPL--3.0--only-green"></a>
</p>

> [!IMPORTANT]
> **You need your own game.** No disc image, game code, music, video or ready-made `.nro` is distributed, and none will be.
> You build the `.nro` yourself from a disc image (`.iso` or `.xiso`) of ***SSX Tricky* for the original Xbox, USA release**.
> The build contains code translated from your copy of the game, so keep it to yourself.

---

## Contents

- [What is this?](#what-is-this)
- [What works](#what-works)
- [What you need](#what-you-need)
- [Build](#build)
- [Install and play](#install-and-play)
- [Controls](#controls)
- [Settings](#settings)
- [Known issues](#known-issues)
- [Other platforms](#other-platforms)
- [Credits](#credits)
- [Legal](#legal)

---

## What is this?

OpenTrickyNX runs *SSX Tricky* (2001) natively on the Switch. It is not an emulator: the game's own Xbox code is translated to C with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and compiled for the Switch's ARM CPU. The Xbox's graphics, sound and input
hardware is reimplemented on Vulkan (Mesa NVK) or OpenGL, SDL2 audio and SDL2 controllers, through devkitPro's libnx.

It is built on [**OpenTricky**](https://github.com/GiZcesi/OpenTricky), the PC port by GiZcesi, which continues
[SSX Tricky PC](https://github.com/MatiasRiveraC/SSX-Tricky-PC) by MatiasRiveraC. Everything OpenTricky restores (fog, lens flares,
colours and more) comes along.

## What works

- **Plays on Switch hardware**: boot, menus, sound, saves and races.
- **60 fps races** with the default build options. The game's x86 registers live in the Switch CPU's own registers, so the translated
  code runs about three times faster than a straight translation.
- **Two renderers**: Vulkan through Mesa NVK (the one tested at 60 fps) and OpenGL through Mesa Nouveau.
- **Two players**: a second controller is player 2.
- **Frame counter**: an optional in-game fps display.
- **Easy Windows builder**: `OpenTrickyNX Builder.bat` opens a window that installs the tools, builds the game from your disc
  and copies it to your SD card ([step-by-step guide](docs/windows-guide.md)).
- **Home-menu icon**: an optional `.nsp` forwarder, so the game starts from the home menu as a full application.

## What you need

| | |
|---|---|
| **A computer** | Windows, Linux or macOS, with **Docker** (Docker Desktop on Windows and macOS, started before the build). It provides the Switch compiler (devkitPro's `devkitpro/devkita64` image, about 3 GB, downloaded on the first build). |
| **On Windows** | [MSYS2](https://www.msys2.org/), with these packages (run in its **UCRT64** shell):<br>`pacman -S --needed mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow mingw-w64-ucrt-x86_64-binutils` |
| **On Linux or macOS** | Python 3 with `capstone`, `numpy` and `pillow` (`python3 -m pip install capstone numpy pillow`), and binutils (`objdump`). |
| **For Vulkan** | The `mesa-switch-nvk-sdk` download from this repository's latest "Mesa NVK SDK" run ([Actions](https://github.com/DevCrashDay/OpenTrickyNX/actions) tab). Unzip it and keep its `portlibs/switch` folder. |
| **The game** | A disc image of your own ***SSX Tricky* Xbox disc, USA release**. The PS2, GameCube, PAL and Japanese versions won't work. |
| **The console** | A Switch running custom firmware with the Homebrew Menu. |

## Build

Get the source: `git clone https://github.com/DevCrashDay/OpenTrickyNX.git`, or the source `.zip` of the
[latest release](https://github.com/DevCrashDay/OpenTrickyNX/releases).

**Windows, the easy way:** double-click **`OpenTrickyNX Builder.bat`**. A window installs the tools, builds the game from your
disc and copies it to your SD card, with no commands to type. [docs/windows-guide.md](docs/windows-guide.md) walks through it.

**Windows by hand** (PowerShell, in the OpenTrickyNX folder, with the tools from [What you need](#what-you-need)), Vulkan renderer:

```powershell
$env:VULKAN = "1"
$env:NVK_SDK = "C:\path\to\portlibs\switch"
.\build_switch.bat "C:\path\to\SSX Tricky (USA).iso"
```

Leave out the two `$env:` lines for the OpenGL renderer.

**Linux or macOS**:

```bash
VULKAN=1 NVK_SDK=/path/to/portlibs/switch ./build_switch.sh "path/to/SSX Tricky (USA).iso"
```

The script reads `default.xbe` from your disc image, translates it to C (about 3 minutes), and compiles
`dist/switch/opentricky/SSX Tricky.nro` (10 to 20 minutes the first time, then only what changed).

| Build option | |
|---|---|
| `SWITCH_JOBS=2` | Compiles fewer files at once. Use it when the build stalls because Docker is short of memory. |
| `SWITCH_OPTS="..."` | CMake options for the build folder, which keeps them, e.g. `-DOT_GUEST_REGS=OFF -DOT_FAST_MEMCHECK=OFF` to turn the speed options off. |
| `GLSLANG_DIR` | glslang built for the Switch (Vulkan only). Without it the first Vulkan build makes one in `build/glslang-switch`. |

In PowerShell set them as `$env:SWITCH_JOBS = "2"`. [docs/switch.md](docs/switch.md) has the details.

## Install and play

1. Copy `dist/switch/opentricky/` to `sdmc:/switch/` on the SD card.
2. Extract your disc into a folder named `game` inside it, so that `sdmc:/switch/opentricky/game/default.xbe` exists
   (with extract-xiso: `extract-xiso -x "SSX Tricky (USA).iso" -d game`). An `.iso` in `sdmc:/switch/opentricky/` works too, when there is
   no `game` folder.
3. Hold **R** while starting any installed game to open the Homebrew Menu **over a game**, then start *SSX Tricky (OpenTricky)*.
   Started from the album, homebrew doesn't get enough memory: the game says so and stops.

**Home-menu icon (optional):** `build_switch_nsp.bat "C:\path\to\prod.keys" "C:\path\to\cover.png"` (or `./build_switch_nsp.sh`) makes
`dist/switch/SSX Tricky.nsp`, which starts the `.nro` as a full application, with no holding R. It is made with your console's own keys,
so keep it to yourself. See [docs/switch.md](docs/switch.md#a-home-menu-icon-optional).

## Controls

Joy-Cons (attached or held together), a Pro Controller or a single Joy-Con held sideways all work as an Xbox pad:
**ZL / ZR** are the triggers, **+** is Start and **−** is Back. The bindings are in `settings.ini`, as on Windows.

**Two players:** hold **−** and press **+** on any controller to open the console's Controllers screen and choose players 1 and 2, then pick a
two-player race in the game.

## Settings

Everything lives in `sdmc:/switch/opentricky/`:

| | |
|---|---|
| `SSX Tricky env.txt` | Read at start, one `KEY=VALUE` per line. The build copies it there with every option listed. |
| `settings.ini` | Optional: the same file as on PC ([settings guide](https://opentricky.com/settings)). Write only the lines you change. |
| `Saves/` | Your saved games. |
| `SSX Tricky.log` | What the game reports, for bug reports. |

Useful lines in `SSX Tricky env.txt`:

| | |
|---|---|
| `XBOX_SHOW_FPS=1` | The frame rate in the top-left corner of the picture (`0` hides it). |
| `OT_CPU_MHZ=1785`, `OT_GPU_MHZ=768`, `OT_MEM_MHZ=1600` | Clocks while the game runs (the defaults). `0` keeps the system's. More heat and battery. |
| `XBOX_PERF=1` | A timing summary in the log every 2 seconds. |
| `XBOX_PROFILE=60,20` | From 60 s after start, for 20 s, logs where each thread spends its time. |

## Known issues

- **Experimental**: not yet played through from start to finish. Please report crashes and glitches.
- **Shader stutter (Vulkan)**: the first time an effect appears its shader is compiled, so the first race of each kind can stutter. The
  compiled pipelines are kept in `vk_pipeline.cache`, and later races are smooth.
- **Not on the Switch yet**: post-processing, SMAA and soft shadows.
- **Two players**: needs more testing. Four-player split screen is not available.

Please report what you see in [Issues](https://github.com/DevCrashDay/OpenTrickyNX/issues): what you did, what happened, and
`SSX Tricky.log`.

## Other platforms

- **Windows**: the original Direct3D 11 build is unchanged. `build.bat "C:\path\to\SSX Tricky (USA).iso"`, see [docs/building.md](docs/building.md),
  or get the ready-made PC releases from [OpenTricky](https://github.com/GiZcesi/OpenTricky/releases).
- **Linux (native)**: the same sources build with SDL2 and OpenGL or Vulkan, see [docs/switch.md](docs/switch.md#building-on-linux).

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
