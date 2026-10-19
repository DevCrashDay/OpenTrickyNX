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
2. Copy your disc image into that folder too: `sdmc:/switch/opentricky/SSX Tricky (USA).iso`. The game uses the
   first `.iso` it finds there, unless `settings.ini` names another one as `DiscImage`.
3. Start the Homebrew Menu **over a game** (hold **R** while starting any installed game), then start
   *SSX Tricky (OpenTricky)*.

Start it this way, not from the album: from the album, homebrew gets a small part of the console's memory, and
the game needs the Xbox's whole 64 MB plus the GPU's memory and its own textures. Started from the album, it says
so and stops.

## Files on the SD card

All in `sdmc:/switch/opentricky/`:

| | |
|---|---|
| `SSX Tricky.nro` | The game. |
| `*.iso` | Your disc image. The game reads it in place; nothing is extracted. |
| `settings.ini` | The settings, the same file as on Windows (written on first start). Edit it on a computer. |
| `Saves/` | Saved games (the Xbox hard disk's save area). |

## Controls

Any controller the Switch knows: Joy-Cons in the rails or a pair held together, a Pro Controller, or a
single Joy-Con held sideways. They act as Xbox pads through SDL: ZL and ZR are the triggers, **+** is Start and
**−** is Back, and the face buttons follow SDL's Switch layout. The bindings are the ones in `settings.ini`, as on
Windows, and can be changed there.

## Rendering

The Switch runs the OpenGL renderer through Mesa's Nouveau driver. Vulkan is not available to Switch homebrew
(only OpenGL and deko3d), and deko3d needs its shaders compiled in advance, while this game's colour combiners
are built into shaders while it runs. The renderer sits behind one interface, so another backend can be added
later.

## Troubleshooting

- **A message says the disc image is missing**: put the `.iso` in `sdmc:/switch/opentricky/`, or give its full
  path (`sdmc:/...`) as `DiscImage` in `settings.ini`.
- **The log**: start it with `nxlink -s "SSX Tricky.nro"` from a computer on the same network to see what it
  prints, or set `LogFile=1` in `settings.ini` for a log file beside it.

## Building on Linux

The same sources build a Linux version, which helps when working on the Switch build:

```bash
cmake -S port -B port/build-linux -DCMAKE_BUILD_TYPE=Release   # needs SDL2 and OpenGL headers
cmake --build port/build-linux -j
```

Without the translated game, `-DSSX_STUB_GEN=ON` links a build check that does not play (what CI builds).
