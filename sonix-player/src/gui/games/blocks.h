#ifndef GUI_BLOCKS_H
#define GUI_BLOCKS_H

#include "src/gui/games/gamekit.h"
#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Blocks: falling pieces, the kind that fill a line and clear it. Drag sideways to
// move the piece, down to speed it up, tap to turn it, flick down to drop it. The
// pieces come out of a shuffled bag, a ghost shows where the piece will land, and
// the game speeds up every ten lines. The best score is kept on the card
// (highscore.h).
extern lv_obj_t *blocks_screen;
extern const gk_def_t blocks_def;

void blocks_init(gui_config_t *cfg);

#endif /* GUI_BLOCKS_H */
