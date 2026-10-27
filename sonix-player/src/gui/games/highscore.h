#ifndef GUI_HIGHSCORE_H
#define GUI_HIGHSCORE_H

#include <stdbool.h>

// What the games remember: best scores, best times, win counts. They are kept on
// the card, in Saves/Games/scores.ini, so they travel with the games and survive
// a firmware update or a factory reset, and in the player's own settings while
// there is no card or it cannot be written.
//
// A name is "snake", or "minesweeper.easy" when a game keeps several; a value is a
// whole number (a time is in seconds). A value that was never set reads as 0.

// Tells the module where to find the card's root: the address of the setting that
// holds it, read each time, because the card can come in after the player started.
// Called once, from gui.c.
void highscore_init(const char *const *sd_root);

long highscore_get(const char *name);

// Stores `value` unconditionally.
void highscore_set(const char *name, long value);

// Keeps `value` when it beats what is stored: bigger is better, or smaller when
// `lower_is_better` (a time: the best of 0 means none yet). Says whether it did.
bool highscore_submit(const char *name, long value, bool lower_is_better);

#endif /* GUI_HIGHSCORE_H */
