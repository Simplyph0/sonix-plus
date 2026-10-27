#include "rally.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/lang.h"

lv_obj_t *rally_screen;

#define LIVES 3
#define PADDLE_H 16
#define BALL_R 9
#define SPEED_START 340.0f
#define SPEED_STEP 16.0f
#define SPEED_MAX 780.0f
#define DEAF_MS 650

typedef enum {
	ST_READY,
	ST_PLAY,
	ST_OVER,
} state_t;

static gk_game_t game_object;
static gk_game_t *game;
static state_t state;
static float ball_x;
static float ball_y;
static float vx;
static float vy;
static float speed;
static float player_x; // the centre of each paddle
static float ai_x;
static float ai_aim;   // where it means to be when the ball arrives
static float finger_x;
static bool finger_down;
static int pw;
static int py_player;
static int py_ai;
static int lives;
static long score;
static long best;
static bool new_best;
static int goals;
static uint32_t clock_ms;
static uint32_t over_at;

// Where things were when last drawn, to be wiped.
static int old_bx;
static int old_by;
static int old_player;
static int old_ai;

static uint16_t col_bg;
static uint16_t col_line;
static uint16_t col_ball;
static uint16_t col_player;
static uint16_t col_ai;
static uint16_t col_life;

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static void erase(int x, int y, int w, int h) {
	gamecanvas_t *c = &game->canvas;
	gamecanvas_rect(c, x, y, w, h, col_bg);
	// the dashes of the centre line
	int mid = game->h / 2;
	if (y <= mid + 2 && y + h >= mid - 2) {
		for (int dx = 0; dx < game->w; dx += 28) {
			gamecanvas_rect(c, dx + 6, mid - 2, 14, 4, col_line);
		}
	}
	// the lives, bottom left
	gamecanvas_dirty(c, x, y, w, h);
}

static void draw_lives(void) {
	gamecanvas_t *c = &game->canvas;
	int y = game->h - 16;
	gamecanvas_rect(c, 0, y - 12, 140, 24, col_bg);
	for (int i = 0; i < lives; i++) {
		gamecanvas_circle(c, 18 + i * 26, y, 8, col_life);
	}
	gamecanvas_dirty(c, 0, y - 12, 140, 24);
}

static void draw_objects(void) {
	gamecanvas_t *c = &game->canvas;
	gamecanvas_round_rect(c, (int)(player_x - pw / 2), py_player, pw, PADDLE_H, PADDLE_H / 2, col_player);
	gamecanvas_round_rect(c, (int)(ai_x - pw / 2), py_ai, pw, PADDLE_H, PADDLE_H / 2, col_ai);
	gamecanvas_circle(c, (int)ball_x, (int)ball_y, BALL_R, col_ball);
}

// Wipes where the objects were, draws them where they are.
static void redraw(void) {
	erase(old_bx - BALL_R - 1, old_by - BALL_R - 1, BALL_R * 2 + 2, BALL_R * 2 + 2);
	erase(old_player - pw / 2 - 1, py_player - 1, pw + 2, PADDLE_H + 2);
	erase(old_ai - pw / 2 - 1, py_ai - 1, pw + 2, PADDLE_H + 2);
	draw_objects();
	old_bx = (int)ball_x;
	old_by = (int)ball_y;
	old_player = (int)player_x;
	old_ai = (int)ai_x;
	gamecanvas_t *c = &game->canvas;
	gamecanvas_dirty(c, (int)ball_x - BALL_R - 1, (int)ball_y - BALL_R - 1, BALL_R * 2 + 2, BALL_R * 2 + 2);
	gamecanvas_dirty(c, (int)(player_x - pw / 2) - 1, py_player - 1, pw + 2, PADDLE_H + 2);
	gamecanvas_dirty(c, (int)(ai_x - pw / 2) - 1, py_ai - 1, pw + 2, PADDLE_H + 2);
}

static void redraw_all(void) {
	gamecanvas_fill(&game->canvas, col_bg);
	int mid = game->h / 2;
	for (int dx = 0; dx < game->w; dx += 28) {
		gamecanvas_rect(&game->canvas, dx + 6, mid - 2, 14, 4, col_line);
	}
	draw_lives();
	draw_objects();
	old_bx = (int)ball_x;
	old_by = (int)ball_y;
	old_player = (int)player_x;
	old_ai = (int)ai_x;
	gk_dirty_all(game);
}

static void show_hud(void) { gk_ui_score_best(&game->ui, score, best > score ? best : score); }

static void serve_ready(void) {
	state = ST_READY;
	gk_timer(game, false);
	ball_x = game->w / 2.0f;
	ball_y = game->h / 2.0f + 40;
	vx = vy = 0;
	ai_aim = ai_x = game->w / 2.0f;
	redraw_all();
	gk_ui_message_ready(&game->ui, "game_tap_serve", NULL, lives == LIVES && score == 0 ? best : 0, -60);
}

static void new_game(void) {
	lives = LIVES;
	score = 0;
	goals = 0;
	new_best = false;
	speed = SPEED_START;
	best = highscore_get("rally");
	player_x = game->w / 2.0f;
	show_hud();
	serve_ready();
}

static void launch(void) {
	state = ST_PLAY;
	gk_ui_message_hide(&game->ui);
	float angle = (frand() - 0.5f) * 0.9f;
	vx = sinf(angle) * speed;
	vy = -cosf(angle) * speed; // away from the player, towards the other paddle
	gk_timer(game, true);
}

static void game_over(void) {
	state = ST_OVER;
	over_at = clock_ms; // the timer runs on, to time the tap that starts again
	new_best = highscore_submit("rally", score, false);
	if (new_best) {
		best = score;
	}
	gamesound_play(SND_LOSE);
	show_hud();
	gk_ui_message_over(&game->ui, score, new_best, -60);
}

static void lose_life(void) {
	lives--;
	gamesound_play(SND_ERROR);
	draw_lives();
	if (lives <= 0) {
		game_over();
		return;
	}
	speed = SPEED_START + SPEED_STEP * (float)(goals > 10 ? 10 : goals);
	serve_ready();
}

static void score_goal(void) {
	goals++;
	score += 5;
	gamesound_play(SND_EAT);
	show_hud();
	speed = SPEED_START + SPEED_STEP * (float)(goals > 10 ? 10 : goals);
	serve_ready();
}

static void bounce_off_paddle(float centre, bool from_player) {
	float offset = (ball_x - centre) / (pw / 2.0f);
	if (offset > 1) {
		offset = 1;
	} else if (offset < -1) {
		offset = -1;
	}
	if (speed < SPEED_MAX) {
		speed += SPEED_STEP;
	}
	float angle = offset * 1.0f; // radians off the straight
	vx = sinf(angle) * speed;
	vy = cosf(angle) * speed * (from_player ? -1.0f : 1.0f);
}

static void tick(gk_game_t *g, uint32_t dt_ms) {
	(void)g;
	clock_ms += dt_ms;
	if (state != ST_PLAY) {
		return;
	}
	float dt = (float)dt_ms / 1000.0f;

	// The opponent: it aims for where the ball will be, with an error that the
	// first goals leave large and later ones do not, and moves no faster than its
	// speed allows.
	float ai_speed = 250.0f + 14.0f * (float)goals;
	if (ai_speed > 560) {
		ai_speed = 560;
	}
	float target = vy < 0 ? ai_aim : game->w / 2.0f;
	if (ai_x < target) {
		ai_x += ai_speed * dt;
		if (ai_x > target) {
			ai_x = target;
		}
	} else {
		ai_x -= ai_speed * dt;
		if (ai_x < target) {
			ai_x = target;
		}
	}
	if (ai_x < pw / 2.0f) {
		ai_x = pw / 2.0f;
	}
	if (ai_x > game->w - pw / 2.0f) {
		ai_x = game->w - pw / 2.0f;
	}

	// The player's: it goes where the finger is, at a speed no finger will outrun.
	if (finger_down) {
		float d = finger_x - player_x;
		float max = 1800.0f * dt;
		player_x += d > max ? max : d < -max ? -max : d;
	}
	if (player_x < pw / 2.0f) {
		player_x = pw / 2.0f;
	}
	if (player_x > game->w - pw / 2.0f) {
		player_x = game->w - pw / 2.0f;
	}

	// The ball, in steps short enough not to jump a paddle.
	float dist = speed * dt;
	int steps = (int)(dist / 5.0f) + 1;
	float sdt = dt / (float)steps;
	for (int i = 0; i < steps; i++) {
		ball_x += vx * sdt;
		ball_y += vy * sdt;
		if (ball_x < BALL_R) {
			ball_x = BALL_R;
			vx = -vx;
			gamesound_play(SND_TICK);
		} else if (ball_x > game->w - BALL_R) {
			ball_x = (float)(game->w - BALL_R);
			vx = -vx;
			gamesound_play(SND_TICK);
		}
		// The player's paddle.
		if (vy > 0 && ball_y + BALL_R >= py_player && ball_y - BALL_R <= py_player + PADDLE_H &&
			fabsf(ball_x - player_x) <= pw / 2.0f + BALL_R * 0.6f) {
			ball_y = (float)(py_player - BALL_R);
			bounce_off_paddle(player_x, true);
			score++;
			show_hud();
			gamesound_play(SND_HIT);
			// the opponent works out its answer, wrongly by a margin that shrinks
			float err = (60.0f - 3.0f * (float)goals);
			if (err < 8) {
				err = 8;
			}
			ai_aim = ball_x + (vx / vy) * (float)(py_ai - py_player) + (frand() - 0.5f) * 2.0f * err * 2.0f;
			// folded back from the walls
			while (ai_aim < 0 || ai_aim > game->w) {
				ai_aim = ai_aim < 0 ? -ai_aim : 2.0f * game->w - ai_aim;
			}
		}
		// The opponent's.
		if (vy < 0 && ball_y - BALL_R <= py_ai + PADDLE_H && ball_y + BALL_R >= py_ai &&
			fabsf(ball_x - ai_x) <= pw / 2.0f + BALL_R * 0.6f) {
			ball_y = (float)(py_ai + PADDLE_H + BALL_R);
			bounce_off_paddle(ai_x, false);
			gamesound_play(SND_BLIP);
		}
		if (ball_y < -BALL_R * 2) {
			redraw();
			score_goal();
			return;
		}
		if (ball_y > game->h + BALL_R * 2) {
			redraw();
			lose_life();
			return;
		}
	}
	redraw();
}

static void touch(gk_game_t *g, gk_phase_t phase, int x, int y) {
	(void)g;
	(void)y;
	if (phase == GK_UP) {
		finger_down = false;
		if (state == ST_READY) {
			launch();
		} else if (state == ST_OVER && clock_ms - over_at > DEAF_MS) {
			new_game();
		}
		return;
	}
	finger_down = true;
	finger_x = (float)x;
	if (state == ST_READY) {
		// the paddle can be placed before serving; the ball waits
		player_x = finger_x;
		if (player_x < pw / 2.0f) {
			player_x = pw / 2.0f;
		}
		if (player_x > game->w - pw / 2.0f) {
			player_x = game->w - pw / 2.0f;
		}
		redraw();
	}
}

static void open_game(gk_game_t *g) {
	game = g;
	srand((unsigned)time(NULL));
	col_bg = gc_rgb(16, 22, 30);
	col_line = gc_rgb(40, 54, 70);
	col_ball = gc_rgb(255, 255, 255);
	col_player = gc_rgb(96, 180, 255);
	col_ai = gc_rgb(255, 120, 110);
	col_life = gc_rgb(255, 95, 120);
	pw = game->w / 5;
	py_player = game->h - 52;
	py_ai = 30;
	finger_down = false;
	clock_ms = 0;
	new_game();
}

static void restart(gk_game_t *g) {
	(void)g;
	new_game();
}

const gk_def_t rally_def = {
	.id = "rally",
	.tick_ms = 16,
	.open = open_game,
	.touch = touch,
	.tick = tick,
	.restart = restart,
};

void rally_init(gui_config_t *cfg) { gk_game_init(&game_object, rally_screen, cfg, &rally_def); }
