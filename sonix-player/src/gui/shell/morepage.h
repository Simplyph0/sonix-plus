#ifndef GUI_MOREPAGE_H
#define GUI_MOREPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The More page: the sixth tile of the main menu.
//
// The main menu has six places and not one more -- a 2x3 grid filling the
// screen -- so the sixth cannot be a destination: it has to be the door to
// everything that does not fit. It holds the DAC, and whatever comes next
// goes there too.
//
// It is a section page like Music, Wireless and Streaming: same grid, same
// tiles, same size.

extern lv_obj_t *morepage_screen;

// The two folders inside More: Games (the games of the player and the apps of the
// card that say they are games) and Emulators. Each is a page of tiles of its
// own, opened from a tile here.
extern lv_obj_t *gamespage_screen;
extern lv_obj_t *emulatorspage_screen;

void morepage_init(gui_config_t *cfg);

#endif /* GUI_MOREPAGE_H */
