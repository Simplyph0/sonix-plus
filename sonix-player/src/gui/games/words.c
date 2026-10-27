#include "words.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/gui/games/wordlist.h"
#include "src/system/core/lang.h"

lv_obj_t *words_screen;

#define ROWS 6
#define LEN 5
#define GAP 6
#define KEY_H 50
#define KEY_GAP 5
#define REVEAL_MS 190

typedef enum {
	ST_READY,
	ST_PLAY,
	ST_WON,
	ST_LOST,
} state_t;

enum { R_NONE = 0, R_ABSENT = 1, R_PRESENT = 2, R_RIGHT = 3 };

static const char *const KEY_ROWS[3] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};

static gk_game_t game_object;
static gk_game_t *game;
static state_t state;
static char target[LEN + 1];
static char guess[ROWS][LEN + 1];
static uint8_t result[ROWS][LEN];
static uint8_t key_state[26];
static int row;
static int len; // letters typed in the row
static int reveal_row = -1;
static int reveal_idx;
static uint32_t reveal_acc;
static bool warned; // the word typed was refused once
static char note[40];
static long streak;
static long best_streak;
static uint32_t clock_ms;
static uint32_t end_at;
static bool new_best;

// Where things are.
static int tile;
static int gx;
static int gy;
static int kb_y;
static int key_w;

// A finger.
static int down_x;
static int down_y;

static uint16_t col_bg;
static uint16_t col_empty;
static uint16_t col_outline;
static uint16_t col_absent;
static uint16_t col_present;
static uint16_t col_right;
static uint16_t col_key;
static uint16_t col_text;

// ---------------------------------------------------------------------------
// Words
// ---------------------------------------------------------------------------

static bool in_list(const char (*list)[6], int n, const char *w) {
	int lo = 0;
	int hi = n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = strcmp(list[mid], w);
		if (c == 0) {
			return true;
		}
		if (c < 0) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return false;
}

static bool known_word(const char *upper) {
	char low[LEN + 1];
	for (int i = 0; i < LEN; i++) {
		low[i] = (char)(upper[i] + 32);
	}
	low[LEN] = '\0';
	return in_list(WORDS_ANSWERS, WORDS_ANSWER_COUNT, low) || in_list(WORDS_EXTRA, WORDS_EXTRA_COUNT, low);
}

static void score_guess(const char *g, uint8_t *out) {
	int left[26] = {0};
	for (int i = 0; i < LEN; i++) {
		out[i] = R_ABSENT;
		if (g[i] == target[i]) {
			out[i] = R_RIGHT;
		} else {
			left[target[i] - 'A']++;
		}
	}
	for (int i = 0; i < LEN; i++) {
		if (out[i] != R_RIGHT && left[g[i] - 'A'] > 0) {
			out[i] = R_PRESENT;
			left[g[i] - 'A']--;
		}
	}
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static int tile_x(int c) { return gx + c * (tile + GAP); }
static int tile_y(int r) { return gy + r * (tile + GAP); }

static void draw_tile(int r, int c) {
	gamecanvas_t *g = &game->canvas;
	int x = tile_x(c);
	int y = tile_y(r);
	gamecanvas_rect(g, x - 2, y - 2, tile + 4, tile + 4, col_bg);
	char letter = guess[r][c];
	bool shown = result[r][c] != R_NONE && !(r == reveal_row && c >= reveal_idx);
	if (shown) {
		uint16_t fill = result[r][c] == R_RIGHT ? col_right : result[r][c] == R_PRESENT ? col_present : col_absent;
		gamecanvas_round_rect(g, x, y, tile, tile, 8, fill);
	} else if (letter) {
		gamecanvas_round_rect(g, x, y, tile, tile, 8, gc_rgb(120, 128, 150));
		gamecanvas_round_rect(g, x + 2, y + 2, tile - 4, tile - 4, 6, col_empty);
	} else {
		gamecanvas_round_rect(g, x, y, tile, tile, 8, col_outline);
		gamecanvas_round_rect(g, x + 2, y + 2, tile - 4, tile - 4, 6, col_empty);
	}
	if (letter) {
		char t[2] = {letter, 0};
		int scale = tile >= 52 ? 5 : 4;
		gamecanvas_text_centered(g, x + tile / 2, y + tile / 2, t, scale, col_text);
	}
	gamecanvas_dirty(g, x - 2, y - 2, tile + 4, tile + 4);
}

static void key_rect(int r, int i, int *x, int *y, int *w) {
	int count = (int)strlen(KEY_ROWS[r]);
	int total_w = 10 * key_w + 9 * KEY_GAP;
	if (r == 2) {
		// ENTER and DEL a half-key wider than the letters, so the row is as wide as the top one
		int wide = (total_w - 7 * key_w - 8 * KEY_GAP) / 2;
		if (i == -1) {
			*x = (game->w - total_w) / 2;
			*w = wide;
		} else if (i == count) {
			*x = (game->w + total_w) / 2 - wide;
			*w = wide;
		} else {
			*x = (game->w - total_w) / 2 + wide + KEY_GAP + i * (key_w + KEY_GAP);
			*w = key_w;
		}
	} else {
		int row_w = count * key_w + (count - 1) * KEY_GAP;
		*x = (game->w - row_w) / 2 + i * (key_w + KEY_GAP);
		*w = key_w;
	}
	*y = kb_y + r * (KEY_H + KEY_GAP);
}

static void draw_key(int r, int i) {
	gamecanvas_t *g = &game->canvas;
	int x;
	int y;
	int w;
	key_rect(r, i, &x, &y, &w);
	char text[8];
	uint16_t fill = col_key;
	if (i == -1) {
		snprintf(text, sizeof(text), "ENTER");
	} else if (i == (int)strlen(KEY_ROWS[r])) {
		snprintf(text, sizeof(text), "DEL");
	} else {
		text[0] = KEY_ROWS[r][i];
		text[1] = '\0';
		int s = key_state[text[0] - 'A'];
		fill = s == R_RIGHT ? col_right : s == R_PRESENT ? col_present : s == R_ABSENT ? col_absent : col_key;
	}
	gamecanvas_rect(g, x - 1, y - 1, w + 2, KEY_H + 2, col_bg);
	gamecanvas_round_rect(g, x, y, w, KEY_H, 10, fill);
	int scale = text[1] ? 2 : 3;
	gamecanvas_text_centered(g, x + w / 2, y + KEY_H / 2, text, scale, col_text);
	gamecanvas_dirty(g, x - 1, y - 1, w + 2, KEY_H + 2);
}

static void draw_keyboard(void) {
	for (int r = 0; r < 3; r++) {
		int n = (int)strlen(KEY_ROWS[r]);
		for (int i = (r == 2 ? -1 : 0); i < n + (r == 2 ? 1 : 0); i++) {
			draw_key(r, i);
		}
	}
}

static void draw_note(void) {
	gamecanvas_t *g = &game->canvas;
	int y = gy + ROWS * (tile + GAP) - GAP + 6;
	int h = kb_y - y - 2;
	gamecanvas_rect(g, 0, y, game->w, h, col_bg);
	if (note[0]) {
		gamecanvas_text_centered(g, game->w / 2, y + h / 2, note, 2, gc_rgb(255, 150, 120));
	}
	gamecanvas_dirty(g, 0, y, game->w, h);
}

static void draw_all(void) {
	gamecanvas_fill(&game->canvas, col_bg);
	for (int r = 0; r < ROWS; r++) {
		for (int c = 0; c < LEN; c++) {
			draw_tile(r, c);
		}
	}
	draw_keyboard();
	draw_note();
	gk_dirty_all(game);
}

static void show_hud(void) {
	char a[40];
	char b[40];
	snprintf(a, sizeof(a), "%s %ld", tr("game_streak"), streak);
	snprintf(b, sizeof(b), "%s %ld", tr("game_best"), best_streak > streak ? best_streak : streak);
	gk_ui_score(&game->ui, a, b);
}

static void start_message(void) {
	char text[200];
	int n = snprintf(text, sizeof(text), "%s", tr("game_tap_start"));
	snprintf(text + n, sizeof(text) - (size_t)n, "\n%s %ld   %s %ld", tr("game_streak"), streak, tr("game_best"),
			 best_streak);
	n = (int)strlen(text);
	snprintf(text + n, sizeof(text) - (size_t)n, "\n%s %ld", tr("game_wins"), highscore_get("words.wins"));
	gk_ui_message(&game->ui, text, 0);
}

// ---------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------

static void ready(void) {
	state = ST_READY;
	memset(guess, 0, sizeof(guess));
	memset(result, 0, sizeof(result));
	memset(key_state, 0, sizeof(key_state));
	row = 0;
	len = 0;
	reveal_row = -1;
	warned = false;
	note[0] = '\0';
	new_best = false;
	streak = highscore_get("words.current");
	best_streak = highscore_get("words.streak");
	gk_timer(game, true); // it also times the tap that starts again, and the reveal
	draw_all();
	show_hud();
	start_message();
}

static void begin(void) {
	const char *w = WORDS_ANSWERS[rand() % WORDS_ANSWER_COUNT];
	for (int i = 0; i < LEN; i++) {
		target[i] = (char)(w[i] - 32);
	}
	target[LEN] = '\0';
	state = ST_PLAY;
	gk_ui_message_hide(&game->ui);
}

static void finish(bool won) {
	state = won ? ST_WON : ST_LOST;
	end_at = clock_ms;
	char text[160];
	if (won) {
		streak++;
		highscore_set("words.current", streak);
		new_best = highscore_submit("words.streak", streak, false);
		if (new_best) {
			best_streak = streak;
		}
		highscore_set("words.wins", highscore_get("words.wins") + 1);
		gamesound_play(SND_WIN);
		int n = snprintf(text, sizeof(text), "%s\n", tr("game_you_win"));
		n += snprintf(text + n, sizeof(text) - (size_t)n, tr("words_solved_in"), row);
		if (new_best && n < (int)sizeof(text)) {
			n += snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", tr("game_new_best"));
		}
		snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", tr("game_tap_again"));
	} else {
		streak = 0;
		highscore_set("words.current", 0);
		gamesound_play(SND_LOSE);
		int n = snprintf(text, sizeof(text), tr("words_word_was"), target);
		snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", tr("game_tap_again"));
	}
	show_hud();
	gk_ui_message(&game->ui, text, 40);
}

static void submit(void) {
	if (len < LEN) {
		snprintf(note, sizeof(note), "TOO SHORT");
		gamesound_play(SND_ERROR);
		draw_note();
		return;
	}
	if (!known_word(guess[row]) && !warned) {
		warned = true;
		snprintf(note, sizeof(note), "NOT IN LIST - ENTER AGAIN");
		gamesound_play(SND_ERROR);
		draw_note();
		return;
	}
	warned = false;
	note[0] = '\0';
	draw_note();
	score_guess(guess[row], result[row]);
	reveal_row = row;
	reveal_idx = 0;
	reveal_acc = 0;
	for (int c = 0; c < LEN; c++) {
		draw_tile(row, c); // covered until its turn in the reveal
	}
}

static void finish_reveal(void) {
	int r = reveal_row;
	reveal_row = -1;
	bool solved = true;
	for (int c = 0; c < LEN; c++) {
		int k = guess[r][c] - 'A';
		if (result[r][c] > key_state[k]) {
			key_state[k] = result[r][c];
		}
		solved = solved && result[r][c] == R_RIGHT;
	}
	row = r + 1;
	len = 0;
	draw_keyboard();
	if (solved) {
		finish(true);
	} else if (row >= ROWS) {
		finish(false);
	}
}

static void type_letter(char ch) {
	if (len >= LEN) {
		return;
	}
	guess[row][len++] = ch;
	warned = false;
	if (note[0]) {
		note[0] = '\0';
		draw_note();
	}
	gamesound_play(SND_TICK);
	draw_tile(row, len - 1);
}

static void backspace(void) {
	if (len <= 0) {
		return;
	}
	guess[row][--len] = '\0';
	warned = false;
	if (note[0]) {
		note[0] = '\0';
		draw_note();
	}
	draw_tile(row, len);
}

static void tap(int x, int y) {
	if (state == ST_READY) {
		begin();
		return;
	}
	if (state == ST_WON || state == ST_LOST) {
		if (clock_ms - end_at > 700) {
			ready();
			begin();
		}
		return;
	}
	if (reveal_row >= 0) {
		return;
	}
	for (int r = 0; r < 3; r++) {
		int n = (int)strlen(KEY_ROWS[r]);
		for (int i = (r == 2 ? -1 : 0); i < n + (r == 2 ? 1 : 0); i++) {
			int kx;
			int ky;
			int kw;
			key_rect(r, i, &kx, &ky, &kw);
			if (x >= kx - KEY_GAP / 2 && x < kx + kw + KEY_GAP / 2 && y >= ky - KEY_GAP / 2 && y < ky + KEY_H + KEY_GAP / 2) {
				if (i == -1) {
					submit();
				} else if (i == n) {
					backspace();
				} else {
					type_letter(KEY_ROWS[r][i]);
				}
				return;
			}
		}
	}
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
	if (reveal_row < 0) {
		return;
	}
	reveal_acc += dt;
	while (reveal_acc >= REVEAL_MS && reveal_row >= 0) {
		reveal_acc -= REVEAL_MS;
		int c = reveal_idx++;
		draw_tile(reveal_row, c);
		gamesound_play(result[reveal_row][c] == R_RIGHT ? SND_BLIP : SND_TICK);
		if (reveal_idx >= LEN) {
			finish_reveal();
		}
	}
}

static void layout(void) {
	key_w = (game->w - 12 - 9 * KEY_GAP) / 10;
	int kb_h = 3 * KEY_H + 2 * KEY_GAP;
	int avail = game->h - kb_h - 36 - 10;
	tile = (avail - (ROWS - 1) * GAP) / ROWS;
	if (tile > 62) {
		tile = 62;
	}
	int grid_w = LEN * tile + (LEN - 1) * GAP;
	int grid_h = ROWS * tile + (ROWS - 1) * GAP;
	gx = (game->w - grid_w) / 2;
	gy = 8;
	kb_y = game->h - kb_h - 6;
	(void)grid_h;
}

static void load_colours(void) {
	col_bg = gc_rgb(18, 20, 28);
	col_empty = gc_rgb(24, 27, 38);
	col_outline = gc_rgb(58, 64, 86);
	col_absent = gc_rgb(60, 66, 84);
	col_present = gc_rgb(190, 150, 40);
	col_right = gc_rgb(60, 160, 84);
	col_key = gc_rgb(96, 106, 136);
	col_text = gc_rgb(255, 255, 255);
}

static void open_game(gk_game_t *g) {
	game = g;
	srand((unsigned)time(NULL));
	load_colours();
	layout();
	clock_ms = 0;
	ready();
}

static void restart(gk_game_t *g) {
	(void)g;
	if (state == ST_PLAY && row > 0) {
		// leaving a game half way loses the streak, as the game would have
		streak = 0;
		highscore_set("words.current", 0);
	}
	ready();
	begin();
}

const gk_def_t words_def = {
	.id = "words",
	.tick_ms = 50,
	.open = open_game,
	.touch = touch_cb,
	.tick = tick,
	.restart = restart,
};

void words_init(gui_config_t *cfg) { gk_game_init(&game_object, words_screen, cfg, &words_def); }

#ifdef GAME_TEST
void words_test_set_target(const char *w) { snprintf(target, sizeof(target), "%s", w); }
const char *words_test_target(void) { return target; }
int words_test_known(const char *w) { return known_word(w); }
void words_test_type(const char *s) {
	while (*s) {
		type_letter(*s++);
	}
}
void words_test_enter(void) { submit(); }
void words_test_result(int r, char *out) {
	for (int c = 0; c < LEN; c++) {
		out[c] = (char)('0' + result[r][c]);
	}
	out[LEN] = 0;
}
#endif
