#include "breakout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/games/gamecanvas.h"
#include "src/gui/games/gamekit.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

lv_obj_t *breakout_screen;

#define TICK_MS 16
#define MAX_STEP_MS 40 // a tick later than this is taken as this long, not as long as it was

#define BRICK_COLS 10
#define BRICK_ROWS 6
#define BRICK_W 44
#define BRICK_H 20
#define BRICK_GAP 3
#define BRICKS_TOP 56

#define PADDLE_W 92
#define PADDLE_H 14
#define PADDLE_FROM_BOTTOM 56
#define BALL_R 7
#define LIVES_START 3
#define SERVE_DEAF_MS 250

// Pixels a second.
#define SPEED_START 330.0f
#define SPEED_PER_LEVEL 40.0f
#define SPEED_MAX 640.0f
// The steepest the ball leaves the paddle at, from the vertical, in degrees.
#define MAX_ANGLE 62.0f

typedef enum {
	ST_READY, // the ball sits on the paddle
	ST_PLAY,
	ST_OVER,
} state_t;

static gui_config_t *g_cfg;
static gamecanvas_t canvas;
static gk_ui_t ui;
static lv_timer_t *tick_timer;
static int field_w;
static int field_h;

static bool alive[BRICK_ROWS][BRICK_COLS];
static int bricks_left;
static float ball_x;
static float ball_y;
static float vx;
static float vy;
static float paddle_x; // centre
static float speed;
static state_t state;
static int score;
static int best;
static int lives;
static int level;
static uint32_t last_tick;
static uint32_t over_at;

// Colours.
static uint16_t col_bg;
static uint16_t col_paddle;
static uint16_t col_ball;
static uint16_t col_row[BRICK_ROWS];

static int brick_x(int c) { return (field_w - (BRICK_COLS * (BRICK_W + BRICK_GAP) - BRICK_GAP)) / 2 + c * (BRICK_W + BRICK_GAP); }
static int brick_y(int r) { return BRICKS_TOP + r * (BRICK_H + BRICK_GAP); }
static int paddle_y(void) { return field_h - PADDLE_FROM_BOTTOM; }

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void draw_brick(int r, int c) {
	int x = brick_x(c);
	int y = brick_y(r);
	gamecanvas_round_rect(&canvas, x, y, BRICK_W, BRICK_H, 4, col_row[r]);
	// A lighter top edge, so they look like bricks and not flat bars.
	gamecanvas_rect(&canvas, x + 3, y + 2, BRICK_W - 6, 2, gc_mix(col_row[r], gc_rgb(255, 255, 255), 90));
}

static void draw_paddle(void) {
	int x = (int)paddle_x - PADDLE_W / 2;
	gamecanvas_round_rect(&canvas, x, paddle_y(), PADDLE_W, PADDLE_H, 7, col_paddle);
}

static void draw_ball(void) { gamecanvas_circle(&canvas, (int)ball_x, (int)ball_y, BALL_R, col_ball); }

// Repaints a part of the field from what is under it: background, the bricks that
// reach it, the paddle, the ball.
static void redraw_region(int x, int y, int w, int h) {
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > field_w) {
		w = field_w - x;
	}
	if (y + h > field_h) {
		h = field_h - y;
	}
	if (w <= 0 || h <= 0) {
		return;
	}
	gamecanvas_rect(&canvas, x, y, w, h, col_bg);
	for (int r = 0; r < BRICK_ROWS; r++) {
		int by = brick_y(r);
		if (by + BRICK_H < y || by > y + h) {
			continue;
		}
		for (int c = 0; c < BRICK_COLS; c++) {
			int bx = brick_x(c);
			if (alive[r][c] && bx + BRICK_W >= x && bx <= x + w) {
				draw_brick(r, c);
			}
		}
	}
	draw_paddle();
	draw_ball();
	gamecanvas_dirty(&canvas, x, y, w, h);
}

static void redraw_all(void) {
	gamecanvas_fill(&canvas, col_bg);
	for (int r = 0; r < BRICK_ROWS; r++) {
		for (int c = 0; c < BRICK_COLS; c++) {
			if (alive[r][c]) {
				draw_brick(r, c);
			}
		}
	}
	draw_paddle();
	draw_ball();
	gamecanvas_dirty_all(&canvas);
}

static void update_labels(void) {
	char a[40];
	char b[40];
	snprintf(a, sizeof(a), "%s %d", tr("game_score"), score);
	snprintf(b, sizeof(b), "%s %d", tr("game_lives"), lives);
	gk_ui_score(&ui, a, b);
}

// ---------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------

static void new_wall(void) {
	for (int r = 0; r < BRICK_ROWS; r++) {
		for (int c = 0; c < BRICK_COLS; c++) {
			alive[r][c] = true;
		}
	}
	bricks_left = BRICK_ROWS * BRICK_COLS;
}

static void serve_ready(void) {
	state = ST_READY;
	ball_x = paddle_x;
	ball_y = paddle_y() - BALL_R - 1;
	vx = vy = 0;
	gk_ui_message_hide(&ui);
}

static void new_game(void) {
	score = 0;
	lives = LIVES_START;
	level = 0;
	speed = SPEED_START;
	paddle_x = field_w / 2.0f;
	new_wall();
	serve_ready();
	redraw_all();
	update_labels();
	gk_ui_message_ready(&ui, "game_tap_serve", NULL, best, 60);
}

static void launch(void) {
	// Up and a little to one side, so that the first bounce is not straight back.
	float angle = (float)((rand() % 41) - 20) * 3.14159265f / 180.0f;
	vx = speed * sinf(angle);
	vy = -speed * cosf(angle);
	state = ST_PLAY;
	last_tick = lv_tick_get();
	gk_ui_message_hide(&ui);
}

static void lose_life(void) {
	lives--;
	update_labels();
	if (lives <= 0) {
		state = ST_OVER;
		over_at = lv_tick_get();
		bool new_best = highscore_submit("breakout", score, false);
		if (new_best) {
			best = score;
		}
		gamesound_play(SND_LOSE);
		update_labels();
		gk_ui_message_over(&ui, score, new_best, 60);
		return;
	}
	gamesound_play(SND_ERROR);
	serve_ready();
	redraw_all();
	gk_ui_message(&ui, tr("game_tap_serve"), 60);
}

static void normalize_speed(void) {
	float len = sqrtf(vx * vx + vy * vy);
	if (len > 0.001f) {
		vx *= speed / len;
		vy *= speed / len;
	}
	// Never so flat that it takes ages to come down.
	if (fabsf(vy) < speed * 0.25f) {
		vy = (vy < 0 ? -1.0f : 1.0f) * speed * 0.25f;
		float len2 = sqrtf(vx * vx + vy * vy);
		vx *= speed / len2;
		vy *= speed / len2;
	}
}

// The ball against the bricks: the first one it overlaps is broken and the ball
// is sent back off the side it came in through.
static bool hit_bricks(float prev_x, float prev_y) {
	for (int r = 0; r < BRICK_ROWS; r++) {
		int by = brick_y(r);
		if (ball_y + BALL_R < by || ball_y - BALL_R > by + BRICK_H) {
			continue;
		}
		for (int c = 0; c < BRICK_COLS; c++) {
			if (!alive[r][c]) {
				continue;
			}
			int bx = brick_x(c);
			if (ball_x + BALL_R < bx || ball_x - BALL_R > bx + BRICK_W) {
				continue;
			}
			alive[r][c] = false;
			gamesound_play(SND_BRICK);
			bricks_left--;
			score += (BRICK_ROWS - r) * 10;
			if (speed < SPEED_MAX && (bricks_left % 10) == 0) {
				speed += 8.0f;
			}
			// Came in from the side or from above/below?
			bool from_side = (prev_x + BALL_R <= bx || prev_x - BALL_R >= bx + BRICK_W) &&
							 !(prev_y + BALL_R <= by || prev_y - BALL_R >= by + BRICK_H);
			if (from_side) {
				vx = -vx;
			} else {
				vy = -vy;
			}
			normalize_speed();
			redraw_region(bx - 1, by - 1, BRICK_W + 2, BRICK_H + 2);
			update_labels();
			return true;
		}
	}
	return false;
}

static void step(float dt) {
	float prev_x = ball_x;
	float prev_y = ball_y;
	ball_x += vx * dt;
	ball_y += vy * dt;

	if (ball_x < BALL_R) {
		ball_x = BALL_R;
		vx = fabsf(vx);
		gamesound_play(SND_TICK);
	} else if (ball_x > field_w - BALL_R) {
		ball_x = (float)(field_w - BALL_R);
		vx = -fabsf(vx);
		gamesound_play(SND_TICK);
	}
	if (ball_y < BALL_R) {
		ball_y = BALL_R;
		vy = fabsf(vy);
		gamesound_play(SND_TICK);
	}

	// The paddle: where on it the ball lands sets the angle it leaves at.
	float py = (float)paddle_y();
	if (vy > 0 && ball_y + BALL_R >= py && prev_y + BALL_R <= py + PADDLE_H &&
		fabsf(ball_x - paddle_x) <= PADDLE_W / 2.0f + BALL_R) {
		float offset = (ball_x - paddle_x) / (PADDLE_W / 2.0f);
		if (offset > 1) {
			offset = 1;
		} else if (offset < -1) {
			offset = -1;
		}
		float angle = offset * MAX_ANGLE * 3.14159265f / 180.0f;
		vx = speed * sinf(angle);
		vy = -speed * cosf(angle);
		ball_y = py - BALL_R;
		gamesound_play(SND_HIT);
	}

	hit_bricks(prev_x, prev_y);

	if (bricks_left == 0) {
		level++;
		speed = SPEED_START + SPEED_PER_LEVEL * (float)level;
		if (speed > SPEED_MAX) {
			speed = SPEED_MAX;
		}
		new_wall();
		serve_ready();
		redraw_all();
		gamesound_play(SND_WIN);
		gk_ui_message(&ui, tr("game_tap_serve"), 60);
		return;
	}
	if (ball_y - BALL_R > field_h) {
		lose_life();
	}
}

static void tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (state != ST_PLAY || ui.paused) {
		return;
	}
	uint32_t now = lv_tick_get();
	uint32_t elapsed = now - last_tick;
	last_tick = now;
	if (elapsed > MAX_STEP_MS) {
		elapsed = MAX_STEP_MS;
	}
	float dt = (float)elapsed / 1000.0f;

	float old_x = ball_x;
	float old_y = ball_y;
	step(dt);
	if (state != ST_PLAY) {
		lv_refr_now(NULL);
		return;
	}
	// Both places the ball was and is.
	int x0 = (int)fminf(old_x, ball_x) - BALL_R - 2;
	int y0 = (int)fminf(old_y, ball_y) - BALL_R - 2;
	int x1 = (int)fmaxf(old_x, ball_x) + BALL_R + 2;
	int y1 = (int)fmaxf(old_y, ball_y) + BALL_R + 2;
	redraw_region(x0, y0, x1 - x0, y1 - y0);
	// On the panel now, in step with the frame just drawn.
	lv_refr_now(NULL);
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------

static void move_paddle_to(float x) {
	float old = paddle_x;
	float lo = PADDLE_W / 2.0f;
	float hi = field_w - PADDLE_W / 2.0f;
	paddle_x = x < lo ? lo : x > hi ? hi : x;
	if (paddle_x == old) {
		return;
	}
	if (state == ST_READY) {
		ball_x = paddle_x; // the ball rides on it until it is served
	}
	int x0 = (int)fminf(old, paddle_x) - PADDLE_W / 2 - BALL_R - 2;
	int x1 = (int)fmaxf(old, paddle_x) + PADDLE_W / 2 + BALL_R + 2;
	redraw_region(x0, paddle_y() - BALL_R * 2 - 4, x1 - x0, PADDLE_H + BALL_R * 2 + 8);
	if (state != ST_PLAY) {
		lv_refr_now(NULL);
	}
}

static float touch_x(void) {
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	return (float)(p.x - lv_obj_get_x(canvas.image));
}

static float press_x;
static float press_y;
static bool moved_far;

static void pressed_cb(lv_event_t *e) {
	(void)e;
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	press_x = (float)p.x;
	press_y = (float)p.y;
	moved_far = false;
	if (state != ST_OVER) {
		move_paddle_to(touch_x());
	}
}

static void pressing_cb(lv_event_t *e) {
	(void)e;
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (fabsf((float)p.x - press_x) > 14 || fabsf((float)p.y - press_y) > 14) {
		moved_far = true;
	}
	if (state != ST_OVER) {
		move_paddle_to(touch_x());
	}
}

static void released_cb(lv_event_t *e) {
	(void)e;
	if (state == ST_READY) {
		launch();
	} else if (state == ST_OVER && lv_tick_elaps(over_at) > SERVE_DEAF_MS * 2) {
		new_game();
	}
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	int top = settingsrow_content_top(g_cfg);
	field_w = (int)g_cfg->screen_width;
	field_h = (int)g_cfg->screen_height - top - 6;
	if (!gamecanvas_create(&canvas, breakout_screen, 0, top, field_w, field_h, col_bg)) {
		gui_notify_popup("game_no_memory");
		return;
	}
	lv_obj_move_background(canvas.image);

	best = (int)highscore_get("breakout");
	gamesound_open();
	srand((unsigned)time(NULL));
	tick_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	new_game();
	power_hold_screen_on(true);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (tick_timer) {
		lv_timer_delete(tick_timer);
		tick_timer = NULL;
	}
	gamecanvas_destroy(&canvas);
	gamesound_close();
	gk_ui_reset(&ui);
	power_hold_screen_on(false);
}

// The pause menu: the ball stands still while it is up, and picks up where it was.
static void pause_cb(bool paused, void *user) {
	(void)user;
	if (!paused) {
		last_tick = lv_tick_get();
	}
}

static void restart_cb(void *user) {
	(void)user;
	if (canvas.px) {
		new_game();
	}
}

void breakout_init(gui_config_t *cfg) {
	g_cfg = cfg;
	col_bg = gc_rgb(18, 20, 32);
	col_paddle = gc_rgb(120, 200, 255);
	col_ball = gc_rgb(255, 255, 255);
	col_row[0] = gc_rgb(232, 70, 70);
	col_row[1] = gc_rgb(240, 130, 50);
	col_row[2] = gc_rgb(236, 190, 60);
	col_row[3] = gc_rgb(90, 190, 100);
	col_row[4] = gc_rgb(70, 150, 230);
	col_row[5] = gc_rgb(150, 100, 220);

	gk_ui_build(&ui, breakout_screen, cfg, "breakout", pause_cb, restart_cb, NULL);

	lv_obj_add_event_cb(breakout_screen, pressed_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(breakout_screen, pressing_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(breakout_screen, released_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(breakout_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(breakout_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
