# Building on Windows, step by step

This guide is for people who have never built anything before. The **OpenTrickyNX Builder** window installs
what is needed, builds the game from your own disc and copies it to your SD card. You press four buttons.

You need:

- A Windows 10 or 11 PC with about **25 GB free** and an internet connection. 16 GB of memory is comfortable;
  8 GB works, more slowly.
- A disc image (`.iso`) of your own ***SSX Tricky* Xbox disc, USA release**. The PS2, GameCube, PAL and Japanese
  versions won't work.
- A Switch with custom firmware and the Homebrew Menu, and its SD card (with a card reader for the PC).

Nothing of the game is downloaded: the builder makes it from your disc, so the result is yours alone. Don't share it.

## 1. Download OpenTrickyNX

1. Open the [latest release](https://github.com/DevCrashDay/OpenTrickyNX/releases/latest) and click
   **Source code (zip)** at the bottom.
2. Right-click the downloaded zip, choose **Extract All...**, and extract it to a short folder such as
   `C:\OpenTrickyNX`. Avoid OneDrive, Desktop and Downloads folders: the build writes many files.
3. Open that folder. You should see `OpenTrickyNX Builder.bat` among the files.

## 2. Start the builder

Double-click **`OpenTrickyNX Builder.bat`**. If Windows says *"Windows protected your PC"*, click
**More info** and then **Run anyway**. The OpenTrickyNX Builder window opens.

The window has four numbered parts, top to bottom: tools, your game, build and SD card, then an optional part for a
home-menu icon. Do them in order.

## 3. Install the tools (once)

Press **Install / check**. The builder downloads and sets up:

| | What it is | Where it goes |
|---|---|---|
| **MSYS2** | Python and small tools that translate the game's code | `C:\msys64` |
| **Docker Desktop** | Runs the Switch compiler | Installed like any program |
| **The Switch compiler** | devkitPro's `devkitpro/devkita64`, about 3 GB | Inside Docker |
| **Vulkan driver files** | Mesa NVK, for the faster renderer | `build\windows\nvk-sdk` in the OpenTrickyNX folder |

Along the way:

- Windows asks **twice for permission** (to turn on WSL and to install Docker Desktop). Click **Yes**.
- After Docker Desktop is installed the builder asks you to **restart the computer**. Do that, start the builder
  again and press **Install / check** once more.
- Docker Desktop opens a window the first time. **Accept** its terms; you can **skip** signing in and the survey.
  Leave Docker Desktop running while you build (it can stay in the taskbar corner).

When the three lines at the top all say **[OK]**, the tools are ready. You never have to do this part again.

## 4. Pick your game

Under **2. Your game**, press **Browse...** and choose your `SSX Tricky (USA).iso`. Leave **Vulkan** selected
(it is faster; OpenGL is there in case Vulkan gives you trouble).

## 5. Build

Press **Build SSX Tricky.nro**. The first build takes **20 to 40 minutes**; the messages show what it is doing and the
line at the bottom shows the step (1 of 4 to 4 of 4). You can use the PC meanwhile. Later builds, after an update,
only redo what changed.

When it says *dist\switch\opentricky\SSX Tricky.nro is built*, it's done.

## 6. Copy to the SD card

1. Put the Switch's SD card in the PC.
2. Under **4. Copy to the SD card**, press **Refresh**, pick the card's drive letter, and press **Copy to SD card**.

This puts on the card:

```
SD card
└── switch
    └── opentricky
        ├── SSX Tricky.nro        the game
        ├── SSX Tricky env.txt    settings (clocks, frame counter)
        └── game\                 the files from your disc
```

Copying your disc's files takes a few minutes. Eject the card safely when it says the SD card is ready.

## 7. Play

1. Put the card back in the Switch and boot into custom firmware.
2. **Hold R** while starting any installed game: the Homebrew Menu opens instead of the game.
3. Pick **SSX Tricky (OpenTricky)**.

It has to be started this way, over a game. Started from the album, homebrew doesn't get enough memory and the game
says so and stops. To skip holding R, make a home-menu icon (below).

## Optional: a home-menu icon

This makes an `.nsp` that starts the game straight from the home menu. You need your console's `prod.keys`
(dumped on your Switch with Lockpick_RCM) and any picture for the icon (a square one fills it best).

Under **Optional: home-menu icon**, pick both files and press **Make .nsp**. It is saved as
`dist\switch\SSX Tricky.nsp` (**Open output folder** takes you there). Install it with your usual installer
(DBI, Tinfoil, ...). It is made with your console's keys, so keep it to yourself.

## Updating

Download the new release's zip and extract it **over** the old folder (say yes to replacing files). Start the builder,
press **Build**, then **Copy to SD card**. Your settings and the disc files already on the card are kept.

## If something goes wrong

The builder shows a message saying what stopped it, and every message is kept in `build\windows\` in the
OpenTrickyNX folder (`setup.log`, `build.log`, `sdcard.log`, `nsp.log`).

| What you see | What to do |
|---|---|
| *Docker Desktop did not start* | Open Docker Desktop from the Start menu, accept its terms, wait until it says it is running, and press the button again. |
| The build stops after a long time without messages | Close other programs and build again. In Docker Desktop, *Settings > Resources* can give it more memory. |
| *the download failed* | Check the internet connection and press the button again; finished downloads are not repeated. |
| *the Vulkan files are missing* or their download fails | Choose **OpenGL** and build again, or ask in Issues. |
| *not an Xbox disc image* / *default.xbe not found* | The `.iso` isn't the Xbox USA version of SSX Tricky, or it is damaged. |
| The game crashes or looks wrong on the Switch | Report it in [Issues](https://github.com/DevCrashDay/OpenTrickyNX/issues) with `SSX Tricky.log` from the SD card's `switch\opentricky` folder. |

The builder runs the same scripts as the command line, so everything in [switch.md](switch.md) still applies.
