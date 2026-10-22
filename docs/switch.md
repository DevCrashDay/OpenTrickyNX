# OpenTricky on the Nintendo Switch

The Switch build is the same game as the Windows one: SSX Tricky's Xbox code, translated to C from **your own
copy of the game**, running on OpenTricky's Xbox runtime. Only the host side differs: SDL2 for the window,
controllers and sound, OpenGL (Mesa, through libnx) for drawing, and the Switch's own memory and exception
handling. It runs as homebrew from the Homebrew Menu, on a console with custom firmware.

Nothing in this repository contains the game, and no Switch build is ever published: you build it yourself from
your disc image, and the result contains code translated from your copy, so keep it to yourself.

## What you need

| | |
|---|---|
| **A computer** | Windows, Linux or macOS, with Python 3 and Docker (Docker Desktop on Windows and macOS, started before the build). |
| **On Windows** | MSYS2 with the packages of the Windows build ([building.md](building.md#what-you-need)): they include Python with capstone, numpy and pillow. |
| **Python packages** (Linux, macOS) | `python3 -m pip install capstone numpy pillow` |
| **The Switch toolchain** | Nothing to install: the build uses devkitPro's `devkitpro/devkita64` Docker image (devkitA64, libnx, SDL2, Mesa). With devkitPro installed, set `DEVKITPRO=/opt/devkitpro` and Docker is not needed. |
| **The game** | A disc image of your own **SSX Tricky (USA) Xbox** disc (`.iso`, an extract-xiso image or a full dump). |
| **The console** | A Switch running custom firmware with the Homebrew Menu (hbmenu). |

## Build

On Windows (or drag the `.iso` onto `build_switch.bat`):

```bat
build_switch.bat "C:\path\to\SSX Tricky (USA).iso"
```

On Linux or macOS (or from an MSYS2 UCRT64 shell on Windows):

```bash
./build_switch.sh "path/to/SSX Tricky (USA).iso"
```

The first build downloads the `devkitpro/devkita64` image (about 3 GB).

1. **Reads your disc image**: `default.xbe` into `game_files/` (git-ignored), as for Windows.
2. **Translates the game** with `port/tools/fork_regen.sh`, into `port/src/recomp/gen/` (git-ignored).
3. **Compiles for the Switch** in `port/build-switch/` with devkitA64 (`port/posix.cmake`).
4. **Copies the result** to `dist/switch/opentricky/SSX Tricky.nro`.

## Install

1. Copy `dist/switch/opentricky/` to `sdmc:/switch/` on the SD card.
2. Extract your disc into a `game` folder there, so that `sdmc:/switch/opentricky/game/default.xbe` exists.
   With extract-xiso: `extract-xiso -x "SSX Tricky (USA).iso" -d game`, then copy `game` over. Without a `game`
   folder the game uses the first `.iso` in `sdmc:/switch/opentricky/` instead, or the one `settings.ini` names as
   `DiscImage`.
3. Start the Homebrew Menu **over a game** (hold **R** while starting any installed game), then start
   *SSX Tricky (OpenTricky)*.

Start it this way, not from the album: from the album, homebrew gets a small part of the console's memory, and
the game needs the Xbox's whole 64 MB plus the GPU's memory and its own textures. Started from the album, it says
so and stops.

### A home-menu icon (optional)

`build_switch_nsp.bat "C:\path\to\prod.keys" "C:\path\to\cover.png"` (or `./build_switch_nsp.sh` elsewhere)
makes `dist/switch/SSX Tricky.nsp`: an icon for the home menu that starts `sdmc:/switch/opentricky/SSX Tricky.nro`
as a full application, so no holding **R**. The picture is yours to choose (a square one fills the icon), and
`prod.keys` is your console's own key file, dumped with Lockpick_RCM. Install the .nsp with your installer (DBI,
Tinfoil, ...); like any unsigned .nsp it needs custom firmware with signature patches. It is made with your
console's keys, so keep it to yourself. Its loader is nx-hbloader (`port/switch/forwarder/`, ISC licence), set
to start the game and close when the game returns; the home-menu entry keeps working when you rebuild the .nro.

## Files on the SD card

All in `sdmc:/switch/opentricky/`:

| | |
|---|---|
| `SSX Tricky.nro` | The game. |
| `game/` | Your extracted disc (`default.xbe` and the `data` folder). |
| `*.iso` | Or, without `game/`: your disc image, read in place. |
| `settings.ini` | Optional: the settings, the same file as on Windows. Not created by the game; write only the lines you change, under their section, e.g. `[Display]` then the key. |
| `SSX Tricky env.txt` | Settings read at start (see below), copied here by `build_switch.sh` with every option listed. They win over `settings.ini`. |
| `Saves/` | Saved games (the Xbox hard disk's save area). |

### SSX Tricky env.txt

One `KEY=VALUE` per line, `#` starts a comment. Useful lines:

| | |
|---|---|
| `OT_CPU_MHZ=1785` | CPU clock. Default 1785; `0` keeps the system's 1020. |
| `OT_GPU_MHZ=768` | GPU clock. Default 768 (up to 921). |
| `OT_MEM_MHZ=1600` | Memory clock. Default 1600. |
| `OT_CLOCK_FORCE=1` | Set the clocks even while sys-clk or Horizon OC runs (by default their profile rules). |
| `XBOX_SHOW_FPS=1` | The frame rate in the top-left corner of the picture (`0` hides it). Also `ShowFrameRate=1` under `[Advanced]` in `settings.ini`. |
| `XBOX_PERF=1` | Writes a timing summary to the log every 2 seconds. |
| `XBOX_AUDIO_TONE=1` | A 440 Hz beep over the game's sound, to check the sound output alone. The log has an `[XA2]` line every 10 seconds with what reached the speakers. |

The higher clocks (the ones flatout2-sw / fo2x uses docked) cost heat and battery. The system's clocks come back
when the game closes.

## Controls

Any controller the Switch knows: Joy-Cons in the rails or a pair held together, a Pro Controller, or a
single Joy-Con held sideways. They act as Xbox pads through SDL: ZL and ZR are the triggers, **+** is Start and
**−** is Back, and the face buttons follow SDL's Switch layout. The bindings are the ones in `settings.ini`, as on
Windows, and can be changed there.

**Two players.** Player 2 is whichever controller the console counts as player 2. Hold **−** and press **+**
on any controller to open the console's Controllers screen and choose who is player 1 and 2 (for example a
Joy-Con each, held sideways); the game waits while it is open. Then pick a two-player race in the game as on
the Xbox. A controller only counts as connected when the console has one for that player, so before the
second controller is set up the game sees one pad.

## Rendering

The Switch runs the OpenGL renderer through Mesa's Nouveau driver (devkitPro's switch-mesa). SMAA, soft shadows
and smoother legs start off here.

A Vulkan renderer, through the community's Mesa NVK builds for the Switch as flatout2-sw / fo2x uses, can be built
instead. It is new and not yet tried on a console, so OpenGL stays the default:

```bash
VULKAN=1 NVK_SDK=/path/to/mesa-switch/usr/local ./build_switch.sh "path/to/SSX Tricky (USA).iso"
```

On Windows, in a Command Prompt in the OpenTricky folder:

```bat
set VULKAN=1
set NVK_SDK=C:\path\to\mesa-switch\usr\local
build_switch.bat "C:\path\to\SSX Tricky (USA).iso"
```

The first Vulkan build also compiles glslang for the Switch into `build/glslang-switch/` (or set `GLSLANG_DIR` to
one you built). `NVK_SDK` is an install of mesa-switch built with NVK (`include/vulkan/`, `lib/libvulkan.a`): download the `mesa-switch-nvk-sdk` artifact of this repository's latest "Mesa NVK SDK" run on GitHub (Actions tab), unzip it, and use its `portlibs/switch` folder. The build goes
to `port/build-switch-vk/`. Shaders are compiled on the console the first time they are needed; the driver keeps
the compiled pipelines in `vk_pipeline.cache` beside the `.nro`, so the first race of each kind stutters and later
ones do not. SMAA, soft shadows and the post chain are not in the Vulkan renderer.

| | |
|---|---|
| `XBOX_VK_PIPECACHE=0` | No pipeline cache file (or a path for it). |
| `XBOX_VK_DEBUG=1` | Writes the source of any shader that fails to compile to the log. |

### Speed options

Two build options make the translated game faster on the Switch; both are on by default.

- `OT_GUEST_REGS`: the game's x86 registers live in the Switch CPU's own registers instead of memory, and the
  rest of its per-thread CPU state (the x87 float stack and the like) sits behind a register too.
- `OT_FAST_MEMCHECK`: the game's stack accesses skip the address check (the stack is always in game memory).

`OT_NULL_IN_PAGE0` (off) saves one more instruction on every other memory access by letting null pointers land
on game address 0 instead of a page of their own; the game can tell, and a sky effect flashes white with it.

To build without one, for instance to see whether it is behind a problem, set `SWITCH_OPTS` before building:

```bash
SWITCH_OPTS="-DOT_GUEST_REGS=OFF -DOT_FAST_MEMCHECK=OFF" ./build_switch.sh "path/to/SSX Tricky (USA).iso"
```

On Windows, in the same window before `build_switch.bat`: `$env:SWITCH_OPTS = "-DOT_GUEST_REGS=OFF"` in
PowerShell, or `set SWITCH_OPTS=-DOT_GUEST_REGS=OFF` in Command Prompt. The build
folder keeps the setting: build again with `=ON` to go back.

`port/tools/switch/codegen_bench/run.sh` (Linux, with qemu) puts a small x86 program through the translator and
checks that both builds of it print what the x86 program prints.

## Troubleshooting

- **A message says the disc image is missing**: check that `game/default.xbe` is there, or put the `.iso` in `sdmc:/switch/opentricky/`, or give its full
  path (`sdmc:/...`) as `DiscImage` in `settings.ini`.
- **The log**: start it with `nxlink -s "SSX Tricky.nro"` from a computer on the same network to see what it
  prints, or read `SSX Tricky.log`, which the game always writes beside the `.nro`.

## Building on Linux

The same sources build a Linux version, which helps when working on the Switch build:

```bash
cmake -S port -B port/build-linux -DCMAKE_BUILD_TYPE=Release   # needs SDL2 and OpenGL headers
cmake --build port/build-linux -j
```

Without the translated game, `-DSSX_STUB_GEN=ON` links a build check that does not play (what CI builds).

`-DXBOX_VULKAN=ON` builds the Vulkan renderer instead (needs the Vulkan loader and headers, and glslang's static
libraries, e.g. Debian's `libvulkan-dev glslang-dev`). `XBOX_VK_VALIDATE=1` turns on the Khronos validation layer,
and `XBOX_VK_DEVICE=<n>` picks a GPU. The renderer has a test that needs no game, which passes on Mesa's lavapipe:

```bash
cmake -S xboxrecomp -B build-vk -DXBOX_VULKAN=ON -DXBOX_BUILD_TESTS=ON
cmake --build build-vk --target vk_smoke_test
XBOX_VK_VALIDATE=1 build-vk/src/d3d/vk_smoke_test        # and again with: msaa
```
