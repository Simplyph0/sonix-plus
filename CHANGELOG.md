# Changelog

What changed in each release of this fork, newest first. The release page shows
the section for its own tag, so write the section here before pushing the tag
(see `.github/workflows/release-firmware.yml`).

## Unreleased

- **Games** folder in More: Flappy Bird (moved here), **Snake**, **2048** and **Breakout**.
- **Emulators** folder in More: the Game Boy (moved here).
- **Apps from the card**: programs in `Apps/<name>/` with a `manifest.ini` appear as
  tiles in More, Games or Emulators and are given the screen, the touch panel and the
  sound while they run ([APPS.md](APPS.md)). Apps that open files get a list of the
  files in the folder they name.
- **Updates from this repository**: the player's "Via internet" update now reads `Simplyph0/sonix-plus`, and
  the release workflow writes the tag into the images as `OS_version`.
- **NES, GBA and SNES** as apps from the card, each in a repository of its own, on a
  shared libretro frontend ([sonix-app-sdk](https://github.com/Simplyph0/sonix-app-sdk)).

## v1.0 - 2026-10-24

My first release of this fork, on top of Sonix Player 1.3.0.

- **Calculator** in More, with a scientific mode.
- **Terminal** in More: a shell in colour with adjustable text size and width, in
  DejaVu Sans Mono. Programs in `Terminal/bin` on the card run from it and from
  an ADB shell (Developer options > Terminal binaries).
- **Terminal keys**: Tab completion, command history, arrows and `| / ~ -` above
  the keyboard.
- **Subsonic** under Streaming: all your songs kept on the card (All tracks,
  Artists, Albums, Genres, Starred, Recently added, search, A-Z bar), the server's
  playlists, server stars and play reporting, and a long-press menu on songs.
- **Lyrics on streamed tracks**, from the lyrics in the file.
- Firmware images are now built and published by GitHub Actions.
