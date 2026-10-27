#ifndef GUI_GAME2048_H
#define GUI_GAME2048_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// 2048: swipe to slide every tile as far as it goes; two equal tiles that meet
// become one of twice the value. Reach 2048, and carry on if you like. The best
// score is kept in the config, [game2048] best.
//
// The tiles are LVGL objects, slid into place with animations: sixteen of them at
// most, moving a few times a second.
extern lv_obj_t *game2048_screen;

void game2048_init(gui_config_t *cfg);

#endif /* GUI_GAME2048_H */
