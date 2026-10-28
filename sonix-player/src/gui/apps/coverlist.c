#include "coverlist.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/theme.h"
#include "src/system/apps/apps.h"

#define COVER_SIZE 80
#define COVER_ROW_H 96
#define COVER_TICK_MS 30

typedef struct {
	char path[660]; // the picture
	lv_obj_t *box;	// where it goes
	uint8_t *pixels;
	lv_image_dsc_t dsc;
} cover_t;

static cover_t *covers;
static int cover_count;
static int next_cover;
static lv_timer_t *timer;

void coverlist_display_name(const char *name, char *out, size_t size) {
	size_t n = 0;
	int depth = 0;
	bool space = false;
	for (const char *p = name; *p && n + 1 < size; p++) {
		char c = *p;
		if (c == '(' || c == '[') {
			depth++;
			continue;
		}
		if (c == ')' || c == ']') {
			if (depth > 0) {
				depth--;
			}
			continue;
		}
		if (depth > 0) {
			continue;
		}
		if (c == '_') {
			c = ' ';
		}
		if (c == ' ') {
			space = n > 0;
			continue;
		}
		if (space) {
			out[n++] = ' ';
			space = false;
			if (n + 1 >= size) {
				break;
			}
		}
		out[n++] = c;
	}
	// a dash or comma left hanging where a tag was cut off
	while (n > 0 && (out[n - 1] == '-' || out[n - 1] == ' ' || out[n - 1] == ',')) {
		n--;
	}
	out[n] = '\0';
	if (n == 0) {
		snprintf(out, size, "%s", name);
	}
}

// Name.png, Name.jpg or Name.jpeg beside the game. Writes the path to `out` when there is one.
static bool find_picture(const char *rom_path, char *out, size_t size) {
	static const char *const EXTS[] = {"png", "jpg", "jpeg", "PNG", "JPG"};
	char base[640];
	snprintf(base, sizeof(base), "%s", rom_path);
	char *dot = strrchr(base, '.');
	char *slash = strrchr(base, '/');
	if (!dot || (slash && dot < slash)) {
		return false;
	}
	*dot = '\0';
	for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++) {
		char candidate[660];
		snprintf(candidate, sizeof(candidate), "%s.%s", base, EXTS[i]);
		if (access(candidate, R_OK) == 0) {
			snprintf(out, size, "%s", candidate);
			return true;
		}
	}
	return false;
}

bool coverlist_has_cover(const char *rom_path) {
	char path[660];
	return find_picture(rom_path, path, sizeof(path));
}

static void stop_timer(void) {
	if (timer) {
		lv_timer_delete(timer);
		timer = NULL;
	}
}

void coverlist_end(void) {
	stop_timer();
	for (int i = 0; i < cover_count; i++) {
		if (covers[i].pixels) {
			lv_image_cache_drop(&covers[i].dsc);
			free(covers[i].pixels);
			covers[i].pixels = NULL;
		}
		covers[i].box = NULL;
	}
	cover_count = 0;
	next_cover = 0;
}

void coverlist_begin(void) {
	coverlist_end();
	if (!covers) {
		covers = calloc(COVERLIST_MAX, sizeof(*covers));
	}
}

// The picture as the screen holds it: 16 bits a pixel, half of what the loader returns, kept for as
// long as the list is open. Anything see-through is laid on the colour of the box behind it.
static uint8_t *load_rgb565(const char *path) {
	uint8_t *argb = apps_load_image(path, COVER_SIZE);
	if (!argb) {
		return NULL;
	}
	uint16_t *out = malloc((size_t)COVER_SIZE * COVER_SIZE * sizeof(uint16_t));
	if (out) {
		lv_color_t bg = theme()->surface_pressed;
		for (int i = 0; i < COVER_SIZE * COVER_SIZE; i++) {
			const uint8_t *s = argb + i * 4; // blue, green, red, alpha
			unsigned a = s[3];
			unsigned b = (s[0] * a + bg.blue * (255 - a)) / 255;
			unsigned g = (s[1] * a + bg.green * (255 - a)) / 255;
			unsigned r = (s[2] * a + bg.red * (255 - a)) / 255;
			out[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
		}
	}
	free(argb);
	return (uint8_t *)out;
}

// One picture a tick, in the order the rows were added: the top of the list fills in first.
static void tick_cb(lv_timer_t *t) {
	if (next_cover >= cover_count) {
		lv_timer_pause(t);
		return;
	}
	cover_t *c = &covers[next_cover++];
	if (!c->box) {
		return;
	}
	c->pixels = load_rgb565(c->path);
	if (!c->pixels) {
		return;
	}
	c->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	c->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	c->dsc.header.w = COVER_SIZE;
	c->dsc.header.h = COVER_SIZE;
	c->dsc.header.stride = COVER_SIZE * 2;
	c->dsc.data_size = COVER_SIZE * COVER_SIZE * 2;
	c->dsc.data = c->pixels;

	lv_obj_clean(c->box); // the letter goes
	lv_obj_t *image = lv_image_create(c->box);
	lv_image_set_src(image, &c->dsc);
	lv_obj_center(image);
}

lv_obj_t *coverlist_row(lv_obj_t *parent, const char *label_text, const char *rom_path, lv_event_cb_t cb, void *user) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, COVER_ROW_H);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 16, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 8, 0);
	lv_obj_set_style_pad_ver(row, 8, 0);
	lv_obj_set_style_pad_column(row, 16, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(row, true); // so the swipe back can start on a row
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);

	lv_obj_t *box = lv_obj_create(row);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, COVER_SIZE, COVER_SIZE);
	lv_obj_set_style_radius(box, 12, 0);
	lv_obj_set_style_bg_color(box, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_clickable(box, false);
	lv_obj_set_style_clip_corner(box, true, 0);

	lv_obj_t *letter = lv_label_create(box);
	char initial[2] = {(char)toupper((unsigned char)label_text[0]), '\0'};
	lv_label_set_text(letter, initial);
	lv_obj_add_style(letter, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(letter, &font_ui_32, 0);
	lv_obj_center(letter);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, label_text);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_24) * 2);

	if (covers && cover_count < COVERLIST_MAX && rom_path && find_picture(rom_path, covers[cover_count].path,
																		   sizeof(covers[cover_count].path))) {
		covers[cover_count].box = box;
		covers[cover_count].pixels = NULL;
		cover_count++;
		if (!timer) {
			timer = lv_timer_create(tick_cb, COVER_TICK_MS, NULL);
		} else {
			lv_timer_resume(timer);
		}
	}
	return row;
}
