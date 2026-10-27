#ifndef GUI_SUDOKU_H
#define GUI_SUDOKU_H

#include "src/gui/games/gamekit.h"
#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Sudoku: every row, column and 3x3 box holds the digits 1 to 9 once. Each puzzle is made
// when it is asked for and has exactly one answer. Tap a square and then a digit
// (NOTES switches to pencil marks, HINT fills the square for 30 seconds on the clock),
// three levels, and the best time of each is kept on the card (highscore.h).
extern lv_obj_t *sudoku_screen;
extern const gk_def_t sudoku_def;

void sudoku_init(gui_config_t *cfg);

#endif /* GUI_SUDOKU_H */
