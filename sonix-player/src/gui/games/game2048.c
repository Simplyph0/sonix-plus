#include "game2048.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/games/gamekit.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *game2048_screen;

#define N 4
#define GAP 10
#define SLIDE_MS 110
#define POP_MS 130
#define SWIPE_PX 36
#define WIN_VALUE 2048

typedef struct {
	lv_obj_t *obj;
	lv_obj_t *label;
	int value;
	int row;
	int col;
	int to_row; // where it is going
	int to_col;
	bool dies;	// slides under another tile and goes
	bool grows; // the tile it slides under: doubles when the slide ends
} tile_t;

static gui_config_t *g_cfg;
static lv_obj_t *board;
static lv_obj_t *score_label;
static lv_obj_t *best_label;
static lv_obj_t *message;
static gk_ui_t ui;
static bool best_dirty; // the best has grown and is not on the card yet
static int cell_size;
static tile_t tiles[N * N];
static int tile_count;
static int grid[N][N]; // index into tiles[], or -1
static int score;
static int best;
static bool animating;
static bool won;
static bool over;
static bool win_announced;
static int pending_anims;
static int origin_x;
static int origin_y;

// ---------------------------------------------------------------------------
// Tile looks
// ---------------------------------------------------------------------------

typedef struct {
	int value;
	uint32_t fill;
	uint32_t ink;
} tile_style_t;

static const tile_style_t TILE_STYLES[] = {
	{2, 0xeee4da, 0x776e65},	{4, 0xede0c8, 0x776e65},	{8, 0xf2b179, 0xf9f6f2},	{16, 0xf59563, 0xf9f6f2},
	{32, 0xf67c5f, 0xf9f6f2},	{64, 0xf65e3b, 0xf9f6f2},	{128, 0xedcf72, 0xf9f6f2},	{256, 0xedcc61, 0xf9f6f2},
	{512, 0xedc850, 0xf9f6f2},	{1024, 0xedc53f, 0xf9f6f2}, {2048, 0xedc22e, 0xf9f6f2},
};
#define STYLE_COUNT ((int)(sizeof(TILE_STYLES) / sizeof(TILE_STYLES[0])))

static void style_tile(tile_t *t) {
	uint32_t fill = 0x3c3a32;
	uint32_t ink = 0xf9f6f2;
	for (int i = 0; i < STYLE_COUNT; i++) {
		if (TILE_STYLES[i].value == t->value) {
			fill = TILE_STYLES[i].fill;
			ink = TILE_STYLES[i].ink;
			break;
		}
	}
	lv_obj_set_style_bg_color(t->obj, lv_color_hex(fill), 0);
	lv_obj_set_style_text_color(t->label, lv_color_hex(ink), 0);

	char text[16];
	snprintf(text, sizeof(text), "%d", t->value);
	lv_label_set_text(t->label, text);
	size_t digits = strlen(text);
	const lv_font_t *font = digits <= 2 ? &font_ui_64_bold : digits == 3 ? &font_ui_36_bold : digits == 4 ? &font_ui_32 : &font_ui_24;
	lv_obj_set_style_text_font(t->label, font, 0);
	lv_obj_center(t->label);
}

static int tile_x(int col) { return GAP + col * (cell_size + GAP); }
static int tile_y(int row) { return GAP + row * (cell_size + GAP); }

// ---------------------------------------------------------------------------
// Animation helpers
// ---------------------------------------------------------------------------

static void anim_x_cb(void *obj, int32_t v) { lv_obj_set_x(obj, v); }
static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_y(obj, v); }
static void anim_scale_cb(void *obj, int32_t v) { lv_obj_set_style_transform_scale(obj, v, 0); }

static void start_anim(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int from, int to, uint32_t ms, lv_anim_completed_cb_t done) {
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, cb);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	if (done) {
		lv_anim_set_completed_cb(&a, done);
	}
	lv_anim_start(&a);
}

// A tile grows a little and settles: new on the board, or doubled.
static void pop(lv_obj_t *obj, int from_scale) {
	lv_obj_set_style_transform_pivot_x(obj, cell_size / 2, 0);
	lv_obj_set_style_transform_pivot_y(obj, cell_size / 2, 0);
	start_anim(obj, anim_scale_cb, from_scale, 256, POP_MS, NULL);
}

// ---------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------

static int add_tile(int row, int col, int value) {
	if (tile_count >= N * N) {
		return -1;
	}
	int id = tile_count++;
	tile_t *t = &tiles[id];
	memset(t, 0, sizeof(*t));
	t->value = value;
	t->row = t->to_row = row;
	t->col = t->to_col = col;
	t->obj = lv_obj_create(board);
	lv_obj_remove_style_all(t->obj);
	lv_obj_set_size(t->obj, cell_size, cell_size);
	lv_obj_set_pos(t->obj, tile_x(col), tile_y(row));
	lv_obj_set_style_radius(t->obj, 10, 0);
	lv_obj_set_style_bg_opa(t->obj, LV_OPA_COVER, 0);
	lv_obj_set_clickable(t->obj, false);
	t->label = lv_label_create(t->obj);
	style_tile(t);
	grid[row][col] = id;
	return id;
}

static void clear_tiles(void) {
	pending_anims = 0; // the animations go with their tiles, and never report
	animating = false;
	for (int i = 0; i < tile_count; i++) {
		if (tiles[i].obj) {
			lv_anim_delete(tiles[i].obj, NULL);
			lv_obj_delete(tiles[i].obj);
		}
	}
	tile_count = 0;
	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			grid[r][c] = -1;
		}
	}
}

static void spawn(void) {
	int free_cells = 0;
	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			free_cells += grid[r][c] < 0;
		}
	}
	if (free_cells == 0) {
		return;
	}
	int pick = rand() % free_cells;
	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			if (grid[r][c] >= 0 || pick-- != 0) {
				continue;
			}
			int id = add_tile(r, c, rand() % 10 == 0 ? 4 : 2);
			if (id >= 0) {
				pop(tiles[id].obj, 80);
			}
			return;
		}
	}
}

static void update_scores(void) {
	char text[32];
	snprintf(text, sizeof(text), "%d", score);
	lv_label_set_text(score_label, text);
	snprintf(text, sizeof(text), "%d", best);
	lv_label_set_text(best_label, text);
}

// Puts a best that has grown on the card.
static void flush_best(void) {
	if (best_dirty) {
		best_dirty = false;
		highscore_set("game2048", best);
	}
}

static bool can_move(void) {
	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			if (grid[r][c] < 0) {
				return true;
			}
			int v = tiles[grid[r][c]].value;
			if (c + 1 < N && grid[r][c + 1] >= 0 && tiles[grid[r][c + 1]].value == v) {
				return true;
			}
			if (r + 1 < N && grid[r + 1][c] >= 0 && tiles[grid[r + 1][c]].value == v) {
				return true;
			}
		}
	}
	return false;
}

static void show_message(const char *key) {
	lv_label_set_text(message, tr(key));
	lv_obj_set_hidden(message, false);
	lv_obj_align(message, LV_ALIGN_CENTER, 0, 0);
}

static void new_game(void) {
	flush_best();
	clear_tiles();
	score = 0;
	won = false;
	win_announced = false;
	over = false;
	animating = false;
	pending_anims = 0;
	lv_obj_set_hidden(message, true);
	spawn();
	spawn();
	update_scores();
}

// Called when every tile has stopped: the ones that slid under another are gone,
// the other doubled, and a new tile comes in.
static void finish_move(void) {
	int merged = 0;
	// The tiles that were swallowed go; what survives is compacted so that tile
	// indices keep meaning something.
	for (int i = 0; i < tile_count; i++) {
		if (tiles[i].dies) {
			lv_obj_delete(tiles[i].obj);
			tiles[i].obj = NULL;
		}
	}
	tile_t kept[N * N];
	int n = 0;
	for (int i = 0; i < tile_count; i++) {
		if (tiles[i].obj) {
			kept[n++] = tiles[i];
		}
	}
	memcpy(tiles, kept, sizeof(kept[0]) * (size_t)n);
	tile_count = n;
	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			grid[r][c] = -1;
		}
	}
	for (int i = 0; i < tile_count; i++) {
		tile_t *t = &tiles[i];
		t->row = t->to_row;
		t->col = t->to_col;
		grid[t->row][t->col] = i;
		if (t->grows) {
			t->grows = false;
			merged++;
			t->value *= 2;
			score += t->value;
			style_tile(t);
			pop(t->obj, 300);
			if (t->value == WIN_VALUE && !won) {
				won = true;
			}
		}
	}
	if (merged > 0) {
		gamesound_play(SND_MERGE);
	}
	if (score > best) {
		best = score;
		best_dirty = true; // written when the game ends or is left, not at every move
	}
	update_scores();
	spawn();
	animating = false;

	if (!can_move()) {
		over = true;
		flush_best();
		gamesound_play(SND_LOSE);
		show_message("game_over");
	} else if (won && !win_announced) {
		// Said once, and then the game goes on.
		win_announced = true;
		gamesound_play(SND_WIN);
		show_message("game2048_win");
	}
}

static void slide_done_cb(lv_anim_t *a) {
	(void)a;
	if (--pending_anims <= 0) {
		finish_move();
	}
}

typedef enum {
	MOVE_UP,
	MOVE_RIGHT,
	MOVE_DOWN,
	MOVE_LEFT,
} move_t;

// The cell `i` places along line `line`, counted from the side the tiles are
// pushed towards.
static void line_cell(move_t m, int line, int i, int *row, int *col) {
	switch (m) {
	case MOVE_LEFT:
		*row = line;
		*col = i;
		break;
	case MOVE_RIGHT:
		*row = line;
		*col = N - 1 - i;
		break;
	case MOVE_UP:
		*row = i;
		*col = line;
		break;
	case MOVE_DOWN:
		*row = N - 1 - i;
		*col = line;
		break;
	}
}

static void do_move(move_t m) {
	if (animating || over) {
		return;
	}
	bool changed = false;
	for (int i = 0; i < tile_count; i++) {
		tiles[i].dies = tiles[i].grows = false;
		tiles[i].to_row = tiles[i].row;
		tiles[i].to_col = tiles[i].col;
	}

	for (int line = 0; line < N; line++) {
		int place = 0;		 // the next free place along the line
		int last_id = -1;	 // the tile at `place - 1`, if it can still take a merge
		bool last_open = false;
		for (int i = 0; i < N; i++) {
			int r;
			int c;
			line_cell(m, line, i, &r, &c);
			int id = grid[r][c];
			if (id < 0) {
				continue;
			}
			tile_t *t = &tiles[id];
			int dest_row;
			int dest_col;
			if (last_open && last_id >= 0 && tiles[last_id].value == t->value) {
				// Merges into the one before it.
				line_cell(m, line, place - 1, &dest_row, &dest_col);
				t->dies = true;
				tiles[last_id].grows = true;
				last_open = false;
			} else {
				line_cell(m, line, place, &dest_row, &dest_col);
				last_id = id;
				last_open = true;
				place++;
			}
			t->to_row = dest_row;
			t->to_col = dest_col;
			if (dest_row != t->row || dest_col != t->col) {
				changed = true;
			}
		}
	}
	if (!changed) {
		return; // nothing moves: not a move
	}

	animating = true;
	gamesound_play(SND_TICK);
	pending_anims = 0;
	for (int i = 0; i < tile_count; i++) {
		tile_t *t = &tiles[i];
		if (t->to_col != t->col) {
			pending_anims++;
			start_anim(t->obj, anim_x_cb, tile_x(t->col), tile_x(t->to_col), SLIDE_MS, slide_done_cb);
		}
		if (t->to_row != t->row) {
			pending_anims++;
			start_anim(t->obj, anim_y_cb, tile_y(t->row), tile_y(t->to_row), SLIDE_MS, slide_done_cb);
		}
	}
	if (pending_anims == 0) {
		finish_move();
	}
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------

static void get_point(int *x, int *y) {
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	*x = p.x;
	*y = p.y;
}

static void pressed_cb(lv_event_t *e) {
	(void)e;
	get_point(&origin_x, &origin_y);
}

static void released_cb(lv_event_t *e) {
	(void)e;
	int x;
	int y;
	get_point(&x, &y);
	int dx = x - origin_x;
	int dy = y - origin_y;

	if (over) {
		if (abs(dx) < SWIPE_PX && abs(dy) < SWIPE_PX) {
			new_game();
		}
		return;
	}
	if (!lv_obj_is_hidden(message)) {
		lv_obj_set_hidden(message, true); // the win notice: tap it away
		if (abs(dx) < SWIPE_PX && abs(dy) < SWIPE_PX) {
			return;
		}
	}
	if (abs(dx) < SWIPE_PX && abs(dy) < SWIPE_PX) {
		return;
	}
	if (abs(dx) >= abs(dy)) {
		do_move(dx > 0 ? MOVE_RIGHT : MOVE_LEFT);
	} else {
		do_move(dy > 0 ? MOVE_DOWN : MOVE_UP);
	}
}

static void new_game_cb(lv_event_t *e) {
	(void)e;
	new_game();
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

static lv_obj_t *score_box(lv_obj_t *parent, const char *caption_key, int x, lv_obj_t **value_out) {
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_add_style(box, &theme_style_card, 0);
	lv_obj_set_size(box, 100, 56);
	lv_obj_set_pos(box, x, (int)g_cfg->top_bar_height + (int)g_cfg->padding);
	lv_obj_set_style_radius(box, 10, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 4, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_clickable(box, false);

	lv_obj_t *caption = lv_label_create(box);
	lv_label_set_text(caption, tr(caption_key));
	lv_obj_add_style(caption, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(caption, &font_ui_14, 0);
	lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 0);

	lv_obj_t *value = lv_label_create(box);
	lv_label_set_text(value, "0");
	lv_obj_add_style(value, &theme_style_text, 0);
	lv_obj_set_style_text_font(value, &font_ui_24_bold, 0);
	lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, 0);
	*value_out = value;
	return box;
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	best = (int)highscore_get("game2048");
	best_dirty = false;
	srand((unsigned)time(NULL));
	gamesound_open();
	new_game();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	flush_best();
	clear_tiles();
	lv_obj_set_hidden(message, true);
	gamesound_close();
	gk_ui_reset(&ui);
}

// The pause menu: nothing here runs on a timer, so there is nothing to stop; the menu
// covers the board and takes the touches.
static void restart_cb(void *user) {
	(void)user;
	new_game();
}

void game2048_init(gui_config_t *cfg) {
	g_cfg = cfg;
	int margin = (int)cfg->padding;
	int side = (int)cfg->screen_width - 2 * margin;
	cell_size = (side - (N + 1) * GAP) / N;
	side = N * cell_size + (N + 1) * GAP;

	lv_obj_add_style(game2048_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(game2048_screen, false);
	lv_obj_t *title = settingsrow_title(game2048_screen, cfg, "game2048");
	settingsrow_title_corner_slots(title, cfg, 3);

	// The pause button has the corner; the two scores sit to the left of it.
	int right = (int)cfg->screen_width - margin - 62;
	score_box(game2048_screen, "game_best", right - 100, &best_label);
	score_box(game2048_screen, "game_score", right - 100 - 8 - 100, &score_label);

	int top = settingsrow_content_top(cfg) + 6;
	board = lv_obj_create(game2048_screen);
	lv_obj_remove_style_all(board);
	lv_obj_set_pos(board, margin + ((int)cfg->screen_width - 2 * margin - side) / 2, top);
	lv_obj_set_size(board, side, side);
	lv_obj_set_style_bg_color(board, lv_color_hex(0xbbada0), 0);
	lv_obj_set_style_bg_opa(board, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(board, 14, 0);
	lv_obj_set_scrollable(board, false);
	lv_obj_set_clickable(board, false);

	// The empty places under the tiles.
	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			lv_obj_t *slot = lv_obj_create(board);
			lv_obj_remove_style_all(slot);
			lv_obj_set_size(slot, cell_size, cell_size);
			lv_obj_set_pos(slot, tile_x(c), tile_y(r));
			lv_obj_set_style_radius(slot, 10, 0);
			lv_obj_set_style_bg_color(slot, lv_color_hex(0xcdc1b4), 0);
			lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
			lv_obj_set_clickable(slot, false);
		}
	}

	// A button under the board for a new game.
	lv_obj_t *button = lv_btn_create(game2048_screen);
	lv_obj_set_size(button, 220, 60);
	lv_obj_set_pos(button, ((int)cfg->screen_width - 220) / 2, top + side + 24);
	lv_obj_add_style(button, &theme_style_card, 0);
	lv_obj_set_style_radius(button, 14, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, new_game_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *button_label = lv_label_create(button);
	lv_label_set_text(button_label, tr("game_new"));
	lv_obj_add_style(button_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(button_label, &font_ui_24_bold, 0);
	lv_obj_center(button_label);

	message = lv_label_create(game2048_screen);
	lv_obj_set_style_text_font(message, &font_ui_32, 0);
	lv_obj_set_style_text_color(message, lv_color_white(), 0);
	lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_bg_color(message, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(message, LV_OPA_70, 0);
	lv_obj_set_style_pad_all(message, 20, 0);
	lv_obj_set_style_radius(message, 16, 0);
	lv_obj_set_clickable(message, false);
	lv_obj_set_hidden(message, true);

	gk_ui_attach_pause(&ui, game2048_screen, cfg, NULL, restart_cb, NULL);

	lv_obj_add_event_cb(game2048_screen, pressed_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(game2048_screen, released_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(game2048_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(game2048_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
