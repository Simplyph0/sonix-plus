#include "blocks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/lang.h"

lv_obj_t *blocks_screen;

#define COLS 10
#define ROWS 20
#define PIECES 7
#define FLASH_MS 160
#define TAP_MS 260
#define FLICK_MS 320

typedef enum {
	ST_READY,
	ST_PLAY,
	ST_OVER,
} state_t;

// A piece in the corner of a box of `size` cells; turning it clockwise sends (x, y)
// to (size - 1 - y, x).
typedef struct {
	int size;
	int8_t cell[4][2];
} piece_t;

static const piece_t PIECE[PIECES] = {
	{4, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}}, // I
	{2, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}}, // O
	{3, {{1, 0}, {0, 1}, {1, 1}, {2, 1}}}, // T
	{3, {{1, 0}, {2, 0}, {0, 1}, {1, 1}}}, // S
	{3, {{0, 0}, {1, 0}, {1, 1}, {2, 1}}}, // Z
	{3, {{0, 0}, {0, 1}, {1, 1}, {2, 1}}}, // J
	{3, {{2, 0}, {0, 1}, {1, 1}, {2, 1}}}, // L
};

static gk_game_t game_object;
static gk_game_t *game; // the running page, set when it opens
static state_t state;
static uint8_t board[ROWS][COLS]; // 0 empty, else the piece's number + 1
static int cur;
static int rot;
static int px;
static int py;
static int next_piece;
static int bag[PIECES];
static int bag_left;
static long score;
static long best;
static bool new_best;
static int lines;
static int level;
static uint32_t gravity_acc;
static uint32_t now_ms;
static uint32_t over_at;
static int flash_rows[4];
static int flash_count;
static uint32_t flash_left;

// The look.
static int cell;
static int bx;
static int by;
static int side_x;
static int side_w;
static uint16_t col_bg;
static uint16_t col_bg2;
static uint16_t col_text;
static uint16_t col_dim;
static uint16_t col_piece[PIECES];

// A finger.
static int down_x;
static int down_y;
static int last_x;
static int anchor_y;
static int drag_x;
static uint32_t down_at;
static bool dragged;

static void cells_of(int type, int r, int out[4][2]) {
	const piece_t *p = &PIECE[type];
	for (int i = 0; i < 4; i++) {
		int x = p->cell[i][0];
		int y = p->cell[i][1];
		for (int k = 0; k < r; k++) {
			int nx = p->size - 1 - y;
			int ny = x;
			x = nx;
			y = ny;
		}
		out[i][0] = x;
		out[i][1] = y;
	}
}

static bool fits(int type, int r, int x, int y) {
	int c[4][2];
	cells_of(type, r, c);
	for (int i = 0; i < 4; i++) {
		int cx = x + c[i][0];
		int cy = y + c[i][1];
		if (cx < 0 || cx >= COLS || cy >= ROWS) {
			return false;
		}
		if (cy >= 0 && board[cy][cx]) {
			return false;
		}
	}
	return true;
}

static int take_from_bag(void) {
	if (bag_left == 0) {
		for (int i = 0; i < PIECES; i++) {
			bag[i] = i;
		}
		for (int i = PIECES - 1; i > 0; i--) {
			int j = rand() % (i + 1);
			int t = bag[i];
			bag[i] = bag[j];
			bag[j] = t;
		}
		bag_left = PIECES;
	}
	return bag[--bag_left];
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static uint16_t lighter(uint16_t c) { return gc_mix(c, gc_rgb(255, 255, 255), 90); }
static uint16_t darker(uint16_t c) { return gc_mix(c, gc_rgb(0, 0, 0), 80); }

static void draw_block(gamecanvas_t *c, int x, int y, int size, uint16_t col) {
	int r = size / 6 + 1;
	gamecanvas_round_rect(c, x + 1, y + 1, size - 2, size - 2, r, darker(col));
	gamecanvas_round_rect(c, x + 1, y + 1, size - 3, size - 3, r, col);
	gamecanvas_rect(c, x + 3, y + 3, size - 8, 2, lighter(col));
}

static void draw_ghost_block(gamecanvas_t *c, int x, int y, int size, uint16_t col) {
	uint16_t edge = gc_mix(col_bg, col, 150);
	gamecanvas_round_rect(c, x + 1, y + 1, size - 2, size - 2, size / 6 + 1, edge);
	gamecanvas_round_rect(c, x + 4, y + 4, size - 8, size - 8, size / 8, col_bg);
}

static int drop_row(void) {
	int y = py;
	while (fits(cur, rot, px, y + 1)) {
		y++;
	}
	return y;
}

static void draw_text_right(gamecanvas_t *c, int x_right, int y, const char *text, int scale, uint16_t col) {
	gamecanvas_text(c, x_right - gamecanvas_text_width(text, scale), y, text, scale, col);
}

static void render(void) {
	gamecanvas_t *c = &game->canvas;
	gamecanvas_fill(c, col_bg);

	// The well.
	gamecanvas_round_rect(c, bx - 4, by - 4, COLS * cell + 8, ROWS * cell + 8, 8, col_dim);
	for (int x = 0; x < COLS; x++) {
		gamecanvas_rect(c, bx + x * cell, by, cell, ROWS * cell, (x & 1) ? col_bg2 : col_bg);
	}
	for (int y = 0; y < ROWS; y++) {
		for (int x = 0; x < COLS; x++) {
			if (board[y][x]) {
				draw_block(c, bx + x * cell, by + y * cell, cell, col_piece[board[y][x] - 1]);
			}
		}
	}

	if (state == ST_PLAY && flash_count == 0) {
		int cs[4][2];
		cells_of(cur, rot, cs);
		int gy = drop_row();
		if (gy != py) {
			for (int i = 0; i < 4; i++) {
				if (gy + cs[i][1] >= 0) {
					draw_ghost_block(c, bx + (px + cs[i][0]) * cell, by + (gy + cs[i][1]) * cell, cell, col_piece[cur]);
				}
			}
		}
		for (int i = 0; i < 4; i++) {
			if (py + cs[i][1] >= 0) {
				draw_block(c, bx + (px + cs[i][0]) * cell, by + (py + cs[i][1]) * cell, cell, col_piece[cur]);
			}
		}
	}
	for (int i = 0; i < flash_count; i++) {
		gamecanvas_rect(c, bx, by + flash_rows[i] * cell, COLS * cell, cell, gc_rgb(255, 255, 255));
	}

	// The side: the next piece, then lines and level.
	int sx = side_x;
	int scale = side_w >= 150 ? 3 : 2;
	gamecanvas_text(c, sx, by, "NEXT", scale, col_dim);
	int pv = side_w / 5;
	if (pv > 30) {
		pv = 30;
	}
	int box = pv * 4 + 16;
	gamecanvas_round_rect(c, sx, by + 7 * scale + 10, box, box, 10, col_bg2);
	if (state != ST_READY) {
		int cs[4][2];
		cells_of(next_piece, 0, cs);
		int size = PIECE[next_piece].size;
		// centred in the box
		int minx = 9;
		int maxx = -9;
		int miny = 9;
		int maxy = -9;
		for (int i = 0; i < 4; i++) {
			minx = cs[i][0] < minx ? cs[i][0] : minx;
			maxx = cs[i][0] > maxx ? cs[i][0] : maxx;
			miny = cs[i][1] < miny ? cs[i][1] : miny;
			maxy = cs[i][1] > maxy ? cs[i][1] : maxy;
		}
		(void)size;
		int ox = sx + (box - (maxx - minx + 1) * pv) / 2 - minx * pv;
		int oy = by + 7 * scale + 10 + (box - (maxy - miny + 1) * pv) / 2 - miny * pv;
		for (int i = 0; i < 4; i++) {
			draw_block(c, ox + cs[i][0] * pv, oy + cs[i][1] * pv, pv, col_piece[next_piece]);
		}
	}
	int y2 = by + 7 * scale + 10 + box + 28;
	char text[24];
	gamecanvas_text(c, sx, y2, "LINES", scale, col_dim);
	snprintf(text, sizeof(text), "%d", lines);
	gamecanvas_text(c, sx, y2 + 7 * scale + 8, text, scale + 1, col_text);
	y2 += 7 * scale + 8 + 7 * (scale + 1) + 24;
	gamecanvas_text(c, sx, y2, "LEVEL", scale, col_dim);
	snprintf(text, sizeof(text), "%d", level);
	gamecanvas_text(c, sx, y2 + 7 * scale + 8, text, scale + 1, col_text);
	(void)draw_text_right;

	gk_dirty_all(game);
}

static void show_score(void) { gk_ui_score_best(&game->ui, score, best > score ? best : score); }

// ---------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------

static void new_game(void) {
	memset(board, 0, sizeof(board));
	bag_left = 0;
	score = 0;
	lines = 0;
	level = 1;
	new_best = false;
	flash_count = 0;
	gravity_acc = 0;
	next_piece = take_from_bag();
	best = highscore_get("blocks");
	state = ST_READY;
	gk_timer(game, false);
	show_score();
	render();
	gk_ui_message_ready(&game->ui, "game_tap_start", "blocks_hint", best, 0);
}

static void game_over(void) {
	state = ST_OVER;
	over_at = now_ms; // the timer runs on, to time the tap that starts again
	new_best = highscore_submit("blocks", score, false);
	if (new_best) {
		best = score;
	}
	gamesound_play(SND_LOSE);
	show_score();
	render();
	gk_ui_message_over(&game->ui, score, new_best, 0);
}

static void spawn(void) {
	cur = next_piece;
	next_piece = take_from_bag();
	rot = 0;
	px = (COLS - PIECE[cur].size) / 2;
	py = cur == 0 ? -1 : 0;
	gravity_acc = 0;
	if (!fits(cur, rot, px, py)) {
		game_over();
	}
}

static uint32_t gravity_ms(void) {
	int ms = 800 - (level - 1) * 65;
	return (uint32_t)(ms < 70 ? 70 : ms);
}

static void collapse(void) {
	int w = ROWS - 1;
	for (int y = ROWS - 1; y >= 0; y--) {
		bool full = true;
		for (int x = 0; x < COLS; x++) {
			full = full && board[y][x];
		}
		if (!full) {
			if (w != y) {
				memcpy(board[w], board[y], COLS);
			}
			w--;
		}
	}
	while (w >= 0) {
		memset(board[w--], 0, COLS);
	}
}

static void lock_piece(void) {
	int cs[4][2];
	cells_of(cur, rot, cs);
	for (int i = 0; i < 4; i++) {
		int y = py + cs[i][1];
		if (y >= 0) {
			board[y][px + cs[i][0]] = (uint8_t)(cur + 1);
		}
	}
	flash_count = 0;
	for (int y = 0; y < ROWS; y++) {
		bool full = true;
		for (int x = 0; x < COLS; x++) {
			full = full && board[y][x];
		}
		if (full && flash_count < 4) {
			flash_rows[flash_count++] = y;
		}
	}
	if (flash_count > 0) {
		static const int PTS[5] = {0, 100, 300, 500, 800};
		score += (long)PTS[flash_count] * level;
		lines += flash_count;
		level = 1 + lines / 10;
		flash_left = FLASH_MS;
		gamesound_play(SND_LINE);
	} else {
		gamesound_play(SND_DROP);
		spawn();
	}
	show_score();
	render();
}

static bool move(int dx) {
	if (fits(cur, rot, px + dx, py)) {
		px += dx;
		gamesound_play(SND_TICK);
		return true;
	}
	return false;
}

static void rotate(void) {
	int nr = (rot + 1) & 3;
	static const int KICKS[5] = {0, -1, 1, -2, 2};
	for (int i = 0; i < 5; i++) {
		if (fits(cur, nr, px + KICKS[i], py)) {
			rot = nr;
			px += KICKS[i];
			gamesound_play(SND_ROTATE);
			return;
		}
	}
}

static bool step_down(bool by_hand) {
	if (fits(cur, rot, px, py + 1)) {
		py++;
		if (by_hand) {
			score++;
		}
		gravity_acc = 0;
		return true;
	}
	return false;
}

static void hard_drop(void) {
	int rows = 0;
	while (fits(cur, rot, px, py + 1)) {
		py++;
		rows++;
	}
	score += 2L * rows;
	lock_piece();
}

static void begin_play(void) {
	state = ST_PLAY;
	gk_ui_message_hide(&game->ui);
	spawn();
	gk_timer(game, true);
	render();
}

static void tick(gk_game_t *g, uint32_t dt) {
	(void)g;
	now_ms += dt;
	if (state != ST_PLAY) {
		return;
	}
	if (flash_count > 0) {
		if (dt >= flash_left) {
			collapse();
			flash_count = 0;
			flash_left = 0;
			spawn();
			if (state == ST_PLAY) {
				render();
			}
		} else {
			flash_left -= dt;
		}
		return;
	}
	gravity_acc += dt;
	if (gravity_acc >= gravity_ms()) {
		gravity_acc = 0;
		if (!step_down(false)) {
			lock_piece();
			return;
		}
		render();
	}
}

static void touch(gk_game_t *g, gk_phase_t phase, int x, int y) {
	(void)g;
	if (phase == GK_DOWN) {
		down_x = last_x = x;
		down_y = anchor_y = y;
		drag_x = 0;
		down_at = now_ms;
		dragged = false;
		return;
	}
	if (state == ST_READY || state == ST_OVER) {
		if (phase == GK_UP && !dragged && (state == ST_READY || now_ms - over_at > 650)) {
			if (state == ST_OVER) {
				new_game();
			}
			begin_play();
		}
		return;
	}
	if (flash_count > 0) {
		return;
	}
	if (phase == GK_MOVE) {
		drag_x += x - last_x;
		last_x = x;
		bool changed = false;
		while (drag_x >= cell && move(1)) {
			drag_x -= cell;
			dragged = changed = true;
		}
		while (drag_x <= -cell && move(-1)) {
			drag_x += cell;
			dragged = changed = true;
		}
		if (drag_x >= cell || drag_x <= -cell) {
			drag_x = 0; // against the wall: nothing builds up
		}
		while (y - anchor_y >= cell) {
			anchor_y += cell;
			dragged = true;
			if (!step_down(true)) {
				break;
			}
			changed = true;
		}
		if (abs(x - down_x) > cell / 2 || abs(y - down_y) > cell / 2) {
			dragged = true;
		}
		if (changed) {
			render();
			show_score();
		}
		return;
	}
	// GK_UP
	uint32_t held = now_ms - down_at;
	int dy = y - down_y;
	int dx = x - down_x;
	if (dy >= cell * 4 && held < FLICK_MS && abs(dx) < cell * 2) {
		hard_drop();
	} else if (!dragged && held < TAP_MS + 200) {
		rotate();
		render();
	}
}

static void load_colours(void) {
	col_bg = gc_rgb(18, 20, 28);
	col_bg2 = gc_rgb(23, 26, 36);
	col_text = gc_rgb(235, 238, 245);
	col_dim = gc_rgb(110, 118, 140);
	col_piece[0] = gc_rgb(64, 208, 232); // I
	col_piece[1] = gc_rgb(245, 205, 60); // O
	col_piece[2] = gc_rgb(176, 98, 224); // T
	col_piece[3] = gc_rgb(90, 214, 110); // S
	col_piece[4] = gc_rgb(236, 82, 82);	 // Z
	col_piece[5] = gc_rgb(76, 120, 240); // J
	col_piece[6] = gc_rgb(248, 150, 54); // L
}

static void open_game(gk_game_t *g) {
	game = g;
	load_colours();
	srand((unsigned)time(NULL));
	int cw = (game->w - 190) / COLS;
	int ch = (game->h - 16) / ROWS;
	cell = cw < ch ? cw : ch;
	if (cell < 12) {
		cell = 12;
	}
	bx = 12;
	by = (game->h - cell * ROWS) / 2;
	side_x = bx + cell * COLS + 22;
	side_w = game->w - side_x - 8;
	new_game();
}

static void restart(gk_game_t *g) {
	(void)g;
	new_game();
}

const gk_def_t blocks_def = {
	.id = "blocks",
	.tick_ms = 16,
	.open = open_game,
	.touch = touch,
	.tick = tick,
	.restart = restart,
};

void blocks_init(gui_config_t *cfg) {
	gk_game_init(&game_object, blocks_screen, cfg, &blocks_def);
}
