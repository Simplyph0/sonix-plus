#ifndef APPLAUNCH_H
#define APPLAUNCH_H

#include <stdbool.h>

#include "src/system/apps/apps.h"

// Starting an app from the card, and what it is given while it runs.
//
// The app is a child process in a group of its own. The player stops music
// playback, takes its own hands off the screen and the touch panel
// (panel_app_begin, system/core/panel.h) and keeps the screen from blanking,
// then runs the program with its folder as the working directory and these in
// the environment:
//
//     SONIX_API        1
//     SONIX_APP_DIR    the app's own folder
//     SONIX_SD         the root of the card
//     SONIX_FB         the framebuffer device, /dev/fb0
//     SONIX_WIDTH      the panel's size in pixels...
//     SONIX_HEIGHT
//     SONIX_ROTATED    1 when the picture is turned 180 degrees, else 0
//     SONIX_TOUCH      the touch panel's evdev node
//     SONIX_AUDIO_FD   a pipe the app writes its sound into, see below
//     SONIX_ROM        the file the user picked, for an app that lists files
//                      (it is also the program's first argument)
//
// Sound goes through the player, not to the sound card directly, so the output
// the user chose, the volume and Bluetooth all keep working. The app writes a
// header and then frames to SONIX_AUDIO_FD:
//
//     char magic[4] = "SNXA"; uint32_t rate; uint32_t channels;  (little-endian)
//     then signed 16-bit interleaved samples, as fast as the output takes them
//
// The pipe is small and the write blocks, so the sound card paces the app. An
// app with no sound simply never writes.
//
// The app's output and errors go to the player's log.

#define APPS_AUDIO_MAGIC "SNXA"

typedef struct {
	char magic[4];
	uint32_t rate;
	uint32_t channels;
} apps_audio_header_t;

// Starts `app`, with `rom` (a path, or NULL) as what it is to open. Returns at
// once; false when nothing could be started. `on_exit` is called on the
// interface thread when the app has ended and the panel is back, with `user`.
// Only one app runs at a time.
bool apps_launch(const app_t *app, const char *rom, void (*on_exit)(void *user), void *user);

bool apps_running(void);

// Ends the running app, without asking: for the player going down.
void apps_kill(void);

#endif /* APPLAUNCH_H */
