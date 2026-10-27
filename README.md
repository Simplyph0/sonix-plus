# Sonix Plus

My fork of **[Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player)**: a replacement player for
the **HiBy R3 Pro II** and the **HiBy R1**, written on LVGL. Sonix Player is Jepl4r's work and I kept their
history as it was. Both are under the GNU General Public License version 3 ([LICENSE](LICENSE)).

Everything the player can do is in **[FEATURES.md](FEATURES.md)**, what changed in each release is in
[CHANGELOG.md](CHANGELOG.md), and how to build it is in [BUILDING.md](BUILDING.md).

> **AI disclaimer.** I wrote what I added to this fork (the Calculator, the Terminal, Subsonic, lyrics on
> streamed tracks, the Games and Emulators folders, apps from the card with their SDK and the NES, GBA and
> SNES apps, and the release workflow) with the help of an AI coding assistant, Claude by Anthropic. They
> build, and I have run parts of them on a computer, but they have had far less testing on real players than
> the rest of Sonix Player. Flash at your own risk, and keep HiBy's original firmware so you can go back.

## What I added

- **Calculator** in More, with a scientific mode.
- **Terminal** in More: a shell in colour, with text size and screen width you can change, and a row of
  keys for Tab completion, history, arrows and `| / ~ -`. Programs in `Terminal/bin` on the card run from
  it and from an ADB shell.
- **Games** in More: Flappy Bird, Snake, 2048 and Breakout. **Emulators** next to it: the Game Boy, and
  **apps from the card**, such as NES, GBA and SNES.
- **Subsonic** under Streaming: all your songs kept on the card, with server stars and play reporting.
- **Lyrics on streamed tracks**, from the lyrics in the file.
- **Firmware builds on GitHub**: a workflow that builds both images and publishes them as a release.

## Install

1. Download the firmware for your player from the
   **[latest release](https://github.com/Simplyph0/sonix-plus/releases/latest)**: `r3proii.upt` for the
   R3 Pro II, `r1.upt` for the R1. **Never the other one**: the update does not check which player it is given.
2. Charge the player **above 30%**.
3. Copy the `.upt` to the **root of the microSD card** and put the card in the player.
4. Start the update, from the stock firmware (its firmware update from the microSD card) or from Sonix Player
   (Settings > System > Update firmware > From SD card).
5. Let it flash: "Upgrading...", then "Succeeded", and it reboots by itself.

**Going back to HiBy's firmware:** flash the original from HiBy's website the same way.

> *Via internet* on Sonix Player's update page looks at the releases of the repository named in
> `system-info.json` (`ota-repo`): `Simplyph0/sonix-plus` in images built from this repository. It offers a
> release whose tag is newer than the installed `OS_version`, which is still 1.3.0, so a release has to be
> tagged above that to be offered.

## Games, emulators and apps

**More** holds **Games** and **Emulators**. Games: Flappy Bird, Snake (swipe to steer), 2048 (swipe to slide)
and Breakout (slide a finger, tap to serve), each with a best score. Emulators: the Game Boy is built in (ROMs
in `Games/GB` and `Games/GBC`). The rest are **apps**: programs on the card that the player lists as tiles and
gives the screen, the touch panel and the sound while they run.

| App | Games go in | Get it |
| --- | --- | --- |
| [NES](https://github.com/Simplyph0/sonix-app-nes) | `Games/NES` | build it |
| [GBA](https://github.com/Simplyph0/sonix-app-gba) | `Games/GBA` | build it |
| [SNES](https://github.com/Simplyph0/sonix-app-snes) | `Games/SNES` | build it yourself: Snes9x is non-commercial, so no program is offered |

1. Open **More > Games** or **More > Emulators** once with a card in. The player makes `Apps/` on the card.
2. Copy an app's folder (for example `NES`) into `Apps/`. Delete the folder to remove it.
3. Put games in the folder named above. The player makes it when you first open the app.
4. Open the app and tap a game: the game is on top and a pad below. **SAVE** and **LOAD** keep one state per
   game, **EXIT** (hold it a moment) leaves, and battery saves go to `Saves/<system>/` on the card.

Apps run as root: install only ones you trust. To write one, see [APPS.md](APPS.md) and
[sonix-app-sdk](https://github.com/Simplyph0/sonix-app-sdk). To build the ones above, each repo has a Makefile
(`make fetch`, then `make package CROSS=<toolchain prefix>`) and needs the MIPS toolchain that `make target`
builds ([BUILDING.md](BUILDING.md)).

## Your own programs in the Terminal

The player's filesystem is read-only, so programs for the Terminal (and `adb shell`) live on the card:

1. Tap "Build number" five times in Settings > System > About, open Developer options and switch on
   **Terminal binaries**. That makes `Terminal/bin` (programs), `Terminal/lib` (their shared libraries) and an
   optional `Terminal/font.ttf` on the card.
2. Copy a program into `bin`. It is first on the PATH, so a file called `fastfetch` starts when you type it.

A program has to be **32-bit little-endian MIPS (mipsel, MIPS32r2)**, linked statically or against **glibc 2.22
or older**. The toolchain from `make target` makes them:
`rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -O2 -march=mips32r2 -static hello.c -o hello`. Programs that
draw the whole screen and read single keys (vi, top) do not work: the Terminal sends a line at a time.

## Supported players

One binary runs on both. It reads which player it is on from
`system-info.json` at startup, and the firmware packer writes a different one
into each image.

| | HiBy R3 Pro II | HiBy R1 |
|---|---|---|
| panel | 480x720 | 480x800 |
| DAC | two Cirrus Logic CS43198 | one Cirrus Logic CS43131 |
| headphone outputs | 3.5 mm, 4.4 mm balanced | 3.5 mm |
| DAC controls | digital filters, DRE, NOS | digital filters, NOS |
| touch | Goodix gt9xx, open-source driver, five fingers | Hynitron CST8xx, open-source driver, two fingers |
| charger | MP2731 | the AXP2101 PMIC |
| kernel | custom 4.4.94, see [hiby-custom-kernel](https://github.com/Jepl4r/hiby-custom-kernel) | the same |
| double tap to wake | yes | no |
| buttons | volume on the left flank, playback on the right | all on the right flank, one skip key |
| firmware image | `r3proii.upt` | `r1.upt` |

## Building

`make host` builds a simulator that draws the real interface in an SDL window; `make target` builds the
player for the device; `sonix-packer/sonix_firmware_packer.sh` turns a stock HiBy firmware and that binary into
an image. [BUILDING.md](BUILDING.md) has the details: dependencies, the resource tree, the packer, streaming
keys, system libraries, generated files and the source tree.

## Special thanks to:
[@Tartarus6](https://github.com/Tartarus6)

[@noisetta](https://github.com/noisetta)

[@endgame47](https://github.com/endgame47)

[@hkhrithik007](https://github.com/hkhrithik007)

and all the members of this fantastic community!!
