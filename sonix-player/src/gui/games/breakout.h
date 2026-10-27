#ifndef GUI_BREAKOUT_H
#define GUI_BREAKOUT_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Breakout: slide a finger along the screen to move the paddle, tap to serve the
// ball, break every brick. The ball speeds up a little with every cleared wall and
// with the higher bricks; three lives. The best score is kept in the config,
// [breakout] best.
//
// Drawn into a canvas (games/gamecanvas.h) and stepped by time, not by frame, so a
// late tick does not slow the ball down. Only the small areas around the ball and
// the paddle are repainted, sixty times a second.
extern lv_obj_t *breakout_screen;

void breakout_init(gui_config_t *cfg);

#endif /* GUI_BREAKOUT_H */
