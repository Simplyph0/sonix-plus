#include "sudoku.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *sudoku_screen;

#define N 81
#define BTN_H 54
#define GAP 10
#define HINT_PENALTY_S 30

typedef enum {
	ST_READY,
	ST_PLAY,
	ST_WON,
} state_t;

typedef struct {
	const char *key;
	const char *name_key;
	int givens;
} level_t;

static const char *const BAR_LEVEL[3] = {"EASY", "MEDIUM", "HARD"}; // drawn in the canvas: capitals only

static const level_t LEVELS[3] = {
	{"sudoku.easy", "game_easy", 40},
	{"sudoku.medium", "game_medium", 32},
	{"sudoku.hard", "game_hard", 27},
};

static gk_game_t game_object;
static gk_game_t *game;
static state_t state;
static int level;
static int8_t grid[N];
static uint8_t given[N];
static int8_t solution[N];
static uint16_t notes[N]; // bit d-1: the pencil mark d
static int sel = -1;
static bool notes_mode;
static uint32_t level_armed_until; // a running game asks twice before the level changes and it starts over
static int hints;
static uint32_t clock_ms;
static uint32_t end_ms;
static uint32_t won_at;
static int shown_seconds;
static bool new_best;

// Where things are.
static int cs;
static int gx;
static int gy;
static int pad_y;
static int tool_y;
static int pad_w;

// A finger.
static int down_x;
static int down_y;

static uint16_t col_bg;
static uint16_t col_cell;
static uint16_t col_cell_alt;
static uint16_t col_peer;
static uint16_t col_same;
static uint16_t col_sel;
static uint16_t col_line;
static uint16_t col_line_box;
static uint16_t col_given;
static uint16_t col_mine;
static uint16_t col_bad;
static uint16_t col_note;
static uint16_t col_button;
static uint16_t col_button_on;

// ---------------------------------------------------------------------------
// The puzzle
// ---------------------------------------------------------------------------

static int row_of(int i) { return i / 9; }
static int col_of(int i) { return i % 9; }
static int box_of(int i) { return (row_of(i) / 3) * 3 + col_of(i) / 3; }

typedef struct {
	uint16_t row[9];
	uint16_t col[9];
	uint16_t box[9];
} masks_t;

static void masks_set(masks_t *m, int i, int d, bool on) {
	uint16_t bit = (uint16_t)(1u << (d - 1));
	if (on) {
		m->row[row_of(i)] |= bit;
		m->col[col_of(i)] |= bit;
		m->box[box_of(i)] |= bit;
	} else {
		m->row[row_of(i)] &= (uint16_t)~bit;
		m->col[col_of(i)] &= (uint16_t)~bit;
		m->box[box_of(i)] &= (uint16_t)~bit;
	}
}

static uint16_t candidates(const masks_t *m, int i) {
	return (uint16_t)(~(m->row[row_of(i)] | m->col[col_of(i)] | m->box[box_of(i)]) & 0x1FF);
}

static int popcount(uint16_t v) {
	int n = 0;
	while (v) {
		n += v & 1;
		v >>= 1;
	}
	return n;
}

// Counts the ways to finish `g`, stopping at `limit`. With `random_order` the first one
// found is left in `g` and its digits are tried in a random order, which is how a
// full grid is made.
static int solve(int8_t *g, masks_t *m, int limit, bool random_order) {
	int best_i = -1;
	int best_n = 10;
	uint16_t best_c = 0;
	for (int i = 0; i < N; i++) {
		if (g[i]) {
			continue;
		}
		uint16_t c = candidates(m, i);
		int n = popcount(c);
		if (n < best_n) {
			best_n = n;
			best_i = i;
			best_c = c;
			if (n <= 1) {
				break;
			}
		}
	}
	if (best_i < 0) {
		return 1; // nothing left to fill
	}
	if (best_n == 0) {
		return 0;
	}
	int order[9];
	int count = 0;
	for (int d = 1; d <= 9; d++) {
		if (best_c & (1u << (d - 1))) {
			order[count++] = d;
		}
	}
	if (random_order) {
		for (int k = count - 1; k > 0; k--) {
			int j = rand() % (k + 1);
			int t = order[k];
			order[k] = order[j];
			order[j] = t;
		}
	}
	int found = 0;
	for (int k = 0; k < count && found < limit; k++) {
		g[best_i] = (int8_t)order[k];
		masks_set(m, best_i, order[k], true);
		found += solve(g, m, limit - found, random_order);
		if (found > 0 && random_order) {
			return found; // keep the grid as it is
		}
		masks_set(m, best_i, order[k], false);
		g[best_i] = 0;
	}
	return found;
}

static void masks_of(const int8_t *g, masks_t *m) {
	memset(m, 0, sizeof(*m));
	for (int i = 0; i < N; i++) {
		if (g[i]) {
			masks_set(m, i, g[i], true);
		}
	}
}

static void make_puzzle(void) {
	int8_t full[N];
	masks_t m;
	memset(full, 0, sizeof(full));
	masks_of(full, &m);
	solve(full, &m, 1, true);
	memcpy(solution, full, N);

	int order[N];
	for (int i = 0; i < N; i++) {
		order[i] = i;
	}
	for (int i = N - 1; i > 0; i--) {
		int j = rand() % (i + 1);
		int t = order[i];
		order[i] = order[j];
		order[j] = t;
	}
	int8_t work[N];
	memcpy(work, full, N);
	int filled = N;
	for (int k = 0; k < N && filled > LEVELS[level].givens; k++) {
		int i = order[k];
		int8_t keep = work[i];
		work[i] = 0;
		int8_t probe[N];
		memcpy(probe, work, N);
		masks_t pm;
		masks_of(probe, &pm);
		if (solve(probe, &pm, 2, false) == 1) {
			filled--;
		} else {
			work[i] = keep; // two answers: this one has to stay
		}
	}
	memcpy(grid, work, N);
	for (int i = 0; i < N; i++) {
		given[i] = grid[i] != 0;
		notes[i] = 0;
	}
}

static bool conflicts(int i) {
	int8_t v = grid[i];
	if (!v) {
		return false;
	}
	for (int j = 0; j < N; j++) {
		if (j != i && grid[j] == v && (row_of(j) == row_of(i) || col_of(j) == col_of(i) || box_of(j) == box_of(i))) {
			return true;
		}
	}
	return false;
}

static bool complete(void) {
	for (int i = 0; i < N; i++) {
		if (grid[i] != solution[i]) {
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void fmt_time(char *out, size_t size, long secs) { snprintf(out, size, "%ld:%02ld", secs / 60, secs % 60); }

static long elapsed_s(void) {
	uint32_t ms = state == ST_WON ? end_ms : clock_ms;
	return (long)(ms / 1000) + (long)hints * HINT_PENALTY_S;
}

static int pad_x(int d) { return 8 + (d - 1) * (pad_w + 4); }
static int tool_w(void) { return (game->w - 16 - 3 * 8) / 4; }
static int tool_x(int i) { return 8 + i * (tool_w() + 8); }

static void draw_cell(int i) {
	gamecanvas_t *c = &game->canvas;
	int x = gx + col_of(i) * cs;
	int y = gy + row_of(i) * cs;
	uint16_t bg = (((row_of(i) / 3) + (col_of(i) / 3)) & 1) ? col_cell_alt : col_cell;
	if (sel >= 0 && state == ST_PLAY) {
		if (i == sel) {
			bg = col_sel;
		} else if (grid[sel] && grid[i] == grid[sel]) {
			bg = col_same;
		} else if (row_of(i) == row_of(sel) || col_of(i) == col_of(sel) || box_of(i) == box_of(sel)) {
			bg = col_peer;
		}
	}
	gamecanvas_rect(c, x, y, cs, cs, bg);
	if (state == ST_READY) {
		gamecanvas_dirty(c, x, y, cs, cs);
		return;
	}
	if (grid[i]) {
		char t[2] = {(char)('0' + grid[i]), 0};
		uint16_t col = conflicts(i) ? col_bad : given[i] ? col_given : col_mine;
		if (i == sel && col == col_mine) {
			col = gc_rgb(255, 255, 255);
		}
		gamecanvas_text_centered(c, x + cs / 2, y + cs / 2, t, cs >= 44 ? 4 : 3, col);
	} else if (notes[i]) {
		int sub = cs / 3;
		int scale = sub >= 15 ? 2 : 1;
		for (int d = 1; d <= 9; d++) {
			if (notes[i] & (1u << (d - 1))) {
				char t[2] = {(char)('0' + d), 0};
				int nx = x + ((d - 1) % 3) * sub + sub / 2;
				int ny = y + ((d - 1) / 3) * sub + sub / 2;
				gamecanvas_text_centered(c, nx, ny, t, scale, col_note);
			}
		}
	}
	gamecanvas_dirty(c, x, y, cs, cs);
}

static void draw_lines(void) {
	gamecanvas_t *c = &game->canvas;
	for (int k = 0; k <= 9; k++) {
		bool thick = k % 3 == 0;
		int t = thick ? 3 : 1;
		gamecanvas_rect(c, gx + k * cs - t / 2, gy - t / 2, t, 9 * cs + t, thick ? col_line_box : col_line);
		gamecanvas_rect(c, gx - t / 2, gy + k * cs - t / 2, 9 * cs + t, t, thick ? col_line_box : col_line);
	}
}

static int digit_count(int d) {
	int n = 0;
	for (int i = 0; i < N; i++) {
		n += grid[i] == d;
	}
	return n;
}

static void draw_controls(void) {
	gamecanvas_t *c = &game->canvas;
	gamecanvas_rect(c, 0, pad_y - 4, game->w, game->h - pad_y + 4, col_bg);
	for (int d = 1; d <= 9; d++) {
		bool used = state == ST_PLAY && digit_count(d) >= 9;
		uint16_t fill = used ? col_cell : col_button;
		gamecanvas_round_rect(c, pad_x(d), pad_y, pad_w, BTN_H, 12, fill);
		char t[2] = {(char)('0' + d), 0};
		gamecanvas_text_centered(c, pad_x(d) + pad_w / 2, pad_y + BTN_H / 2, t, 4, used ? col_line : gc_rgb(255, 255, 255));
	}
	for (int i = 0; i < 4; i++) {
		char text[24];
		uint16_t fill = col_button;
		switch (i) {
		case 0:
			snprintf(text, sizeof(text), "ERASE");
			break;
		case 1:
			snprintf(text, sizeof(text), "NOTES");
			if (notes_mode) {
				fill = col_button_on;
			}
			break;
		case 2:
			snprintf(text, sizeof(text), "HINT");
			break;
		default:
			snprintf(text, sizeof(text), "%s", level_armed_until ? "SURE?" : BAR_LEVEL[level]);
			if (level_armed_until) {
				fill = col_button_on;
			}
			break;
		}
		gamecanvas_round_rect(c, tool_x(i), tool_y, tool_w(), BTN_H, 20, fill);
		int scale = gamecanvas_text_width(text, 3) <= tool_w() - 8 ? 3 : 2;
		gamecanvas_text_centered(c, tool_x(i) + tool_w() / 2, tool_y + BTN_H / 2, text, scale, gc_rgb(255, 255, 255));
	}
	gamecanvas_dirty(c, 0, pad_y - 4, game->w, game->h - pad_y + 4);
}

static void draw_all(void) {
	gamecanvas_fill(&game->canvas, col_bg);
	for (int i = 0; i < N; i++) {
		draw_cell(i);
	}
	draw_lines();
	draw_controls();
	gk_dirty_all(game);
}

static void show_hud(void) {
	char a[40];
	char b[40];
	char t[16];
	fmt_time(t, sizeof(t), elapsed_s());
	snprintf(a, sizeof(a), "%s %s", tr("game_time"), t);
	long best = highscore_get(LEVELS[level].key);
	if (best > 0) {
		char bt[16];
		fmt_time(bt, sizeof(bt), best);
		snprintf(b, sizeof(b), "%s %s", tr("game_best"), bt);
	} else {
		snprintf(b, sizeof(b), "%s", tr(LEVELS[level].name_key));
	}
	gk_ui_score(&game->ui, a, b);
}

static void start_message(void) {
	char text[240];
	int n = snprintf(text, sizeof(text), "%s", tr("game_tap_start"));
	for (int i = 0; i < 3; i++) {
		long best = highscore_get(LEVELS[i].key);
		char t[16];
		if (best > 0) {
			fmt_time(t, sizeof(t), best);
		} else {
			snprintf(t, sizeof(t), "--");
		}
		n += snprintf(text + n, sizeof(text) - (size_t)n, "\n%s %s", tr(LEVELS[i].name_key), t);
	}
	gk_ui_message(&game->ui, text, -40);
}

// ---------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------

static void ready(void) {
	level_armed_until = 0;
	state = ST_READY;
	sel = -1;
	hints = 0;
	clock_ms = 0;
	shown_seconds = 0;
	new_best = false;
	memset(grid, 0, sizeof(grid));
	memset(given, 0, sizeof(given));
	memset(notes, 0, sizeof(notes));
	gk_timer(game, false);
	draw_all();
	show_hud();
	start_message();
}

static void begin(void) {
	make_puzzle();
	state = ST_PLAY;
	clock_ms = 0;
	gk_ui_message_hide(&game->ui);
	gk_timer(game, true);
	draw_all();
	show_hud();
}

static void win(void) {
	state = ST_WON;
	end_ms = clock_ms;
	won_at = clock_ms;
	long secs = elapsed_s();
	new_best = highscore_submit(LEVELS[level].key, secs, true);
	gamesound_play(SND_WIN);
	sel = -1;
	draw_all();
	show_hud();
	char text[160];
	char t[16];
	fmt_time(t, sizeof(t), secs);
	int n = snprintf(text, sizeof(text), "%s\n%s %s", tr("game_you_win"), tr("game_time"), t);
	if (new_best && n < (int)sizeof(text)) {
		n += snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", tr("game_new_best"));
	}
	snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", tr("game_tap_again"));
	gk_ui_message(&game->ui, text, -40);
}

static void redraw_peers(int old_sel) {
	// the highlight follows the selection: everything that was or is in its lines
	for (int i = 0; i < N; i++) {
		bool was = old_sel >= 0 && (row_of(i) == row_of(old_sel) || col_of(i) == col_of(old_sel) ||
									box_of(i) == box_of(old_sel) || (grid[old_sel] && grid[i] == grid[old_sel]));
		bool is = sel >= 0 && (row_of(i) == row_of(sel) || col_of(i) == col_of(sel) || box_of(i) == box_of(sel) ||
							   (grid[sel] && grid[i] == grid[sel]));
		if (was || is) {
			draw_cell(i);
		}
	}
	draw_lines();
	gk_dirty_all(game);
}

static void select_cell(int i) {
	int old = sel;
	sel = i;
	redraw_peers(old);
	gamesound_play(SND_TICK);
}

// Drops a digit from the pencil marks of everything that sees `i`.
static void clear_marks_around(int i, int d) {
	for (int j = 0; j < N; j++) {
		if (row_of(j) == row_of(i) || col_of(j) == col_of(i) || box_of(j) == box_of(i)) {
			notes[j] &= (uint16_t)~(1u << (d - 1));
		}
	}
}

static void put_digit(int d) {
	if (sel < 0 || given[sel] || state != ST_PLAY) {
		gamesound_play(SND_ERROR);
		return;
	}
	if (notes_mode && !grid[sel]) {
		notes[sel] ^= (uint16_t)(1u << (d - 1));
		gamesound_play(SND_TICK);
		draw_cell(sel);
		return;
	}
	if (grid[sel] == d) {
		grid[sel] = 0; // the same digit again takes it back
	} else {
		grid[sel] = (int8_t)d;
		notes[sel] = 0;
		clear_marks_around(sel, d);
		gamesound_play(conflicts(sel) ? SND_ERROR : SND_BLIP);
	}
	draw_all();
	if (complete()) {
		win();
	}
}

static void erase_cell(void) {
	if (sel < 0 || given[sel] || state != ST_PLAY) {
		return;
	}
	grid[sel] = 0;
	notes[sel] = 0;
	gamesound_play(SND_TICK);
	draw_all();
}

static void hint(void) {
	if (sel < 0 || given[sel] || state != ST_PLAY || grid[sel] == solution[sel]) {
		gamesound_play(SND_ERROR);
		return;
	}
	grid[sel] = solution[sel];
	notes[sel] = 0;
	clear_marks_around(sel, grid[sel]);
	hints++;
	gamesound_play(SND_EAT);
	draw_all();
	show_hud();
	if (complete()) {
		win();
	}
}

static void tap(int x, int y) {
	if (state == ST_WON) {
		if (clock_ms - won_at > 700) {
			ready();
		}
		return;
	}
	bool was_armed = level_armed_until != 0;
	level_armed_until = 0; // any other tap takes the question back
	// the bars at the bottom
	if (y >= tool_y - 4 && y < tool_y + BTN_H + 4) {
		for (int i = 0; i < 4; i++) {
			if (x >= tool_x(i) - 4 && x < tool_x(i) + tool_w() + 4) {
				switch (i) {
				case 0:
					erase_cell();
					break;
				case 1:
					notes_mode = !notes_mode;
					gamesound_play(SND_BLIP);
					draw_controls();
					break;
				case 2:
					hint();
					break;
				default:
					if (state == ST_PLAY && !was_armed) {
						level_armed_until = clock_ms + 2500;
						gamesound_play(SND_BLIP);
						draw_controls();
						break;
					}
					level = (level + 1) % 3;
					config_set_int("games", "sudoku_level", level);
					config_save();
					gamesound_play(SND_BLIP);
					ready();
					break;
				}
				if (was_armed && i != 3) {
					draw_controls();
				}
				return;
			}
		}
		if (was_armed) {
			draw_controls();
		}
		return;
	}
	if (was_armed) {
		draw_controls();
	}
	if (y >= pad_y - 4 && y < pad_y + BTN_H + 4) {
		for (int d = 1; d <= 9; d++) {
			if (x >= pad_x(d) - 2 && x < pad_x(d) + pad_w + 2) {
				if (state == ST_PLAY) {
					put_digit(d);
				}
				return;
			}
		}
		return;
	}
	if (x >= gx && y >= gy && x < gx + 9 * cs && y < gy + 9 * cs) {
		if (state == ST_READY) {
			begin();
			return;
		}
		select_cell((y - gy) / cs * 9 + (x - gx) / cs);
		return;
	}
	if (state == ST_READY) {
		begin();
	}
}

static void touch_cb(gk_game_t *g, gk_phase_t phase, int x, int y) {
	(void)g;
	if (phase == GK_DOWN) {
		down_x = x;
		down_y = y;
	} else if (phase == GK_UP) {
		if (abs(x - down_x) < 24 && abs(y - down_y) < 24) {
			tap(x, y);
		}
	}
}

static void tick(gk_game_t *g, uint32_t dt) {
	(void)g;
	clock_ms += dt;
	if (level_armed_until && clock_ms >= level_armed_until) {
		level_armed_until = 0;
		draw_controls();
	}
	if (state != ST_PLAY) {
		return; // the clock runs on once it is over, only to time the tap that starts again
	}
	int secs = (int)elapsed_s();
	if (secs != shown_seconds) {
		shown_seconds = secs;
		show_hud();
	}
}

static void layout(void) {
	int w = game->w;
	int h = game->h;
	int by_h = (h - 2 * BTN_H - 3 * GAP - 8) / 9;
	int by_w = (w - 16) / 9;
	cs = by_h < by_w ? by_h : by_w;
	gx = (w - 9 * cs) / 2;
	gy = 4;
	pad_y = gy + 9 * cs + GAP + 4;
	tool_y = pad_y + BTN_H + GAP;
	pad_w = (w - 16 - 8 * 4) / 9;
}

static void load_colours(void) {
	col_bg = gc_rgb(18, 20, 28);
	col_cell = gc_rgb(34, 38, 52);
	col_cell_alt = gc_rgb(29, 33, 46);
	col_peer = gc_rgb(46, 54, 78);
	col_same = gc_rgb(62, 80, 120);
	col_sel = gc_rgb(52, 104, 200);
	col_line = gc_rgb(54, 60, 82);
	col_line_box = gc_rgb(120, 130, 160);
	col_given = gc_rgb(236, 240, 248);
	col_mine = gc_rgb(110, 176, 255);
	col_bad = gc_rgb(255, 96, 96);
	col_note = gc_rgb(140, 150, 178);
	col_button = gc_rgb(62, 72, 100);
	col_button_on = gc_rgb(52, 104, 200);
}

static void open_game(gk_game_t *g) {
	game = g;
	srand((unsigned)time(NULL));
	load_colours();
	level = (int)config_get_int("games", "sudoku_level", 0);
	if (level < 0 || level > 2) {
		level = 0;
	}
	notes_mode = false;
	clock_ms = 0;
	layout();
	ready();
}

static void restart(gk_game_t *g) {
	(void)g;
	ready();
	begin(); // New game from the pause menu goes straight to a new puzzle
}

const gk_def_t sudoku_def = {
	.id = "sudoku",
	.tick_ms = 100,
	.open = open_game,
	.touch = touch_cb,
	.tick = tick,
	.restart = restart,
};

void sudoku_init(gui_config_t *cfg) { gk_game_init(&game_object, sudoku_screen, cfg, &sudoku_def); }

#ifdef GAME_TEST
// For the host harness.
void sudoku_test_solve(int cells_left) {
	for (int i = 0; i < N && cells_left >= 0; i++) {
		if (!grid[i]) {
			if (cells_left-- == 0) {
				break;
			}
			grid[i] = solution[i];
		}
	}
	draw_all();
	if (complete()) {
		win();
	}
}
int sudoku_test_givens(void) {
	int n = 0;
	for (int i = 0; i < N; i++) {
		n += given[i] != 0;
	}
	return n;
}
int sudoku_test_unique(void) {
	int8_t probe[N];
	memcpy(probe, grid, N);
	masks_t m;
	masks_of(probe, &m);
	return solve(probe, &m, 5, false);
}
#endif
