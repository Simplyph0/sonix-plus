#include "snake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/games/gamecanvas.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

lv_obj_t *snake_screen;

#define CELL 24
#define COLS 20
#define START_LENGTH 3
#define SWIPE_PX 24
#define TAP_PX 12
// A game that has just ended does not restart on the tap that was meant for a
// turn.
#define DEAD_DEAF_MS 600

// The step, in milliseconds: slow to begin with, quicker with every apple, never
// quicker than this.
#define STEP_START_MS 190
#define STEP_FASTEST_MS 85
#define STEP_PER_APPLE_MS 4

typedef enum {
	ST_READY,
	ST_PLAY,
	ST_DEAD,
} state_t;

typedef enum {
	DIR_UP,
	DIR_RIGHT,
	DIR_DOWN,
	DIR_LEFT,
} dir_t;

static const int DX[4] = {0, 1, 0, -1};
static const int DY[4] = {-1, 0, 1, 0};

static gui_config_t *g_cfg;
static gamecanvas_t canvas;
static lv_obj_t *score_label;
static lv_obj_t *message;
static lv_timer_t *step_timer;
static int rows;

typedef struct {
	int8_t x;
	int8_t y;
} cell_pos_t;

static cell_pos_t *body; // ring buffer: the head at `head`, `length` cells back from it
static int body_capacity;
static int head_index;
static int length;
static uint8_t *occupied; // rows * COLS: the snake is on that cell
static int food_x;
static int food_y;
static dir_t heading;
static dir_t queued[2];
static int queued_count;
static state_t state;
static int score;
static int best;
static uint32_t died_at;

// Pointer tracking for swipes.
static int origin_x;
static int origin_y;
static int moved;

// The look.
static uint16_t col_field_a;
static uint16_t col_field_b;
static uint16_t col_body;
static uint16_t col_body_edge;
static uint16_t col_head;
static uint16_t col_dead;
static uint16_t col_food;
static uint16_t col_leaf;

static cell_pos_t body_at(int back) { return body[(head_index - back + body_capacity) % body_capacity]; }

static void paint_cell(int x, int y) {
	uint16_t bg = ((x + y) & 1) ? col_field_b : col_field_a;
	gamecanvas_rect(&canvas, x * CELL, y * CELL, CELL, CELL, bg);
	if (occupied[y * COLS + x]) {
		bool is_head = length > 0 && body_at(0).x == x && body_at(0).y == y;
		uint16_t fill = is_head ? (state == ST_DEAD ? col_dead : col_head) : col_body;
		gamecanvas_round_rect(&canvas, x * CELL + 1, y * CELL + 1, CELL - 2, CELL - 2, 6, is_head ? fill : col_body_edge);
		if (!is_head) {
			gamecanvas_round_rect(&canvas, x * CELL + 3, y * CELL + 3, CELL - 6, CELL - 6, 5, fill);
		} else {
			// The eyes look the way the snake is going.
			int ex = x * CELL + CELL / 2;
			int ey = y * CELL + CELL / 2;
			int sx = DY[heading] != 0 ? 5 : 0;
			int sy = DX[heading] != 0 ? 5 : 0;
			int fx = DX[heading] * 4;
			int fy = DY[heading] * 4;
			gamecanvas_circle(&canvas, ex + fx - sx, ey + fy - sy, 3, gc_rgb(255, 255, 255));
			gamecanvas_circle(&canvas, ex + fx + sx, ey + fy + sy, 3, gc_rgb(255, 255, 255));
			gamecanvas_circle(&canvas, ex + fx - sx + DX[heading], ey + fy - sy + DY[heading], 1, gc_rgb(20, 20, 20));
			gamecanvas_circle(&canvas, ex + fx + sx + DX[heading], ey + fy + sy + DY[heading], 1, gc_rgb(20, 20, 20));
		}
	} else if (x == food_x && y == food_y) {
		gamecanvas_circle(&canvas, x * CELL + CELL / 2, y * CELL + CELL / 2 + 1, CELL / 2 - 3, col_food);
		gamecanvas_rect(&canvas, x * CELL + CELL / 2 - 1, y * CELL + 2, 2, 5, col_leaf);
	}
	gamecanvas_dirty(&canvas, x * CELL, y * CELL, CELL, CELL);
}

static void paint_all(void) {
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < COLS; x++) {
			paint_cell(x, y);
		}
	}
}

static void update_score(void) {
	char text[64];
	snprintf(text, sizeof(text), "%s %d   %s %d", tr("game_score"), score, tr("game_best"), best);
	lv_label_set_text(score_label, text);
}

static void show_message(const char *key, const char *second) {
	char text[160];
	if (second) {
		snprintf(text, sizeof(text), "%s\n%s", tr(key), tr(second));
	} else {
		snprintf(text, sizeof(text), "%s", tr(key));
	}
	lv_label_set_text(message, text);
	lv_obj_set_hidden(message, false);
	lv_obj_align(message, LV_ALIGN_CENTER, 0, 0);
}

static void place_food(void) {
	int free_cells = rows * COLS - length;
	if (free_cells <= 0) {
		food_x = food_y = -1;
		return;
	}
	int pick = rand() % free_cells;
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < COLS; x++) {
			if (occupied[y * COLS + x]) {
				continue;
			}
			if (pick-- == 0) {
				food_x = x;
				food_y = y;
				return;
			}
		}
	}
}

static void new_game(void) {
	memset(occupied, 0, (size_t)rows * COLS);
	length = 0;
	head_index = 0;
	heading = DIR_RIGHT;
	queued_count = 0;
	score = 0;
	int y = rows / 2;
	int x = COLS / 2 - START_LENGTH / 2;
	for (int i = 0; i < START_LENGTH; i++) {
		head_index = (head_index + 1) % body_capacity;
		body[head_index].x = (int8_t)(x + i);
		body[head_index].y = (int8_t)y;
		occupied[y * COLS + x + i] = 1;
		length++;
	}
	place_food();
	state = ST_READY;
	lv_timer_set_period(step_timer, STEP_START_MS);
	lv_timer_pause(step_timer);
	paint_all();
	update_score();
	show_message("game_tap_start", "snake_hint");
}

static void start_play(void) {
	lv_obj_set_hidden(message, true);
	state = ST_PLAY;
	lv_timer_resume(step_timer);
	lv_timer_reset(step_timer);
}

static void die(void) {
	state = ST_DEAD;
	lv_timer_pause(step_timer);
	died_at = lv_tick_get();
	if (score > best) {
		best = score;
		config_set_int("snake", "best", best);
		config_save();
	}
	paint_cell(body_at(0).x, body_at(0).y);
	update_score();
	show_message("game_over", "game_tap_again");
}

static dir_t last_direction(void) { return queued_count > 0 ? queued[queued_count - 1] : heading; }

// A turn is taken at the next step; one more may wait behind it so that two quick
// swipes make a U-turn in two steps instead of losing the second.
static void request_turn(dir_t d) {
	dir_t last = last_direction();
	if (d == last || (d + 2) % 4 == last) {
		return; // the way it already goes, or straight back into itself
	}
	if (queued_count < 2) {
		queued[queued_count++] = d;
	}
}

static void step_cb(lv_timer_t *timer) {
	(void)timer;
	if (state != ST_PLAY) {
		return;
	}
	if (queued_count > 0) {
		heading = queued[0];
		queued[0] = queued[1];
		queued_count--;
	}

	cell_pos_t head = body_at(0);
	int nx = head.x + DX[heading];
	int ny = head.y + DY[heading];
	if (nx < 0 || ny < 0 || nx >= COLS || ny >= rows) {
		die();
		return;
	}
	bool eats = nx == food_x && ny == food_y;
	cell_pos_t tail = body_at(length - 1);
	// The tail cell is free again by the time the head gets there, unless the
	// snake grows and the tail stays.
	if (occupied[ny * COLS + nx] && !(!eats && tail.x == nx && tail.y == ny)) {
		die();
		return;
	}

	if (!eats) {
		occupied[tail.y * COLS + tail.x] = 0;
		length--;
		paint_cell(tail.x, tail.y);
	}
	head_index = (head_index + 1) % body_capacity;
	body[head_index].x = (int8_t)nx;
	body[head_index].y = (int8_t)ny;
	occupied[ny * COLS + nx] = 1;
	length++;
	paint_cell(head.x, head.y); // no longer the head
	paint_cell(nx, ny);

	if (eats) {
		score++;
		int period = STEP_START_MS - score * STEP_PER_APPLE_MS;
		lv_timer_set_period(step_timer, period < STEP_FASTEST_MS ? STEP_FASTEST_MS : (uint32_t)period);
		place_food();
		if (food_x >= 0) {
			paint_cell(food_x, food_y);
		}
		update_score();
		if (food_x < 0) {
			die(); // the whole field is snake: nothing left to eat
		}
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
	moved = 0;
}

static void pressing_cb(lv_event_t *e) {
	(void)e;
	if (state != ST_PLAY) {
		return;
	}
	int x;
	int y;
	get_point(&x, &y);
	int dx = x - origin_x;
	int dy = y - origin_y;
	if (abs(dx) < SWIPE_PX && abs(dy) < SWIPE_PX) {
		return;
	}
	moved = 1000;
	if (abs(dx) >= abs(dy)) {
		request_turn(dx > 0 ? DIR_RIGHT : DIR_LEFT);
	} else {
		request_turn(dy > 0 ? DIR_DOWN : DIR_UP);
	}
	// The next turn is measured from here, so a long drag can steer twice.
	origin_x = x;
	origin_y = y;
}

static void released_cb(lv_event_t *e) {
	(void)e;
	int x;
	int y;
	get_point(&x, &y);
	bool tap = moved < 1000 && abs(x - origin_x) < TAP_PX && abs(y - origin_y) < TAP_PX;
	if (!tap) {
		return;
	}
	if (state == ST_READY) {
		start_play();
	} else if (state == ST_DEAD && lv_tick_elaps(died_at) > DEAD_DEAF_MS) {
		new_game();
	}
}

// Before the first step a swipe both turns and starts, which is what a hand
// expects.
static void ready_swipe_cb(lv_event_t *e) {
	(void)e;
	if (state != ST_READY) {
		return;
	}
	int x;
	int y;
	get_point(&x, &y);
	int dx = x - origin_x;
	int dy = y - origin_y;
	if (abs(dx) < SWIPE_PX && abs(dy) < SWIPE_PX) {
		return;
	}
	start_play();
	pressing_cb(e);
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

static void load_colours(void) {
	col_field_a = gc_rgb(26, 42, 34);
	col_field_b = gc_rgb(31, 49, 40);
	col_body_edge = gc_rgb(46, 150, 80);
	col_body = gc_rgb(88, 205, 116);
	col_head = gc_rgb(150, 235, 130);
	col_dead = gc_rgb(232, 80, 70);
	col_food = gc_rgb(232, 64, 64);
	col_leaf = gc_rgb(110, 200, 90);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	int top = settingsrow_content_top(g_cfg);
	rows = ((int)g_cfg->screen_height - top - 6) / CELL;
	int field_h = rows * CELL;
	int field_x = ((int)g_cfg->screen_width - COLS * CELL) / 2;

	body_capacity = COLS * rows + 1;
	body = malloc(sizeof(*body) * (size_t)body_capacity);
	occupied = malloc((size_t)COLS * rows);
	if (!body || !occupied || !gamecanvas_create(&canvas, snake_screen, field_x, top, COLS * CELL, field_h, col_field_a)) {
		free(body);
		free(occupied);
		body = NULL;
		occupied = NULL;
		gui_notify_popup("game_no_memory");
		return;
	}
	lv_obj_move_background(canvas.image);

	best = (int)config_get_int("snake", "best", 0);
	srand((unsigned)time(NULL));
	step_timer = lv_timer_create(step_cb, STEP_START_MS, NULL);
	lv_timer_pause(step_timer);
	new_game();
	power_hold_screen_on(true); // a game is played with the screen on and nobody touching it
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (step_timer) {
		lv_timer_delete(step_timer);
		step_timer = NULL;
	}
	gamecanvas_destroy(&canvas);
	free(body);
	free(occupied);
	body = NULL;
	occupied = NULL;
	lv_obj_set_hidden(message, true);
	power_hold_screen_on(false);
}

void snake_init(gui_config_t *cfg) {
	g_cfg = cfg;
	load_colours();

	lv_obj_add_style(snake_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(snake_screen, false);
	lv_obj_t *title = settingsrow_title(snake_screen, cfg, "snake");
	settingsrow_title_corner_slots(title, cfg, 3);

	// The score on the heading's line, at the right.
	score_label = lv_label_create(snake_screen);
	lv_obj_add_style(score_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(score_label, &font_ui_24_bold, 0);
	lv_obj_set_style_text_align(score_label, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_width(score_label, 250);
	lv_obj_align(score_label, LV_ALIGN_TOP_RIGHT, -(int)cfg->padding, (int)cfg->top_bar_height + (int)cfg->padding + 16);
	lv_label_set_text(score_label, "");

	message = lv_label_create(snake_screen);
	lv_obj_set_style_text_font(message, &font_ui_32, 0);
	lv_obj_set_style_text_color(message, lv_color_white(), 0);
	lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_bg_color(message, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(message, LV_OPA_60, 0);
	lv_obj_set_style_pad_all(message, 18, 0);
	lv_obj_set_style_radius(message, 16, 0);
	lv_obj_set_hidden(message, true);

	lv_obj_add_event_cb(snake_screen, pressed_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(snake_screen, pressing_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(snake_screen, ready_swipe_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(snake_screen, released_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(snake_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(snake_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
