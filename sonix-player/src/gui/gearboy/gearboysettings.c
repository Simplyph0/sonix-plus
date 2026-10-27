#include "gearboysettings.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/gearboy/gearboy.h"
#include "src/system/core/lang.h"

#include <stdint.h>
#include <stdio.h>

lv_obj_t *gearboysettings_screen;

// The two pill rows: 5 palettes and 4 shaders. Same shape as the MSEB radius
// and keyboard-type pills, but wrapped over several lines: five names do not
// fit across 480 pixels.
static lv_obj_t *palette_pills[6];
static lv_obj_t *shader_pills[4];
static lv_obj_t *correction_switch;
static lv_obj_t *fps_switch;
static lv_obj_t *bootrom_switch;

static const char *const PALETTE_NAMES[6] = {
	"gearboy_palette_original", "gearboy_palette_sharp", "gearboy_palette_bw",
	"gearboy_palette_autumn",	"gearboy_palette_soft",	 "gearboy_palette_custom",
};
static const char *const SHADER_NAMES[4] = {
	"Pixel perfect", "GBC", "GBC Dot matrix", "GB Dot matrix",
};

// Repaints the pills: accent on the selected one, quiet on the rest.
static void refresh_pills(void) {
	int pal = (int)config_get_int("gearboy", "palette", 0);
	int sh = (int)config_get_int("gearboy", "shader", 0);
	for (int i = 0; i < 6; i++) {
		if (!palette_pills[i]) {
			continue;
		}
		bool on = i == pal;
		lv_obj_set_style_bg_color(palette_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(palette_pills[i], 0),
									on ? lv_color_white() : theme()->text_primary, 0);
	}
	for (int i = 0; i < 4; i++) {
		if (!shader_pills[i]) {
			continue;
		}
		bool on = i == sh;
		lv_obj_set_style_bg_color(shader_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(shader_pills[i], 0),
									on ? lv_color_white() : theme()->text_primary, 0);
	}
}

// ---------------------------------------------------------------------------
// The custom palette: four shades, each three sliders of red, green and blue.
// ---------------------------------------------------------------------------

#define CHANNEL_STEPS 17 // 0, 16, 32 ... 240, 255

static int custom_sel;
static lv_obj_t *custom_chip[4];
static lv_obj_t *channel_slider[3];
static lv_obj_t *channel_value[3];
static const char *const SHADE_NAMES[4] = {"gearboy_custom_lightest", "gearboy_custom_light", "gearboy_custom_dark",
										   "gearboy_custom_darkest"};

static int channel_to_step(int v) {
	int s = (v + 8) / 16;
	return s > CHANNEL_STEPS - 1 ? CHANNEL_STEPS - 1 : s;
}
static int step_to_channel(int s) { return s >= CHANNEL_STEPS - 1 ? 255 : s * 16; }

static void refresh_custom(bool sliders_too) {
	for (int i = 0; i < 4; i++) {
		if (!custom_chip[i]) {
			continue;
		}
		uint32_t c = gearboy_custom_colour(i);
		lv_obj_set_style_bg_color(custom_chip[i], lv_color_hex(c), 0);
		// text that reads on the shade it sits on
		unsigned lum = (((c >> 16) & 255) * 30 + ((c >> 8) & 255) * 59 + (c & 255) * 11) / 100;
		lv_obj_set_style_text_color(lv_obj_get_child(custom_chip[i], 0), lum > 140 ? lv_color_black() : lv_color_white(), 0);
		lv_obj_set_style_border_width(custom_chip[i], i == custom_sel ? 5 : 0, 0);
		lv_obj_set_style_border_color(custom_chip[i], theme()->accent, 0);
	}
	uint32_t c = gearboy_custom_colour(custom_sel);
	int ch[3] = {(int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255)};
	for (int k = 0; k < 3; k++) {
		if (sliders_too && channel_slider[k]) {
			lv_slider_set_value(channel_slider[k], channel_to_step(ch[k]), LV_ANIM_OFF);
		}
		if (channel_value[k]) {
			char text[8];
			snprintf(text, sizeof(text), "%d", step_to_channel(channel_to_step(ch[k])));
			lv_label_set_text(channel_value[k], text);
		}
	}
}

static void custom_pick_cb(lv_event_t *e) {
	custom_sel = (int)(intptr_t)lv_event_get_user_data(e);
	refresh_custom(true);
}

static void channel_cb(lv_event_t *e) {
	(void)e;
	int r = step_to_channel((int)lv_slider_get_value(channel_slider[0]));
	int g = step_to_channel((int)lv_slider_get_value(channel_slider[1]));
	int b = step_to_channel((int)lv_slider_get_value(channel_slider[2]));
	gearboy_set_custom_colour(custom_sel, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
	refresh_pills(); // the palette is now Custom
	refresh_custom(false);
}

static void fps_toggle_cb(lv_event_t *e) {
	(void)e;
	config_set_int("gearboy", "show_fps", lv_obj_has_state(fps_switch, LV_STATE_CHECKED) ? 1 : 0);
	config_save();
}

static void palette_pick_cb(lv_event_t *e) {
	config_set_int("gearboy", "palette", (int)(intptr_t)lv_event_get_user_data(e));
	config_save();
	gearboy_refresh_video_settings();
	refresh_pills();
}

static void shader_pick_cb(lv_event_t *e) {
	config_set_int("gearboy", "shader", (int)(intptr_t)lv_event_get_user_data(e));
	config_save();
	gearboy_refresh_video_settings();
	refresh_pills();
}

static void correction_toggle_cb(lv_event_t *e) {
	(void)e;
	config_set_int("gearboy", "gbc_correction", lv_obj_has_state(correction_switch, LV_STATE_CHECKED) ? 1 : 0);
	config_save();
	gearboy_refresh_video_settings();
}

static void bootrom_toggle_cb(lv_event_t *e) {
	(void)e;
	config_set_int("gearboy", "bootrom", lv_obj_has_state(bootrom_switch, LV_STATE_CHECKED) ? 1 : 0);
	config_save();
	// No refresh: the boot ROMs are only consulted when a game starts.
}

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int value) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 60);
	lv_obj_set_style_pad_hor(btn, 18, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // the Adwaita pill
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)value);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);
	return btn;
}

// A card with a title and a wrapping row of pills inside.
static lv_obj_t *make_pill_card(lv_obj_t *parent, const char *title_tag) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 16, 0);
	lv_obj_set_style_pad_row(card, 12, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text(title, tr(title_tag));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_24, 0);

	lv_obj_t *pills = lv_obj_create(card);
	lv_obj_set_width(pills, lv_pct(100));
	lv_obj_set_height(pills, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_column(pills, 8, 0);
	lv_obj_set_style_pad_row(pills, 8, 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return pills;
}

void gearboysettings_init(gui_config_t *cfg) {
	gearboysettings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(gearboysettings_screen, cfg, "gearboy_settings");

	// Palette: applies to original Game Boy games only, since GBC titles carry
	// their own colours.
	lv_obj_t *pal_pills = make_pill_card(container, "gearboy_palette");
	for (int i = 0; i < 6; i++) {
		palette_pills[i] = make_pill(pal_pills, PALETTE_NAMES[i], palette_pick_cb, i);
	}

	// Custom colours: pick a shade, then set its red, green and blue. Changing any of them
	// makes the palette the custom one.
	lv_obj_t *custom_pills = make_pill_card(container, "gearboy_custom_title");
	for (int i = 0; i < 4; i++) {
		custom_chip[i] = make_pill(custom_pills, SHADE_NAMES[i], custom_pick_cb, i);
	}
	static const char *const CHANNEL_NAMES[3] = {"gearboy_custom_red", "gearboy_custom_green", "gearboy_custom_blue"};
	for (int k = 0; k < 3; k++) {
		settingsrow_slider(container, CHANNEL_NAMES[k], CHANNEL_STEPS, &channel_value[k], &channel_slider[k], channel_cb);
	}
	refresh_custom(true);

	// Shaders: the real panels' grid, drawn inside the 3x upscale.
	lv_obj_t *sh_pills = make_pill_card(container, "gearboy_shaders");
	for (int i = 0; i < 4; i++) {
		shader_pills[i] = make_pill(sh_pills, SHADER_NAMES[i], shader_pick_cb, i);
	}

	// The frame counter over the picture, for finding out whether a game is keeping up.
	settingsrow_toggle(container, "gearboy_show_fps", &fps_switch, fps_toggle_cb);
	if (config_get_int("gearboy", "show_fps", 0) != 0) {
		lv_obj_add_state(fps_switch, LV_STATE_CHECKED);
	}

	settingsrow_toggle(container, "gearboy_gbc_color_correction", &correction_switch, correction_toggle_cb);
	if (config_get_int("gearboy", "gbc_correction", 1) != 0) {
		lv_obj_add_state(correction_switch, LV_STATE_CHECKED);
	}

	// Boot ROMs: the logo scrolling down at start-up, as on the real machine.
	// The toggle carries its instructions underneath, because without the two
	// files it does nothing and the user has to be told where to put them.
	settingsrow_toggle(container, "gearboy_show_boot_rom", &bootrom_switch, bootrom_toggle_cb);
	if (config_get_int("gearboy", "bootrom", 0) != 0) {
		lv_obj_add_state(bootrom_switch, LV_STATE_CHECKED);
	}
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("gearboy_boot_rom_note"));

	refresh_pills();
	theme_register_refresh(refresh_pills);
	switcher_attach_back_gesture(gearboysettings_screen);
}
