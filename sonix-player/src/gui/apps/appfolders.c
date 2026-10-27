#include "appfolders.h"

#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include "src/gui/apps/applauncherui.h"
#include "src/gui/apps/rompage.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/system/apps/apps.h"

#define BUILTIN_MAX 16
#define TILES_MAX (BUILTIN_MAX + APPS_MAX)

struct appfolder {
	gui_config_t *cfg;
	lv_obj_t *grid;
	app_folder_t folder;
	const grid_entry_t *builtin;
	int builtin_count;

	// What the tiles of the apps draw: a copy of each icon, owned by this page,
	// because another page scanning the card replaces the scan's own.
	lv_image_dsc_t dsc[APPS_MAX];
	uint8_t *pixels[APPS_MAX];
};

static void tile_cb(void *arg) {
	const app_t *app = arg;
	if (app->rom_dir[0]) {
		rompage_open(app);
	} else {
		applauncherui_start(app, NULL);
	}
}

static void free_icons(appfolder_t *f) {
	for (int i = 0; i < APPS_MAX; i++) {
		if (f->pixels[i]) {
			lv_image_cache_drop(&f->dsc[i]); // keyed by the descriptor's address
			free(f->pixels[i]);
			f->pixels[i] = NULL;
		}
	}
}

static void refresh(appfolder_t *f) {
	const char *sd = f->cfg->sd_root_path;
	apps_prepare(sd);
	apps_scan(sd);

	// The old tiles go first: they point at the icons about to be freed.
	lv_obj_clean(f->grid);
	free_icons(f);

	const app_t *found[APPS_MAX];
	int found_count = apps_in_folder(f->folder, found, APPS_MAX);

	grid_entry_t entries[TILES_MAX];
	int n = 0;
	for (int i = 0; i < f->builtin_count && n < BUILTIN_MAX; i++) {
		entries[n++] = f->builtin[i];
	}
	for (int i = 0; i < found_count && n < TILES_MAX; i++) {
		const app_t *app = found[i];
		const lv_image_dsc_t *icon = &icon_menu_app;
		if (app->icon) {
			size_t bytes = (size_t)app->icon_w * app->icon_h * 4;
			f->pixels[i] = malloc(bytes);
			if (f->pixels[i]) {
				memcpy(f->pixels[i], app->icon, bytes);
				lv_image_dsc_t *d = &f->dsc[i];
				memset(d, 0, sizeof(*d));
				d->header.magic = LV_IMAGE_HEADER_MAGIC;
				d->header.cf = LV_COLOR_FORMAT_ARGB8888;
				d->header.w = (uint32_t)app->icon_w;
				d->header.h = (uint32_t)app->icon_h;
				d->header.stride = (uint32_t)app->icon_w * 4;
				d->data_size = (uint32_t)bytes;
				d->data = f->pixels[i];
				icon = d;
			}
		}
		grid_entry_t e;
		memset(&e, 0, sizeof(e));
		e.label = app->name;
		e.icon = icon;
		e.action_arg = tile_cb;
		e.arg = (void *)app;
		entries[n++] = e;
	}

	gridpage_set_entries(f->grid, f->cfg, entries, n, 2, 3, true);
	if (n > 6) {
		gridpage_set_scrolling(f->grid);
	} else {
		lv_obj_set_scrollable(f->grid, false);
	}
	lv_obj_scroll_to_y(f->grid, 0, LV_ANIM_OFF);
}

static void loaded_cb(lv_event_t *e) { refresh(lv_event_get_user_data(e)); }

appfolder_t *appfolder_create(lv_obj_t *screen, gui_config_t *cfg, const char *title, app_folder_t folder,
							  const grid_entry_t *builtin, int count) {
	appfolder_t *f = calloc(1, sizeof(*f));
	if (!f) {
		return NULL;
	}
	f->cfg = cfg;
	f->folder = folder;
	f->builtin = builtin;
	f->builtin_count = count;

	// Two columns by three rows, like the Music and Wireless pages, so a tile
	// here is the size of a tile there. Laid out as full even with fewer entries,
	// so a lone tile sits top left instead of floating in the middle.
	f->grid = gridpage_build(screen, cfg, builtin, count, 2, 3, true);
	if (count > 6) {
		gridpage_set_scrolling(f->grid);
	}
	settingsrow_title(screen, cfg, title);
	lv_obj_add_event_cb(screen, loaded_cb, LV_EVENT_SCREEN_LOADED, f);
	return f;
}
