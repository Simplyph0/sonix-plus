# Changelog

What changed in each release of this fork, newest first. The release page shows
the section for its own tag, so write the section here before pushing the tag
(see `.github/workflows/release-firmware.yml`).

## v1.4 - 2026-10-27

- **Games** and **Emulators** folders in More: Flappy Bird, **Snake**, **2048** and **Breakout**, and the Game Boy.
- **Apps from the card**: programs in `Apps/<name>/` appear as tiles and get the screen, touch and sound
  ([APPS.md](APPS.md)). NES, GBA and SNES are apps, in their own repositories.
- **Updates** now come from this repository, and each release carries its tag as `OS_version`.

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
