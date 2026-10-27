#include "rompage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lvgl/lvgl.h"

#include "src/gui/apps/applauncherui.h"
#include "src/gui/apps/coverlist.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"

lv_obj_t *rompage_screen;

#define ROMS_MAX 500
#define SCAN_DEPTH 3

static gui_config_t *g_cfg;
static lv_obj_t *container;
static app_t current;

typedef struct {
	char path[640];
	char name[128];	   // the file's name, without its extension
	char display[128]; // what the list shows
} rom_t;

static rom_t *roms;
static int rom_count;

static int by_display(const void *a, const void *b) {
	return strcasecmp(((const rom_t *)a)->display, ((const rom_t *)b)->display);
}

static void scan(const char *dir_path, int depth) {
	DIR *dir = opendir(dir_path);
	if (!dir) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL && rom_count < ROMS_MAX) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if (snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name) >= (int)sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (depth < SCAN_DEPTH) {
				scan(path, depth + 1);
			}
		} else if (S_ISREG(st.st_mode) && apps_rom_matches(&current, entry->d_name)) {
			rom_t *r = &roms[rom_count++];
			snprintf(r->path, sizeof(r->path), "%s", path);
			snprintf(r->name, sizeof(r->name), "%s", entry->d_name);
			char *dot = strrchr(r->name, '.');
			if (dot) {
				*dot = '\0'; // the name without its extension
			}
			coverlist_display_name(r->name, r->display, sizeof(r->display));
		}
	}
	closedir(dir);
}

// <card>/<rom_dir>, made with the folders above it when the card is writable.
static void make_rom_dir(const char *full) {
	if (!storage_card_attached() || !storage_sd_writable()) {
		return;
	}
	char partial[640];
	snprintf(partial, sizeof(partial), "%s", full);
	for (char *p = partial + strlen(g_cfg->sd_root_path) + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(partial, 0755);
			*p = '/';
		}
	}
	mkdir(partial, 0755);
}

static void row_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	const char *path = lv_event_get_user_data(e);
	applauncherui_start(&current, path);
}

static void row_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

void rompage_open(const app_t *app) {
	if (!app || !app->rom_dir[0] || !g_cfg->sd_root_path) {
		return;
	}
	current = *app;
	current.icon = NULL; // not ours to use, or to free

	char dir[700];
	snprintf(dir, sizeof(dir), "%s/%s", g_cfg->sd_root_path, app->rom_dir);
	make_rom_dir(dir);

	lv_obj_clean(container); // the old rows go before the covers they were showing
	coverlist_begin();
	free(roms);
	roms = malloc(sizeof(*roms) * ROMS_MAX);
	rom_count = 0;
	if (roms) {
		scan(dir, 0);
		// "Pokemon Gold (USA)" and "pokemon gold" stand together: sorted by what is shown
		qsort(roms, (size_t)rom_count, sizeof(*roms), by_display);
	}

	lv_label_set_text(settingsrow_page_title(rompage_screen), app->name);

	if (rom_count == 0) {
		lv_obj_t *note = lv_label_create(container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(note, &font_ui_22, 0);
		char text[900];
		snprintf(text, sizeof(text), tr("apps_no_files"), app->rom_exts[0] ? app->rom_exts : "*", app->rom_dir);
		lv_label_set_text(note, text);
	}

	// With a cover anywhere the rows all have a place for one; without, they are the plain rows
	// of every other list.
	bool covers = false;
	for (int i = 0; i < rom_count && !covers; i++) {
		covers = coverlist_has_cover(roms[i].path);
	}
	for (int i = 0; i < rom_count; i++) {
		char *path = strdup(roms[i].path);
		if (!path) {
			break;
		}
		lv_obj_t *row;
		if (covers) {
			row = coverlist_row(container, roms[i].display, roms[i].path, row_cb, path);
		} else {
			row = settingsrow_action(container, roms[i].display, row_cb, path);
			settingsrow_name_lines(row, 2);
		}
		lv_obj_add_event_cb(row, row_free_cb, LV_EVENT_DELETE, path);
	}

	switch_screen(rompage_screen);
}

// The pictures go when the page does (they stay while a game runs from it: sixty of them
// are a megabyte and a half).
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_obj_clean(container); // the rows that show them go first
	coverlist_end();
}

void rompage_init(gui_config_t *cfg) {
	g_cfg = cfg;
	container = settingsrow_page(rompage_screen, cfg, "apps");
	lv_obj_add_event_cb(rompage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
