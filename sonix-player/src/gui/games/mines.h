#ifndef GUI_MINES_H
#define GUI_MINES_H

#include "src/gui/games/gamekit.h"
#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Mines: open every square that is not hiding a mine. A number says how many of the
// eight squares around it do. Tap to open, press and hold to flag (or switch the
// bar's DIG to FLAG), tap a number whose flags are all down to open the rest around
// it. The first tap is always safe. Three sizes; the best time of each is kept on
// the card (highscore.h).
extern lv_obj_t *mines_screen;
extern const gk_def_t mines_def;

void mines_init(gui_config_t *cfg);

#endif /* GUI_MINES_H */
