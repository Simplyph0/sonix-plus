#include "gamekit.h"

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

#define PAUSE_BUTTON_SIZE 56

// ---------------------------------------------------------------------------
// The pause menu
// ---------------------------------------------------------------------------

static void update_sound_label(gk_ui_t *ui) {
	if (ui->sound_label) {
		lv_label_set_text(ui->sound_label, tr(gamesound_enabled() ? "game_sound_on" : "game_sound_off"));
	}
}

static void set_paused(gk_ui_t *ui, bool paused) {
	if (ui->paused == paused) {
		return;
	}
	ui->paused = paused;
	if (paused) {
		update_sound_label(ui);
	}
	lv_obj_set_hidden(ui->menu, !paused);
	gamesound_play(SND_PAUSE);
	if (ui->on_pause) {
		ui->on_pause(paused, ui->user);
	}
}

static void pause_button_cb(lv_event_t *e) {
	gk_ui_t *ui = lv_event_get_user_data(e);
	if (switcher_back_drag_active()) {
		return;
	}
	set_paused(ui, true);
}

static void resume_cb(lv_event_t *e) { set_paused(lv_event_get_user_data(e), false); }

static void restart_cb(lv_event_t *e) {
	gk_ui_t *ui = lv_event_get_user_data(e);
	// The game starts afresh; its pause hook is told the pause is over first, so a
	// timer it stopped is not left stopped.
	set_paused(ui, false);
	if (ui->on_restart) {
		ui->on_restart(ui->user);
	}
}

static void sound_cb(lv_event_t *e) {
	gk_ui_t *ui = lv_event_get_user_data(e);
	gamesound_set_enabled(!gamesound_enabled());
	update_sound_label(ui);
	gamesound_play(SND_BLIP);
}

static lv_obj_t *menu_button(lv_obj_t *card, const char *text, bool primary, lv_event_cb_t cb, gk_ui_t *ui,
							 lv_obj_t **label_out) {
	lv_obj_t *button = lv_btn_create(card);
	lv_obj_set_size(button, lv_pct(100), 72);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_bg_color(button, primary ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, ui);

	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, &font_ui_28, 0);
	lv_obj_set_style_text_color(label, primary ? lv_color_white() : theme()->text_primary, 0);
	lv_obj_center(label);
	if (label_out) {
		*label_out = label;
	}
	return button;
}

static void build_menu(gk_ui_t *ui) {
	// A veil over the whole page that takes the touches: nothing behind it can be
	// played while the menu is up.
	lv_obj_t *veil = lv_obj_create(ui->screen);
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_70, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_set_scrollable(veil, false);
	lv_obj_set_clickable(veil, true);
	lv_obj_set_hidden(veil, true);

	lv_obj_t *card = lv_obj_create(veil);
	lv_obj_set_width(card, 340);
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 28, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 22, 0);
	lv_obj_set_style_pad_row(card, 14, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text(title, tr("game_paused"));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_32, 0);
	lv_obj_set_style_pad_bottom(title, 4, 0);

	menu_button(card, tr("game_resume"), true, resume_cb, ui, NULL);
	menu_button(card, tr("game_new"), false, restart_cb, ui, NULL);
	menu_button(card, tr(gamesound_enabled() ? "game_sound_on" : "game_sound_off"), false, sound_cb, ui,
				&ui->sound_label);
	ui->menu = veil;
}

// ---------------------------------------------------------------------------
// The page's pieces
// ---------------------------------------------------------------------------

void gk_ui_build(gk_ui_t *ui, lv_obj_t *screen, gui_config_t *cfg, const char *title_id,
				 void (*on_pause)(bool paused, void *user), void (*on_restart)(void *user), void *user) {
	memset(ui, 0, sizeof(*ui));
	ui->screen = screen;
	ui->cfg = cfg;
	ui->on_pause = on_pause;
	ui->on_restart = on_restart;
	ui->user = user;

	lv_obj_add_style(screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(screen, false);
	lv_obj_t *title = settingsrow_title(screen, cfg, title_id);
	// The pause button takes the one corner slot, and the score the room beside it.
	settingsrow_title_corner_slots(title, cfg, 3);

	// The score: two small lines to the left of the pause button.
	ui->score_label = lv_label_create(screen);
	lv_obj_add_style(ui->score_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(ui->score_label, &font_ui_20_bold, 0);
	lv_obj_set_style_text_align(ui->score_label, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_width(ui->score_label, 200);
	lv_obj_align(ui->score_label, LV_ALIGN_TOP_RIGHT, -(int)cfg->padding - PAUSE_BUTTON_SIZE - 8,
				 (int)cfg->top_bar_height + (int)cfg->padding + 2);
	lv_label_set_text(ui->score_label, "");

	ui->message = lv_label_create(screen);
	lv_obj_set_style_text_font(ui->message, &font_ui_32, 0);
	lv_obj_set_style_text_color(ui->message, lv_color_white(), 0);
	lv_obj_set_style_text_align(ui->message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_bg_color(ui->message, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(ui->message, LV_OPA_60, 0);
	lv_obj_set_style_pad_all(ui->message, 18, 0);
	lv_obj_set_style_radius(ui->message, 16, 0);
	lv_obj_set_clickable(ui->message, false);
	lv_obj_set_hidden(ui->message, true);

	gk_ui_attach_pause(ui, screen, cfg, on_pause, on_restart, user);
}

void gk_ui_attach_pause(gk_ui_t *ui, lv_obj_t *screen, gui_config_t *cfg, void (*on_pause)(bool paused, void *user),
						void (*on_restart)(void *user), void *user) {
	ui->screen = screen;
	ui->cfg = cfg;
	ui->on_pause = on_pause;
	ui->on_restart = on_restart;
	ui->user = user;

	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, PAUSE_BUTTON_SIZE, PAUSE_BUTTON_SIZE);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -(int)cfg->padding, (int)cfg->padding + (int)cfg->top_bar_height);
	lv_obj_add_event_cb(button, pause_button_cb, LV_EVENT_CLICKED, ui);
	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, &icon_pause);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	ui->pause_button = button;

	build_menu(ui); // last: it lies over everything else
}

void gk_ui_score(gk_ui_t *ui, const char *line1, const char *line2) {
	char text[96];
	snprintf(text, sizeof(text), "%s%s%s", line1 ? line1 : "", line1 && line2 ? "\n" : "", line2 ? line2 : "");
	lv_label_set_text(ui->score_label, text);
}

void gk_ui_score_best(gk_ui_t *ui, long score, long best) {
	char a[40];
	char b[40];
	snprintf(a, sizeof(a), "%s %ld", tr("game_score"), score);
	snprintf(b, sizeof(b), "%s %ld", tr("game_best"), best);
	gk_ui_score(ui, a, b);
}

void gk_ui_message(gk_ui_t *ui, const char *text, int y_offset) {
	lv_label_set_text(ui->message, text);
	lv_obj_set_hidden(ui->message, false);
	lv_obj_align(ui->message, LV_ALIGN_CENTER, 0, y_offset);
}

void gk_ui_message_hide(gk_ui_t *ui) { lv_obj_set_hidden(ui->message, true); }

void gk_ui_message_ready(gk_ui_t *ui, const char *prompt_key, const char *hint_key, long best, int y_offset) {
	char text[200];
	int n = snprintf(text, sizeof(text), "%s", tr(prompt_key));
	if (hint_key && n < (int)sizeof(text)) {
		n += snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", tr(hint_key));
	}
	if (best > 0 && n < (int)sizeof(text)) {
		snprintf(text + n, sizeof(text) - (size_t)n, "\n%s %ld", tr("game_best"), best);
	}
	gk_ui_message(ui, text, y_offset);
}

void gk_ui_message_over(gk_ui_t *ui, long score, bool new_best, int y_offset) {
	char text[200];
	int n = snprintf(text, sizeof(text), "%s\n%s %ld", tr("game_over"), tr("game_score"), score);
	if (new_best && n < (int)sizeof(text)) {
		n += snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", tr("game_new_best"));
	}
	if (n < (int)sizeof(text)) {
		snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", tr("game_tap_again"));
	}
	gk_ui_message(ui, text, y_offset);
}

void gk_ui_reset(gk_ui_t *ui) {
	ui->paused = false;
	if (ui->menu) {
		lv_obj_set_hidden(ui->menu, true);
	}
	if (ui->message) {
		lv_obj_set_hidden(ui->message, true);
	}
}

bool gk_ui_paused(const gk_ui_t *ui) { return ui->paused; }

// ---------------------------------------------------------------------------
// The runner
// ---------------------------------------------------------------------------

static void timer_cb(lv_timer_t *timer) {
	gk_game_t *g = lv_timer_get_user_data(timer);
	if (!g->open || g->ui.paused || !g->def->tick) {
		return;
	}
	uint32_t now = lv_tick_get();
	uint32_t dt = now - g->last_tick;
	g->last_tick = now;
	if (dt > 250) {
		dt = 250; // a long stall is not a long step
	}
	g->def->tick(g, dt);
}

static void apply_timer(gk_game_t *g) {
	if (!g->timer) {
		return;
	}
	if (g->timer_wanted && !g->ui.paused) {
		g->last_tick = lv_tick_get();
		lv_timer_resume(g->timer);
	} else {
		lv_timer_pause(g->timer);
	}
}

void gk_timer(gk_game_t *g, bool run) {
	g->timer_wanted = run;
	apply_timer(g);
}

static void ui_pause_cb(bool paused, void *user) {
	gk_game_t *g = user;
	apply_timer(g);
	if (g->def->paused) {
		g->def->paused(g, paused);
	}
}

static void ui_restart_cb(void *user) {
	gk_game_t *g = user;
	if (g->def->restart) {
		g->def->restart(g);
	}
}

static void touch_point(gk_game_t *g, int *x, int *y) {
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	*x = p.x - lv_obj_get_x(g->canvas.image);
	*y = p.y - lv_obj_get_y(g->canvas.image);
}

static void touch_cb(lv_event_t *e) {
	gk_game_t *g = lv_event_get_user_data(e);
	if (!g->open || g->ui.paused || !g->def->touch) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);
	int x;
	int y;
	touch_point(g, &x, &y);
	g->def->touch(g, code == LV_EVENT_PRESSED ? GK_DOWN : code == LV_EVENT_RELEASED ? GK_UP : GK_MOVE, x, y);
}

static void loaded_cb(lv_event_t *e) {
	gk_game_t *g = lv_event_get_user_data(e);
	int top = settingsrow_content_top(g->cfg);
	g->w = g->cfg->screen_width;
	g->h = (int)g->cfg->screen_height - top - 6;
	if (!gamecanvas_create(&g->canvas, g->screen, 0, top, g->w, g->h, 0)) {
		gui_notify_popup("game_no_memory");
		return;
	}
	lv_obj_move_background(g->canvas.image);

	g->open = true;
	g->timer_wanted = false;
	if (g->def->tick_ms > 0) {
		g->timer = lv_timer_create(timer_cb, (uint32_t)g->def->tick_ms, g);
		lv_timer_pause(g->timer);
	}
	gamesound_open();
	g->def->open(g);
	power_hold_screen_on(true); // a game is played with the screen on and nobody touching it
}

static void unloaded_cb(lv_event_t *e) {
	gk_game_t *g = lv_event_get_user_data(e);
	if (!g->open) {
		return;
	}
	g->open = false;
	if (g->timer) {
		lv_timer_delete(g->timer);
		g->timer = NULL;
	}
	if (g->def->close) {
		g->def->close(g);
	}
	gamesound_close();
	gk_ui_reset(&g->ui);
	gamecanvas_destroy(&g->canvas);
	power_hold_screen_on(false);
}

void gk_game_init(gk_game_t *g, lv_obj_t *screen, gui_config_t *cfg, const gk_def_t *def) {
	memset(g, 0, sizeof(*g));
	g->def = def;
	g->cfg = cfg;
	g->screen = screen;
	gk_ui_build(&g->ui, screen, cfg, def->id, ui_pause_cb, ui_restart_cb, g);

	lv_obj_add_event_cb(screen, touch_cb, LV_EVENT_PRESSED, g);
	lv_obj_add_event_cb(screen, touch_cb, LV_EVENT_PRESSING, g);
	lv_obj_add_event_cb(screen, touch_cb, LV_EVENT_RELEASED, g);
	lv_obj_add_event_cb(screen, loaded_cb, LV_EVENT_SCREEN_LOADED, g);
	lv_obj_add_event_cb(screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, g);
}
