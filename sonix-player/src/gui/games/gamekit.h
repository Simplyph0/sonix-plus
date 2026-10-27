#ifndef GUI_GAMEKIT_H
#define GUI_GAMEKIT_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/gui/games/gamecanvas.h"
#include "src/gui/games/gamesound.h"
#include "src/gui/games/highscore.h"
#include "src/gui/shell/gui.h"

// What every game page has in common: the heading with the score at its right, a
// pause button in the corner, the pause menu (resume, new game, sound on or off),
// and the message that lies over the field ("Tap to play", "Game over").
//
// Two ways to use it. The first three games (Snake, 2048, Breakout) build their own
// page and take the pieces: gk_ui_build() and the functions after it. The newer
// ones are described by a gk_def_t and run by gk_game_init(): the runner makes the
// page, the canvas under the heading, the timer and the touch events, and the
// game only decides what a tap does and what a tick changes.

// ---------------------------------------------------------------------------
// The pieces
// ---------------------------------------------------------------------------

typedef struct gk_ui {
	lv_obj_t *screen;
	gui_config_t *cfg;
	lv_obj_t *score_label;
	lv_obj_t *message;
	lv_obj_t *pause_button;
	lv_obj_t *menu;
	lv_obj_t *sound_label;
	void (*on_pause)(bool paused, void *user); // the menu opened (true) or was closed (false)
	void (*on_restart)(void *user);			   // "New game" in the menu
	void *user;
	bool paused;
} gk_ui_t;

// Builds the heading (`title_id` is a language key), the score label, the message,
// the pause button and the pause menu on `screen`. `on_pause` and `on_restart` may be
// NULL.
void gk_ui_build(gk_ui_t *ui, lv_obj_t *screen, gui_config_t *cfg, const char *title_id,
				 void (*on_pause)(bool paused, void *user), void (*on_restart)(void *user), void *user);

// Only the pause button and its menu, for a page that has its own heading and score
// (2048). Call it last, so the menu lies over everything else.
void gk_ui_attach_pause(gk_ui_t *ui, lv_obj_t *screen, gui_config_t *cfg, void (*on_pause)(bool paused, void *user),
						void (*on_restart)(void *user), void *user);

// The score, in two small lines beside the pause button. NULL clears a line.
void gk_ui_score(gk_ui_t *ui, const char *line1, const char *line2);

// "Score 12" over "Best 40", the way most games show it.
void gk_ui_score_best(gk_ui_t *ui, long score, long best);

// The message over the field: `text` may have several lines. `y_offset` moves it
// down from the middle of the screen.
void gk_ui_message(gk_ui_t *ui, const char *text, int y_offset);
void gk_ui_message_hide(gk_ui_t *ui);

// "Tap to play", and under it "Best 120" when there is a best.
void gk_ui_message_ready(gk_ui_t *ui, const char *prompt_key, const char *hint_key, long best, int y_offset);

// "Game over", the score, and "Tap to play again"; "New best!" when it is one.
void gk_ui_message_over(gk_ui_t *ui, long score, bool new_best, int y_offset);

// Closes the pause menu without telling the game (the page is being left).
void gk_ui_reset(gk_ui_t *ui);

bool gk_ui_paused(const gk_ui_t *ui);

// ---------------------------------------------------------------------------
// The runner
// ---------------------------------------------------------------------------

typedef struct gk_game gk_game_t;

typedef enum {
	GK_DOWN,
	GK_MOVE,
	GK_UP,
} gk_phase_t;

typedef struct {
	const char *id; // "blocks": the language key of the title, and the name of its scores
	int tick_ms;	// 0 for a game with no timer
	// The page has opened and g->w x g->h is known: set up and start a game.
	void (*open)(gk_game_t *g);
	// The page is being left: free what open() took.
	void (*close)(gk_game_t *g);
	// Every tick_ms while the timer runs (gk_timer) and the game is not paused.
	void (*tick)(gk_game_t *g, uint32_t dt_ms);
	// A finger, in canvas coordinates.
	void (*touch)(gk_game_t *g, gk_phase_t phase, int x, int y);
	// "New game" in the pause menu.
	void (*restart)(gk_game_t *g);
	// The pause menu opened or closed (optional).
	void (*paused)(gk_game_t *g, bool paused);
} gk_def_t;

struct gk_game {
	const gk_def_t *def;
	gui_config_t *cfg;
	lv_obj_t *screen;
	gamecanvas_t canvas;
	int w;
	int h;
	gk_ui_t ui;
	lv_timer_t *timer;
	uint32_t last_tick;
	bool timer_wanted;
	bool open;
};

// Makes the page on `screen`; the game is opened each time the page is shown.
void gk_game_init(gk_game_t *g, lv_obj_t *screen, gui_config_t *cfg, const gk_def_t *def);

// Starts or stops the game's timer. It also stops while the pause menu is open.
void gk_timer(gk_game_t *g, bool run);

// Repaints a part of the canvas (or all of it) on the next refresh.
static inline void gk_dirty(gk_game_t *g, int x, int y, int w, int h) { gamecanvas_dirty(&g->canvas, x, y, w, h); }
static inline void gk_dirty_all(gk_game_t *g) { gamecanvas_dirty_all(&g->canvas); }

#endif /* GUI_GAMEKIT_H */
