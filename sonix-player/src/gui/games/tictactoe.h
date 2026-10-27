#ifndef GUI_TICTACTOE_H
#define GUI_TICTACTOE_H

#include "src/gui/games/gamekit.h"
#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Tic-Tac-Toe against the player: X is yours, tap a square to put it down, three in a
// line wins. Three levels: Easy often plays at random, Medium takes a win and blocks
// yours, Hard never loses. Who starts alternates. Wins, draws and losses of each
// level are kept on the card (highscore.h).
extern lv_obj_t *tictactoe_screen;
extern const gk_def_t tictactoe_def;

void tictactoe_init(gui_config_t *cfg);

#endif /* GUI_TICTACTOE_H */
