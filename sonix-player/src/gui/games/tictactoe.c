#include "tictactoe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *tictactoe_screen;

#define AI_DELAY_MS 420
#define DEAF_MS 700
#define LEVEL_BAR_H 56

typedef enum {
	ST_READY,
	ST_PLAYER,
	ST_AI,
	ST_OVER,
} state_t;

enum { EMPTY = 0, MARK_X = 1, MARK_O = 2 };
enum { OUT_NONE, OUT_WIN, OUT_LOSS, OUT_DRAW };

static const char *const LEVEL_KEYS[3] = {"game_easy", "game_medium", "game_hard"};
static const char *const BAR_LEVEL[3] = {"EASY", "MEDIUM", "HARD"}; // drawn in the canvas: capitals only
static const char *const STAT_NAMES[3][3] = {
	{"ttt.easy.w", "ttt.easy.d", "ttt.easy.l"},
	{"ttt.medium.w", "ttt.medium.d", "ttt.medium.l"},
	{"ttt.hard.w", "ttt.hard.d", "ttt.hard.l"},
};

static const int LINES[8][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}, {0, 3, 6}, {1, 4, 7}, {2, 5, 8}, {0, 4, 8}, {2, 4, 6}};

static gk_game_t game_object;
static gk_game_t *game;
static state_t state;
static int level;
static uint8_t board[9];
static int outcome;
static int win_line = -1;
static int games_played;
static uint32_t clock_ms;
static uint32_t ai_at;
static uint32_t end_at;

// Where things are.
static int cell;
static int gx;
static int gy;
static int bar_x[3];
static int bar_w;

// A finger.
static int down_x;
static int down_y;

static uint16_t col_bg;
static uint16_t col_line;
static uint16_t col_x;
static uint16_t col_o;
static uint16_t col_win;
static uint16_t col_loss;
static uint16_t col_pill;
static uint16_t col_pill_on;

// ---------------------------------------------------------------------------
// The play
// ---------------------------------------------------------------------------

static int winner_of(const uint8_t *b, int *line) {
	for (int i = 0; i < 8; i++) {
		int a = LINES[i][0];
		if (b[a] && b[a] == b[LINES[i][1]] && b[a] == b[LINES[i][2]]) {
			if (line) {
				*line = i;
			}
			return b[a];
		}
	}
	return 0;
}

static bool full(const uint8_t *b) {
	for (int i = 0; i < 9; i++) {
		if (!b[i]) {
			return false;
		}
	}
	return true;
}

// The value of the position to O (the computer) with `turn` to move: +1 a win, 0 a draw,
// -1 a loss, the quickest win and the slowest loss preferred by the caller through depth.
static int minimax(uint8_t *b, int turn, int depth) {
	int w = winner_of(b, NULL);
	if (w == MARK_O) {
		return 10 - depth;
	}
	if (w == MARK_X) {
		return depth - 10;
	}
	if (full(b)) {
		return 0;
	}
	int best = turn == MARK_O ? -100 : 100;
	for (int i = 0; i < 9; i++) {
		if (b[i]) {
			continue;
		}
		b[i] = (uint8_t)turn;
		int v = minimax(b, turn == MARK_O ? MARK_X : MARK_O, depth + 1);
		b[i] = EMPTY;
		if (turn == MARK_O ? v > best : v < best) {
			best = v;
		}
	}
	return best;
}

static int random_empty(void) {
	int options[9];
	int n = 0;
	for (int i = 0; i < 9; i++) {
		if (!board[i]) {
			options[n++] = i;
		}
	}
	return options[rand() % n];
}

// A square that finishes a line for `mark`, or -1.
static int finishing_move(int mark) {
	for (int i = 0; i < 9; i++) {
		if (board[i]) {
			continue;
		}
		board[i] = (uint8_t)mark;
		bool wins = winner_of(board, NULL) == mark;
		board[i] = EMPTY;
		if (wins) {
			return i;
		}
	}
	return -1;
}

static int best_move(void) {
	int options[9];
	int n = 0;
	int best = -100;
	for (int i = 0; i < 9; i++) {
		if (board[i]) {
			continue;
		}
		board[i] = MARK_O;
		int v = minimax(board, MARK_X, 1);
		board[i] = EMPTY;
		if (v > best) {
			best = v;
			n = 0;
		}
		if (v == best) {
			options[n++] = i;
		}
	}
	return options[rand() % n];
}

static int medium_move(void) {
	int m = finishing_move(MARK_O);
	if (m < 0) {
		m = finishing_move(MARK_X);
	}
	if (m >= 0) {
		return m;
	}
	if (!board[4]) {
		return 4;
	}
	static const int CORNERS[4] = {0, 2, 6, 8};
	int start = rand() % 4;
	for (int k = 0; k < 4; k++) {
		if (!board[CORNERS[(start + k) % 4]]) {
			return CORNERS[(start + k) % 4];
		}
	}
	return random_empty();
}

static int choose_move(void) {
	int roll = rand() % 100;
	if (level == 0) {
		return roll < 65 ? random_empty() : medium_move();
	}
	if (level == 1) {
		return roll < 20 ? random_empty() : medium_move();
	}
	return best_move();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static int cx_of(int i) { return gx + (i % 3) * cell + cell / 2; }
static int cy_of(int i) { return gy + (i / 3) * cell + cell / 2; }

// A line with round ends, as a string of dots.
static void thick_line(int x0, int y0, int x1, int y1, int r, uint16_t col) {
	int dx = x1 - x0;
	int dy = y1 - y0;
	int steps = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 2 + 1;
	for (int i = 0; i <= steps; i++) {
		gamecanvas_circle(&game->canvas, x0 + dx * i / steps, y0 + dy * i / steps, r, col);
	}
}

static void draw_mark(int i, uint16_t col) {
	int cx = cx_of(i);
	int cy = cy_of(i);
	int arm = cell * 30 / 100;
	int r = cell / 12 + 2;
	gamecanvas_t *c = &game->canvas;
	if (board[i] == MARK_X) {
		thick_line(cx - arm, cy - arm, cx + arm, cy + arm, r, col);
		thick_line(cx - arm, cy + arm, cx + arm, cy - arm, r, col);
	} else if (board[i] == MARK_O) {
		gamecanvas_circle(c, cx, cy, arm + r, col);
		gamecanvas_circle(c, cx, cy, arm - r, col_bg);
	}
}

static void draw_stats(void) {
	gamecanvas_t *c = &game->canvas;
	int y = gy + 3 * cell + 26;
	gamecanvas_rect(c, 0, y - 4, game->w, 60, col_bg);
	char text[48];
	snprintf(text, sizeof(text), "W %ld   D %ld   L %ld", highscore_get(STAT_NAMES[level][0]),
			 highscore_get(STAT_NAMES[level][1]), highscore_get(STAT_NAMES[level][2]));
	gamecanvas_text_centered(c, game->w / 2, y + 22, text, 3, gc_rgb(200, 206, 222));
	gamecanvas_dirty(c, 0, y - 4, game->w, 60);
}

static void draw_bar(void) {
	gamecanvas_t *c = &game->canvas;
	gamecanvas_rect(c, 0, 0, game->w, LEVEL_BAR_H + 16, col_bg);
	for (int i = 0; i < 3; i++) {
		gamecanvas_round_rect(c, bar_x[i], 8, bar_w, LEVEL_BAR_H, LEVEL_BAR_H / 2, i == level ? col_pill_on : col_pill);
		gamecanvas_text_centered(c, bar_x[i] + bar_w / 2, 8 + LEVEL_BAR_H / 2, BAR_LEVEL[i],
								 gamecanvas_text_width(BAR_LEVEL[i], 3) <= bar_w - 10 ? 3 : 2, gc_rgb(255, 255, 255));
	}
	gamecanvas_dirty(c, 0, 0, game->w, LEVEL_BAR_H + 16);
}

static void draw_board(void) {
	gamecanvas_t *c = &game->canvas;
	gamecanvas_rect(c, 0, LEVEL_BAR_H + 16, game->w, 3 * cell + gy - LEVEL_BAR_H - 16 + 16, col_bg);
	for (int k = 1; k < 3; k++) {
		gamecanvas_round_rect(c, gx + k * cell - 3, gy + 8, 6, 3 * cell - 16, 3, col_line);
		gamecanvas_round_rect(c, gx + 8, gy + k * cell - 3, 3 * cell - 16, 6, 3, col_line);
	}
	for (int i = 0; i < 9; i++) {
		if (board[i]) {
			draw_mark(i, board[i] == MARK_X ? col_x : col_o);
		}
	}
	if (win_line >= 0) {
		uint16_t col = outcome == OUT_WIN ? col_win : col_loss;
		for (int k = 0; k < 3; k++) {
			int i = LINES[win_line][k];
			draw_mark(i, col);
		}
	}
	gamecanvas_dirty(c, 0, LEVEL_BAR_H + 16, game->w, 3 * cell + 24);
}

static void draw_all(void) {
	gamecanvas_fill(&game->canvas, col_bg);
	draw_bar();
	draw_board();
	draw_stats();
	gk_dirty_all(game);
}

static void show_hud(void) {
	char b[48];
	snprintf(b, sizeof(b), "W%ld D%ld L%ld", highscore_get(STAT_NAMES[level][0]), highscore_get(STAT_NAMES[level][1]),
			 highscore_get(STAT_NAMES[level][2]));
	gk_ui_score(&game->ui, tr(LEVEL_KEYS[level]), b);
}

static void start_message(void) {
	char text[240];
	int n = snprintf(text, sizeof(text), "%s", tr("game_tap_start"));
	for (int i = 0; i < 3; i++) {
		n += snprintf(text + n, sizeof(text) - (size_t)n, "\n%s  %ld-%ld-%ld", tr(LEVEL_KEYS[i]),
					  highscore_get(STAT_NAMES[i][0]), highscore_get(STAT_NAMES[i][1]), highscore_get(STAT_NAMES[i][2]));
	}
	snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", tr("ttt_legend"));
	gk_ui_message(&game->ui, text, 20);
}

// ---------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------

static void ready(void) {
	state = ST_READY;
	memset(board, 0, sizeof(board));
	win_line = -1;
	outcome = OUT_NONE;
	draw_all();
	show_hud();
	start_message();
}

static void begin(void) {
	memset(board, 0, sizeof(board));
	win_line = -1;
	outcome = OUT_NONE;
	gk_ui_message_hide(&game->ui);
	bool player_first = (games_played & 1) == 0;
	games_played++;
	draw_all();
	if (player_first) {
		state = ST_PLAYER;
	} else {
		state = ST_AI;
		ai_at = clock_ms + AI_DELAY_MS;
	}
	gk_timer(game, true);
}

static void finish(int result) {
	state = ST_OVER;
	outcome = result;
	end_at = clock_ms;
	int which = result == OUT_WIN ? 0 : result == OUT_DRAW ? 1 : 2;
	highscore_set(STAT_NAMES[level][which], highscore_get(STAT_NAMES[level][which]) + 1);
	winner_of(board, &win_line);
	if (result == OUT_DRAW) {
		win_line = -1;
	}
	gamesound_play(result == OUT_WIN ? SND_WIN : result == OUT_LOSS ? SND_LOSE : SND_ERROR);
	draw_all();
	show_hud();
	char text[120];
	snprintf(text, sizeof(text), "%s\n%s", tr(result == OUT_WIN ? "ttt_you_win" : result == OUT_LOSS ? "ttt_you_lose" : "ttt_draw"),
			 tr("game_tap_again"));
	// Under the board, where the squares and the line through them stay in view.
	int below = gy + 3 * cell + 96; // the middle of the message, in canvas coordinates
	gk_ui_message(&game->ui, text, (int)game->cfg->screen_height - game->h - 6 + below - (int)game->cfg->screen_height / 2);
}

static void check_end(void) {
	int w = winner_of(board, NULL);
	if (w == MARK_X) {
		finish(OUT_WIN);
	} else if (w == MARK_O) {
		finish(OUT_LOSS);
	} else if (full(board)) {
		finish(OUT_DRAW);
	}
}

static void place(int i, int mark) {
	board[i] = (uint8_t)mark;
	gamesound_play(mark == MARK_X ? SND_BLIP : SND_TICK);
	draw_board();
	check_end();
}

static void tap(int x, int y) {
	// The level pills: a different level starts over, and does not count.
	if (y < LEVEL_BAR_H + 16) {
		for (int i = 0; i < 3; i++) {
			if (x >= bar_x[i] - 4 && x < bar_x[i] + bar_w + 4 && i != level) {
				level = i;
				config_set_int("games", "ttt_level", level);
				config_save();
				gamesound_play(SND_BLIP);
				ready();
				return;
			}
		}
		return;
	}
	if (state == ST_READY) {
		begin();
		return;
	}
	if (state == ST_OVER) {
		if (clock_ms - end_at > DEAF_MS) {
			begin();
		}
		return;
	}
	if (state != ST_PLAYER) {
		return;
	}
	if (x < gx || y < gy || x >= gx + 3 * cell || y >= gy + 3 * cell) {
		return;
	}
	int i = (y - gy) / cell * 3 + (x - gx) / cell;
	if (board[i]) {
		gamesound_play(SND_ERROR);
		return;
	}
	state = ST_AI;
	ai_at = clock_ms + AI_DELAY_MS;
	place(i, MARK_X);
}

static void touch_cb(gk_game_t *g, gk_phase_t phase, int x, int y) {
	(void)g;
	if (phase == GK_DOWN) {
		down_x = x;
		down_y = y;
	} else if (phase == GK_UP && abs(x - down_x) < 30 && abs(y - down_y) < 30) {
		tap(x, y);
	}
}

static void tick(gk_game_t *g, uint32_t dt) {
	(void)g;
	clock_ms += dt;
	if (state == ST_AI && clock_ms >= ai_at) {
		state = ST_PLAYER;
		place(choose_move(), MARK_O);
	}
}

static void layout(void) {
	// Room under the board for the tally and the message that ends a game.
	int by_w = (game->w - 48) / 3;
	int by_h = (game->h - LEVEL_BAR_H - 16 - 14 - 190) / 3;
	cell = by_w < by_h ? by_w : by_h;
	gx = (game->w - 3 * cell) / 2;
	gy = LEVEL_BAR_H + 16 + 14;
	bar_w = (game->w - 24 - 2 * 10) / 3;
	for (int i = 0; i < 3; i++) {
		bar_x[i] = 12 + i * (bar_w + 10);
	}
}

static void load_colours(void) {
	col_bg = gc_rgb(18, 20, 28);
	col_line = gc_rgb(70, 78, 104);
	col_x = gc_rgb(96, 180, 255);
	col_o = gc_rgb(255, 140, 120);
	col_win = gc_rgb(110, 220, 130);
	col_loss = gc_rgb(255, 90, 90);
	col_pill = gc_rgb(54, 62, 88);
	col_pill_on = gc_rgb(52, 104, 200);
}

static void open_game(gk_game_t *g) {
	game = g;
	srand((unsigned)time(NULL));
	load_colours();
	level = (int)config_get_int("games", "ttt_level", 1);
	if (level < 0 || level > 2) {
		level = 1;
	}
	games_played = 0;
	clock_ms = 0;
	layout();
	gk_timer(game, true);
	ready();
}

static void restart(gk_game_t *g) {
	(void)g;
	ready();
	begin();
}

const gk_def_t tictactoe_def = {
	.id = "tictactoe",
	.tick_ms = 50,
	.open = open_game,
	.touch = touch_cb,
	.tick = tick,
	.restart = restart,
};

void tictactoe_init(gui_config_t *cfg) { gk_game_init(&game_object, tictactoe_screen, cfg, &tictactoe_def); }

#ifdef GAME_TEST
void ttt_test_cell(int i, int *x, int *y) {
	*x = cx_of(i);
	*y = cy_of(i);
}
int ttt_test_state(void) { return (int)state; }
int ttt_test_outcome(void) { return outcome; }
void ttt_test_level(int l) {
	level = l;
	config_set_int("games", "ttt_level", level);
	ready();
}
int ttt_test_empty(int i) { return board[i] == EMPTY; }
#endif
