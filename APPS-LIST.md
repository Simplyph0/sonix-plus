# Apps for Sonix Plus

A list of the apps that exist for the player: programs you copy into `Apps/` on the microSD card and that
show up as tiles in **More**, **Games** or **Emulators**. One line and one link for each. How the folder
works, and how to write one, is in [APPS.md](APPS.md); the library to write one with is
[sonix-app-sdk](https://github.com/Simplyph0/sonix-app-sdk).

Apps run as root, so install only ones you trust.

## Emulators

| App | One line | Licence | Get it |
| --- | --- | --- | --- |
| [NES](https://github.com/Simplyph0/sonix-app-nes) | NES and Famicom games, FCEUmm on the SDK frontend. Games in `Games/NES`. | GPL-2.0 (core), MIT (rest) | build it |
| [GBA](https://github.com/Simplyph0/sonix-app-gba) | Game Boy Advance, gpSP with its MIPS recompiler (the interpreter takes over if the recompiler fails). Games in `Games/GBA`. | GPL-2.0 (core), MIT (rest) | build it |
| [SNES](https://github.com/Simplyph0/sonix-app-snes) | Super Nintendo, Snes9x 2005. Games in `Games/SNES`. A build recipe only: Snes9x is non-commercial, so no program is offered. | non-commercial (core), MIT (recipe) | build it yourself |

The **Game Boy and Game Boy Color** emulator is built into the player (**More > Emulators**, games in
`Games/GB` and `Games/GBC`), and is not an app.

## For developers

| App | One line | Get it |
| --- | --- | --- |
| [Hello](https://github.com/Simplyph0/sonix-app-sdk/tree/main/examples/hello) | The smallest app: draws, reads touches, makes a tone. Start from this one. | in the SDK |
| [FrontendTest](https://github.com/Simplyph0/sonix-app-sdk/tree/main/frontend/testcore) | The emulator frontend with a test core instead of a console: checks the screen, touch and sound. | in the SDK |

## Built into the player (not apps)

Flappy Bird, Snake, 2048, Breakout, Blocks, Mines, Rally, Sudoku, Words and Tic-Tac-Toe in **More > Games**; the
Game Boy in **More > Emulators**; the Calculator and the Terminal in **More**. They come with
[the firmware](https://github.com/Simplyph0/sonix-plus/releases/latest).

## Putting an app on this list

Open an issue or a pull request on this repository with the app's name, one line, its licence and a link. The
app should be its own repository with a `manifest.ini` in the format of [APPS.md](APPS.md), say how to build it
or where its program can be had, and name the licence of anything it includes. Programs that are not mine are
listed, not hosted.
