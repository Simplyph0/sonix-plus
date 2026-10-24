#ifndef SUBSONICPAGE_H
#define SUBSONICPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Subsonic pages: the main one, sign in, search, settings and the results
// list (a single list, reused for artists, albums, tracks and playlists).
void subsonicpage_init(gui_config_t *cfg);

// The main page, for the streaming grid.
extern lv_obj_t *subsonic_screen;

#endif /* SUBSONICPAGE_H */
