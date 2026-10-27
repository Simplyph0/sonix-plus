#ifndef GUI_RALLY_H
#define GUI_RALLY_H

#include "src/gui/games/gamekit.h"
#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Rally: a ball and two paddles. Slide a finger sideways to move yours, at the bottom;
// the other one belongs to the player's opponent, which gets quicker with every
// goal. A point for each return, five for a goal, three lives. The best score is kept
// on the card (highscore.h).
extern lv_obj_t *rally_screen;
extern const gk_def_t rally_def;

void rally_init(gui_config_t *cfg);

#endif /* GUI_RALLY_H */
