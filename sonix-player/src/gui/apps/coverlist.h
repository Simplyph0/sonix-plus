#ifndef GUI_COVERLIST_H
#define GUI_COVERLIST_H

#include <stdbool.h>
#include <stddef.h>

#include "lvgl/lvgl.h"

// Rows for a list of games that can show a cover: a picture with the same name as the game,
// in the same folder (Tetris.gb and Tetris.png, .jpg or .jpeg), on the left of its name.
// A game with none shows its first letter in the same place. Used by the lists of the
// emulator apps and of the built-in Game Boy.
//
// The pictures are decoded one at a time, a few milliseconds apart, so a list of a hundred
// games opens at once and fills in from the top. At most COVERLIST_MAX of them are kept,
// 12 kB each; the rest of the games keep their letter.
//
// A list that has no cover anywhere is better built with plain rows (settingsrow_action),
// so that is what coverlist_has_cover() is for.

#define COVERLIST_MAX 60

// What a list shows for a game: its file name without the extension, minus what is in
// brackets and parentheses ("Pokemon - Gold Version (USA, Europe) [!]" is shown as
// "Pokemon - Gold Version"), underscores as spaces.
void coverlist_display_name(const char *name, char *out, size_t size);

// Whether a picture belongs to the game at `rom_path`.
bool coverlist_has_cover(const char *rom_path);

// Starts a list: forgets the covers of the last one and frees their memory. Call it after the
// rows of the old list have been deleted.
void coverlist_begin(void);

// Adds a row to `parent`. `cb` is called with `user` when the row is tapped (and not at the end of a
// back swipe: that check is the caller's). Returns the row.
lv_obj_t *coverlist_row(lv_obj_t *parent, const char *label, const char *rom_path, lv_event_cb_t cb, void *user);

// Frees the pictures and stops decoding. The rows have to be gone, or about to be.
void coverlist_end(void);

#endif /* GUI_COVERLIST_H */
