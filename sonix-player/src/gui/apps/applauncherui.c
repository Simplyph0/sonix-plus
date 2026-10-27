#include "applauncherui.h"

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"
#include "src/system/apps/applaunch.h"

// Long enough for the finger that left the app and whatever the touch queue held,
// short enough not to be noticed.
#define SETTLE_MS 450

static gui_config_t *g_cfg;
static lv_obj_t *blocker;
static lv_timer_t *blocker_timer;

static void blocker_done_cb(lv_timer_t *timer) {
	(void)timer;
	if (blocker) {
		lv_obj_delete(blocker);
		blocker = NULL;
	}
	blocker_timer = NULL; // a repeat count of one deletes the timer after this call
}

static void app_exited_cb(void *user) {
	(void)user;
	if (blocker) {
		return;
	}
	blocker = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(blocker);
	lv_obj_set_size(blocker, (int32_t)g_cfg->screen_width, (int32_t)g_cfg->screen_height);
	lv_obj_set_clickable(blocker, true);
	blocker_timer = lv_timer_create(blocker_done_cb, SETTLE_MS, NULL);
	lv_timer_set_repeat_count(blocker_timer, 1);
}

void applauncherui_init(gui_config_t *cfg) { g_cfg = cfg; }

void applauncherui_start(const app_t *app, const char *rom) {
	if (!apps_launch(app, rom, app_exited_cb, NULL)) {
		gui_notify_popup(apps_running() ? "apps_already_running" : "apps_cannot_start");
	}
}
