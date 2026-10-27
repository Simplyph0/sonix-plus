# Apps

The player's own filesystem is read-only and has about two megabytes to spare, so anything big
cannot be added to the firmware. **Apps** live on the microSD card instead. You copy a folder to
the card, and the player shows it as a tile.

Nothing is linked into the player. An app is a separate program that the player starts, hands the
screen, the touch panel and the sound to, and takes them back from when it ends. That is why an app
can be under any licence, and why the player stays GPLv3.

## Installing and removing

Open **More > Games**, **More > Emulators** or **More** once with a card in. The player makes an
`Apps` folder on the card, with a README in it. Copy an app's folder into it:

```
Apps/
└── NES/
    ├── manifest.ini
    ├── icon.png
    └── nes             the program
```

Open the page again and the tile is there. To remove an app, delete its folder. Apps are read each
time a page opens, so nothing needs a restart.

**An app is a program that runs as root with the whole player at its disposal. Only install ones
you trust.**

## manifest.ini

```ini
[app]
name = NES                  shown under the tile (required)
exec = nes                  the program, inside the app's folder (required)
folder = Emulators          Games, Emulators or More (default More)
icon = icon.png             a PNG; 112x112 is the size drawn, others are scaled to fit
version = 1.0
author = Somebody
description = A line about it

rom_dir = Games/NES         for an app that opens files: the folder of the card to list...
rom_ext = nes,zip           ...and the extensions that count, without the dot
```

Lines starting with `;` or `#` are comments. `exec` and `icon` are paths inside the app's folder and
cannot go above it. An app whose program is missing is not shown.

### Apps that open a file

With `rom_dir`, tapping the tile opens a list of the files in that folder of the card (and its
subfolders, three deep) that have one of the `rom_ext` extensions. The player makes the folder if
it is not there, so the first time you open it you can see where to put your files. Tapping a file
starts the program with that file's path as its only argument and in `SONIX_ROM`. Without `rom_dir`
the program just starts.

## What the player gives an app

The program is started in its own folder, in a process group of its own, with standard input from
`/dev/null` and its output and errors going to the player's log. Music playback is stopped first.
The environment says what it needs:

| Variable | Value |
| --- | --- |
| `SONIX_API` | `1` |
| `SONIX_APP_DIR` | the app's own folder, which is also the working directory |
| `SONIX_SD` | the root of the card |
| `SONIX_FB` | the framebuffer, `/dev/fb0` |
| `SONIX_WIDTH`, `SONIX_HEIGHT` | the panel in pixels: 480x720 (R3 Pro II) or 480x800 (R1) |
| `SONIX_ROTATED` | `1` when the picture is turned 180 degrees (Settings), else `0` |
| `SONIX_TOUCH` | the touch panel's evdev node |
| `SONIX_AUDIO_FD` | a pipe the app writes its sound into |
| `SONIX_ROM` | the file the user picked, for an app with `rom_dir` |

### Screen

The framebuffer is 16 bits per pixel (RGB565) and the player leaves it two screens tall (the second
screen is for flipping). An app draws on the screen that is not showing and pans to it, as the
player does. If `SONIX_ROTATED` is set the app has to turn its own picture, and its touch
coordinates, by 180 degrees.

### Touch

The panel is read from the evdev node. The two players use different drivers, which report fingers
differently (the R3 Pro II numbers contacts in type A packets, the R1 reports each finger every
other scan, and positions come from `ABS_X` and `ABS_Y`, which are in pixels). Whilst an app runs,
the player's own touch reader is switched off, so there is only one reader.

### Sound

Sound goes through the player so that the output the user chose, the volume and Bluetooth all keep
working. The app writes this to `SONIX_AUDIO_FD`:

```
char magic[4] = "SNXA"; uint32 rate; uint32 channels;      little-endian, 12 bytes
then signed 16-bit interleaved samples
```

The pipe is small and writing to it blocks, so the sound card sets the pace: write one frame's
worth of sound per frame drawn and the app runs at the right speed. An app with no sound never
writes.

### Leaving

While an app runs the player cannot draw anything, so **the app has to draw its own way out** and
exit when it is touched. When the program ends, the player takes the screen and the touch panel
back, repaints, and ignores touches for a moment so that the finger that left the app does not land
on a tile underneath. Anything the app left running in its process group is stopped.

If an app hangs, the power button's long press still works.

## Writing one

[sonix-app-sdk](https://github.com/Simplyph0/sonix-app-sdk) has all of the above as one header
(`sonixapp.h`: video, touch, audio, a little drawing and text), a libretro frontend that plays any
libretro core, and two examples. Programs have to be **32-bit little-endian MIPS (mipsel,
MIPS32r2)**, and linking them statically means they do not depend on what the player has installed.
The MIPS toolchain that `make target` builds makes them.

## Apps

| App | What | Repository | Licence |
| --- | --- | --- | --- |
| Hello | The smallest app: colour bars, a circle under every finger, a tone while you touch | [sonix-app-sdk](https://github.com/Simplyph0/sonix-app-sdk) (`examples/hello`) | MIT |
| Frontend test | The emulator frontend with a test core: the pad, a tone, a save counter | [sonix-app-sdk](https://github.com/Simplyph0/sonix-app-sdk) (`examples/frontend-test`) | MIT |
| NES | Nintendo Entertainment System, with FCEUmm | [sonix-app-nes](https://github.com/Simplyph0/sonix-app-nes) | MIT (the app), GPLv2 (FCEUmm) |
| GBA | Game Boy Advance, with gpSP and its MIPS recompiler | [sonix-app-gba](https://github.com/Simplyph0/sonix-app-gba) | MIT (the app), GPLv2 (gpSP) |
| SNES | Super Nintendo, with Snes9x 2005. A recipe only: Snes9x is non-commercial, so there is no program to download, and you build it yourself | [sonix-app-snes](https://github.com/Simplyph0/sonix-app-snes) | MIT (the recipe), Snes9x licence |

The three emulators are the SDK's libretro frontend with a core linked in. They put the game on the
top of the screen and a pad below it, keep battery saves in `Saves/<system>/` and one save state per
game, and list the files of `Games/NES`, `Games/GBA` or `Games/SNES`. The GBA and SNES apps have
been run with test ROMs on a computer, not on a player yet.

## Built in

Some things are in the firmware itself, because they use the player's own interface:

- **Games**: Flappy Bird, Snake, 2048 and Breakout.
- **Emulators**: Game Boy and Game Boy Color (Gearboy).
