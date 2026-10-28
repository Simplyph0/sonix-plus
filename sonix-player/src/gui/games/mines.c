#include "mines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *mines_screen;

#define MAX_COLS 20
#define MAX_ROWS 30
#define BAR_H 64
#define HOLD_MS 420

typedef enum {
	ST_READY,
	ST_PLAY,
	ST_WON,
	ST_LOST,
} state_t;

typedef struct {
	const char *key; // the best time's name, and its label's language key
	int cell;		 // the square's side, as asked for: the grid is as many as fit
	int density;	 // mines per hundred squares
} level_t;

static const level_t LEVELS[3] = {
	{"mines.easy", 48, 12},
	{"mines.medium", 36, 16},
	{"mines.hard", 30, 20},
};
static const char *const LEVEL_NAMES[3] = {"game_easy", "game_medium", "game_hard"};
// The bar is drawn into the canvas, whose face has capitals and digits only: its words are
// not translated.
static const char *const BAR_LEVEL[3] = {"EASY", "MEDIUM", "HARD"};

static gk_game_t game_object;
static gk_game_t *game;
static state_t state;
static int level;
static int cols;
static int rows;
static int cell;
static int gx;
static int gy;
static int mine_total;
static int flags;
static int opened;
static uint8_t is_mine[MAX_ROWS][MAX_COLS];
static uint8_t near_count[MAX_ROWS][MAX_COLS];
static uint8_t shown[MAX_ROWS][MAX_COLS]; // 0 covered, 1 open, 2 flagged
static int boom_x;
static int boom_y;
static bool flag_mode;
static uint32_t clock_ms;
static int armed_button = -1; // a running game asks twice before NEW or the level starts it over
static uint32_t armed_until;
static int shown_seconds;
static bool new_best;
static uint32_t final_ms;
static uint32_t end_at;

// A finger.
static int press_col;
static int press_row;
static int press_button; // a bar button, or -1
static uint32_t press_ms;
static bool press_long;
static bool press_valid;

// The look.
static uint16_t col_bg;
static uint16_t col_covered;
static uint16_t col_covered_hi;
static uint16_t col_covered_lo;
static uint16_t col_open;
static uint16_t col_text;
static uint16_t col_dim;
static uint16_t col_num[9];

enum { BTN_LEVEL, BTN_MODE, BTN_NEW, BTN_COUNT };

static void fmt_time(char *out, size_t size, long secs) { snprintf(out, size, "%ld:%02ld", secs / 60, secs % 60); }

static int bar_x(int i) { return 12 + i * ((game->w - 24 + 10) / BTN_COUNT); }
static int bar_w(void) { return (game->w - 24 + 10) / BTN_COUNT - 10; }
static int bar_y(void) { return game->h - BAR_H - 6; }

static void draw_flag(gamecanvas_t *c, int x, int y, int s) {
	uint16_t red = gc_rgb(232, 64, 64);
	uint16_t pole = gc_rgb(225, 228, 236);
	int px = x + s * 38 / 100;
	int top = y + s * 16 / 100;
	int bottom = y + s * 76 / 100;
	// the pennant, pointing right from the top of the pole
	int h = s * 36 / 100;
	for (int i = 0; i < h; i++) {
		int half = i < h / 2 ? i : h - 1 - i;
		gamecanvas_rect(c, px + 2, top + i, half * 2 * 6 / 10 + 2, 1, red);
	}
	gamecanvas_rect(c, px, top, 3, bottom - top, pole);
	gamecanvas_rect(c, x + s * 24 / 100, bottom, s * 40 / 100, 4, pole);
}

static void draw_mine(gamecanvas_t *c, int x, int y, int s, uint16_t body) {
	int cx = x + s / 2;
	int cy = y + s / 2;
	int r = s * 24 / 100;
	gamecanvas_rect(c, cx - r - 4, cy - 1, r * 2 + 8, 3, body);
	gamecanvas_rect(c, cx - 1, cy - r - 4, 3, r * 2 + 8, body);
	gamecanvas_circle(c, cx, cy, r, body);
	gamecanvas_circle(c, cx - r / 3, cy - r / 3, r / 4 + 1, gc_rgb(230, 230, 235));
}

static void draw_cell(int cx, int cy) {
	gamecanvas_t *c = &game->canvas;
	int x = gx + cx * cell;
	int y = gy + cy * cell;
	int s = cell;
	int pad = 1;
	bool lost = state == ST_LOST;
	uint8_t st = shown[cy][cx];

	if (st == 1) {
		uint16_t bg = (lost && cx == boom_x && cy == boom_y) ? gc_rgb(200, 56, 56) : col_open;
		gamecanvas_rect(c, x, y, s, s, col_bg);
		gamecanvas_round_rect(c, x + pad, y + pad, s - pad * 2, s - pad * 2, 4, bg);
		if (is_mine[cy][cx]) {
			draw_mine(c, x, y, s, gc_rgb(20, 20, 24));
		} else if (near_count[cy][cx]) {
			char t[2] = {(char)('0' + near_count[cy][cx]), 0};
			int scale = s >= 44 ? 5 : s >= 34 ? 4 : 3;
			gamecanvas_text_centered(c, x + s / 2, y + s / 2, t, scale, col_num[near_count[cy][cx]]);
		}
	} else {
		gamecanvas_rect(c, x, y, s, s, col_bg);
		if (lost && is_mine[cy][cx] && st != 2) {
			gamecanvas_round_rect(c, x + pad, y + pad, s - pad * 2, s - pad * 2, 4, col_open);
			draw_mine(c, x, y, s, gc_rgb(20, 20, 24));
		} else {
			gamecanvas_round_rect(c, x + pad, y + pad, s - pad * 2, s - pad * 2, 5, col_covered_lo);
			gamecanvas_round_rect(c, x + pad, y + pad, s - pad * 2 - 2, s - pad * 2 - 3, 5, col_covered);
			gamecanvas_rect(c, x + 5, y + 4, s - 12, 2, col_covered_hi);
			if (st == 2) {
				draw_flag(c, x, y, s);
				if (lost && !is_mine[cy][cx]) {
					// a flag on a square with no mine under it
					gamecanvas_rect(c, x + 4, y + s / 2 - 1, s - 8, 3, gc_rgb(255, 255, 255));
				}
			}
		}
	}
	gamecanvas_dirty(c, x, y, s, s);
}

static void draw_button(int i, bool pressed) {
	gamecanvas_t *c = &game->canvas;
	int x = bar_x(i);
	int y = bar_y();
	int w = bar_w();
	uint16_t fill = pressed ? col_covered_hi : col_covered;
	char text[24];
	switch (i) {
	case BTN_LEVEL:
		snprintf(text, sizeof(text), "%s", armed_button == i ? "SURE?" : BAR_LEVEL[level]);
		break;
	case BTN_MODE:
		snprintf(text, sizeof(text), "%s", flag_mode ? "FLAG" : "DIG");
		if (flag_mode) {
			fill = gc_rgb(200, 70, 60);
		}
		break;
	default:
		snprintf(text, sizeof(text), "%s", armed_button == i ? "SURE?" : "NEW");
		break;
	}
	if (armed_button == i) {
		fill = gc_rgb(200, 70, 60);
	}
	gamecanvas_rect(c, x - 2, y - 2, w + 4, BAR_H + 4, col_bg);
	gamecanvas_round_rect(c, x, y, w, BAR_H, 20, fill);
	int scale = gamecanvas_text_width(text, 3) <= w - 12 ? 3 : 2;
	gamecanvas_text_centered(c, x + w / 2, y + BAR_H / 2, text, scale, gc_rgb(255, 255, 255));
	gamecanvas_dirty(c, x - 2, y - 2, w + 4, BAR_H + 4);
}

static void draw_all(void) {
	gamecanvas_fill(&game->canvas, col_bg);
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < cols; x++) {
			draw_cell(x, y);
		}
	}
	for (int i = 0; i < BTN_COUNT; i++) {
		draw_button(i, false);
	}
	gk_dirty_all(game);
}

static void show_hud(void) {
	char a[40];
	char b[40];
	char t[16];
	snprintf(a, sizeof(a), "%s %d", tr("mines_mines"), mine_total - flags);
	fmt_time(t, sizeof(t), (state == ST_WON || state == ST_LOST ? final_ms : clock_ms) / 1000);
	snprintf(b, sizeof(b), "%s %s", tr("game_time"), t);
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
		n += snprintf(text + n, sizeof(text) - (size_t)n, "\n%s %s", tr(LEVEL_NAMES[i]), t);
	}
	gk_ui_message(&game->ui, text, 0);
}

static void layout(void) {
	cell = LEVELS[level].cell;
	cols = (game->w - 16) / cell;
	rows = (game->h - BAR_H - 20) / cell;
	if (cols > MAX_COLS) {
		cols = MAX_COLS;
	}
	if (rows > MAX_ROWS) {
		rows = MAX_ROWS;
	}
	gx = (game->w - cols * cell) / 2;
	gy = 6 + (game->h - BAR_H - 20 - rows * cell) / 2;
	mine_total = cols * rows * LEVELS[level].density / 100;
}

static void new_game(void) {
	armed_button = -1;
	layout();
	memset(is_mine, 0, sizeof(is_mine));
	memset(near_count, 0, sizeof(near_count));
	memset(shown, 0, sizeof(shown));
	state = ST_READY;
	flags = 0;
	opened = 0;
	clock_ms = 0;
	shown_seconds = 0;
	new_best = false;
	press_valid = false;
	gk_timer(game, false);
	draw_all();
	show_hud();
	start_message();
}

static void place_mines(int safe_x, int safe_y) {
	int placed = 0;
	int guard = 0;
	while (placed < mine_total && guard++ < 100000) {
		int x = rand() % cols;
		int y = rand() % rows;
		if (is_mine[y][x] || (abs(x - safe_x) <= 1 && abs(y - safe_y) <= 1)) {
			continue;
		}
		is_mine[y][x] = 1;
		placed++;
	}
	mine_total = placed;
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < cols; x++) {
			int n = 0;
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					int nx = x + dx;
					int ny = y + dy;
					if (nx >= 0 && ny >= 0 && nx < cols && ny < rows && is_mine[ny][nx]) {
						n++;
					}
				}
			}
			near_count[y][x] = (uint8_t)n;
		}
	}
}

static void finish(bool won) {
	final_ms = clock_ms;
	end_at = clock_ms; // the timer keeps running, to tell a tap meant for the board from one meant to start again
	if (won) {
		state = ST_WON;
		for (int y = 0; y < rows; y++) {
			for (int x = 0; x < cols; x++) {
				if (is_mine[y][x] && shown[y][x] != 2) {
					shown[y][x] = 2;
				}
			}
		}
		flags = mine_total;
		long secs = (long)(final_ms / 1000);
		if (secs < 1) {
			secs = 1;
		}
		new_best = highscore_submit(LEVELS[level].key, secs, true);
		gamesound_play(SND_WIN);
	} else {
		state = ST_LOST;
		gamesound_play(SND_BOOM);
	}
	draw_all();
	show_hud();

	char text[160];
	char t[16];
	fmt_time(t, sizeof(t), final_ms / 1000);
	int n;
	if (won) {
		n = snprintf(text, sizeof(text), "%s\n%s %s", tr("game_you_win"), tr("game_time"), t);
		if (new_best && n < (int)sizeof(text)) {
			n += snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", tr("game_new_best"));
		}
	} else {
		n = snprintf(text, sizeof(text), "%s", tr("game_over"));
	}
	snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", tr("game_tap_again"));
	// Over the bar's row, not the squares: the board is what the player wants to see.
	gk_ui_message(&game->ui, text, game->h / 2 - 120);
}

// Opens a square, and the ones around it while they are blank. Returns false on a mine.
static bool open_from(int x0, int y0) {
	static int stack[MAX_ROWS * MAX_COLS][2];
	int top = 0;
	stack[top][0] = x0;
	stack[top][1] = y0;
	top++;
	if (shown[y0][x0] == 2) {
		return true;
	}
	if (is_mine[y0][x0]) {
		boom_x = x0;
		boom_y = y0;
		shown[y0][x0] = 1;
		return false;
	}
	int count = 0;
	while (top > 0) {
		top--;
		int x = stack[top][0];
		int y = stack[top][1];
		if (shown[y][x] != 0) {
			continue;
		}
		shown[y][x] = 1;
		opened++;
		count++;
		draw_cell(x, y);
		if (near_count[y][x] == 0) {
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					int nx = x + dx;
					int ny = y + dy;
					if (nx >= 0 && ny >= 0 && nx < cols && ny < rows && shown[ny][nx] == 0 && !is_mine[ny][nx]) {
						stack[top][0] = nx;
						stack[top][1] = ny;
						top++;
					}
				}
			}
		}
	}
	gamesound_play(count > 3 ? SND_REVEAL : SND_TICK);
	return true;
}

static void dig(int x, int y) {
	if (state == ST_READY) {
		place_mines(x, y);
		state = ST_PLAY;
		gk_ui_message_hide(&game->ui);
		gk_timer(game, true);
	}
	if (shown[y][x] == 2) {
		return;
	}
	if (shown[y][x] == 1) {
		// A number with all its flags down: open what is left around it.
		int around = 0;
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				int nx = x + dx;
				int ny = y + dy;
				if (nx >= 0 && ny >= 0 && nx < cols && ny < rows && shown[ny][nx] == 2) {
					around++;
				}
			}
		}
		if (near_count[y][x] == 0 || around != near_count[y][x]) {
			return;
		}
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				int nx = x + dx;
				int ny = y + dy;
				if (nx >= 0 && ny >= 0 && nx < cols && ny < rows && shown[ny][nx] == 0) {
					if (!open_from(nx, ny)) {
						finish(false);
						return;
					}
				}
			}
		}
	} else if (!open_from(x, y)) {
		finish(false);
		return;
	}
	if (opened == cols * rows - mine_total) {
		finish(true);
	}
}

static void toggle_flag(int x, int y) {
	if (state == ST_READY || shown[y][x] == 1) {
		return;
	}
	if (shown[y][x] == 2) {
		shown[y][x] = 0;
		flags--;
	} else {
		shown[y][x] = 2;
		flags++;
	}
	gamesound_play(SND_FLAG);
	draw_cell(x, y);
	show_hud();
}

static bool cell_at(int x, int y, int *cx, int *cy) {
	if (x < gx || y < gy) {
		return false;
	}
	*cx = (x - gx) / cell;
	*cy = (y - gy) / cell;
	return *cx < cols && *cy < rows;
}

static int button_at(int x, int y) {
	if (y < bar_y() - 4 || y > bar_y() + BAR_H + 4) {
		return -1;
	}
	for (int i = 0; i < BTN_COUNT; i++) {
		if (x >= bar_x(i) - 4 && x < bar_x(i) + bar_w() + 4) {
			return i;
		}
	}
	return -1;
}

static void disarm(void) {
	int was = armed_button;
	armed_button = -1;
	if (was >= 0) {
		draw_button(was, false);
	}
}

static void press_button_action(int b) {
	if (b != BTN_LEVEL && b != BTN_NEW) {
		disarm();
	} else if (state == ST_PLAY && armed_button != b) {
		disarm();
		armed_button = b;
		armed_until = clock_ms + 2500;
		gamesound_play(SND_BLIP);
		draw_button(b, false);
		return;
	}
	armed_button = -1;
	if (b == BTN_LEVEL) {
		level = (level + 1) % 3;
		config_set_int("games", "mines_level", level);
		config_save();
		new_game();
	} else if (b == BTN_MODE) {
		flag_mode = !flag_mode;
		gamesound_play(SND_BLIP);
		draw_button(BTN_MODE, false);
	} else {
		new_game();
	}
}

static void touch(gk_game_t *g, gk_phase_t phase, int x, int y) {
	(void)g;
	if (phase == GK_DOWN) {
		press_valid = true;
		press_long = false;
		press_ms = clock_ms;
		press_button = button_at(x, y);
		press_col = press_row = -1;
		if (press_button >= 0) {
			draw_button(press_button, true);
		} else if (state == ST_PLAY || state == ST_READY) {
			cell_at(x, y, &press_col, &press_row);
		}
		return;
	}
	if (!press_valid) {
		return;
	}
	if (phase == GK_MOVE) {
		int cx = -1;
		int cy = -1;
		if (press_button >= 0) {
			if (button_at(x, y) != press_button) {
				draw_button(press_button, false);
				press_valid = false;
			}
		} else if (!cell_at(x, y, &cx, &cy) || cx != press_col || cy != press_row) {
			press_valid = false; // slid off: nothing happens
		}
		return;
	}
	// GK_UP
	press_valid = false;
	if (press_button >= 0) {
		press_button_action(press_button);
		return;
	}
	disarm(); // a tap on the board takes the question back
	if (state == ST_WON || state == ST_LOST) {
		if (clock_ms - end_at > 700) {
			new_game();
		}
		return;
	}
	if (press_long || press_col < 0) {
		return;
	}
	if (flag_mode) {
		if (shown[press_row][press_col] == 1) {
			dig(press_col, press_row);
		} else {
			toggle_flag(press_col, press_row);
		}
	} else {
		dig(press_col, press_row);
	}
}

static void tick(gk_game_t *g, uint32_t dt) {
	(void)g;
	clock_ms += dt;
	if (armed_button >= 0 && clock_ms >= armed_until) {
		disarm();
	}
	if (state != ST_PLAY) {
		return; // won or lost: the clock runs on, only to time the tap that starts again
	}
	int secs = (int)(clock_ms / 1000);
	if (secs != shown_seconds) {
		shown_seconds = secs;
		show_hud();
	}
	// A finger held on a covered square puts a flag on it.
	if (press_valid && !press_long && press_button < 0 && press_col >= 0 && clock_ms - press_ms >= HOLD_MS) {
		press_long = true;
		if (shown[press_row][press_col] != 1) {
			toggle_flag(press_col, press_row);
		}
	}
}

static void load_colours(void) {
	col_bg = gc_rgb(18, 20, 28);
	col_covered = gc_rgb(74, 86, 116);
	col_covered_hi = gc_rgb(104, 118, 154);
	col_covered_lo = gc_rgb(44, 52, 74);
	col_open = gc_rgb(34, 38, 52);
	col_text = gc_rgb(235, 238, 245);
	col_dim = gc_rgb(110, 118, 140);
	col_num[1] = gc_rgb(96, 170, 255);
	col_num[2] = gc_rgb(96, 205, 120);
	col_num[3] = gc_rgb(255, 104, 96);
	col_num[4] = gc_rgb(190, 120, 255);
	col_num[5] = gc_rgb(255, 170, 70);
	col_num[6] = gc_rgb(80, 220, 210);
	col_num[7] = gc_rgb(235, 235, 240);
	col_num[8] = gc_rgb(150, 156, 170);
}

static void open_game(gk_game_t *g) {
	game = g;
	srand((unsigned)time(NULL));
	load_colours();
	level = (int)config_get_int("games", "mines_level", 0);
	if (level < 0 || level > 2) {
		level = 0;
	}
	flag_mode = false;
	new_game();
}

static void restart(gk_game_t *g) {
	(void)g;
	new_game();
}

const gk_def_t mines_def = {
	.id = "mines",
	.tick_ms = 50,
	.open = open_game,
	.touch = touch,
	.tick = tick,
	.restart = restart,
};

void mines_init(gui_config_t *cfg) { gk_game_init(&game_object, mines_screen, cfg, &mines_def); }

#ifdef GAME_TEST
// For the host harness: opens every square that is safe, as a win.
void mines_test_cheat(void) {
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < cols; x++) {
			if (shown[y][x] == 2 && !is_mine[y][x]) {
				shown[y][x] = 0;
				flags--;
			}
			if (!is_mine[y][x] && shown[y][x] == 0 && state == ST_PLAY) {
				dig(x, y);
			}
		}
	}
}
#endif
