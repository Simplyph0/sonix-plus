#ifndef GUI_SNAKE_H
#define GUI_SNAKE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Snake: swipe anywhere on the field to turn, eat the red apples, do not touch a
// wall or yourself. The snake speeds up as it grows. The best score is kept in
// the config, [snake] best.
//
// Drawn into a canvas (games/gamecanvas.h) one cell at a time: a step changes
// four cells, so that is all that is repainted. The field is 24 pixel cells, 20
// across the panel and as many down as fit under the heading.
extern lv_obj_t *snake_screen;

void snake_init(gui_config_t *cfg);

#endif /* GUI_SNAKE_H */
