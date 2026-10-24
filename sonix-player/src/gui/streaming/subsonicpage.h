#ifndef SUBSONICPAGE_H
#define SUBSONICPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Subsonic pages: the main one, sign in, search, settings and the results
// list (a single list, reused for artists, albums, tracks and playlists).
void subsonicpage_init(gui_config_t *cfg);

// The main page, for the streaming grid.
extern lv_obj_t *subsonic_screen;

// The list page, for callers that need to know where back leads.
extern lv_obj_t *subsonic_list_screen;

// Opens the songs of an album from outside: the player's "show album" when what
// is playing came from the server. False when it cannot be done (not signed in).
bool subsonicpage_open_album(const char *album_id, const char *title);

#endif /* SUBSONICPAGE_H */
