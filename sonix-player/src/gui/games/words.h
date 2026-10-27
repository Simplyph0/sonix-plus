#ifndef GUI_WORDS_H
#define GUI_WORDS_H

#include "src/gui/games/gamekit.h"
#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Words: a five letter word is hidden; six guesses to find it. After each guess a
// letter is green when it is in the right place, yellow when the word has it
// elsewhere, grey when it has none. The word list is in the player (wordlist.h),
// nothing is fetched; a guess that is not in it is refused once and taken if it is
// sent again. The streak of won games, and the longest, are kept on the card
// (highscore.h).
extern lv_obj_t *words_screen;
extern const gk_def_t words_def;

void words_init(gui_config_t *cfg);

#endif /* GUI_WORDS_H */
