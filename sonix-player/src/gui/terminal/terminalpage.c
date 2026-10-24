#include "terminalpage.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/respath.h"
#include "src/system/terminal/terminal.h"

lv_obj_t *terminalpage_screen;

#define TERM_KEYBOARD_H 316
#define FIELD_H 62
#define GAP 10
#define INTERRUPT_W 72
#define KEYS_H 40
#define KEYS_GAP 6
#define HISTORY_MAX 32
#define LINE_MAX_CHARS 512
#define POLL_MS 120

// The page uses nearly the whole width: the box and the input line sit close to
// the edges, and the text close to the box's.
#define MARGIN 6
#define BOX_PAD 6

// A terminal looks the same whatever the theme: the screen is dark, and the
// sixteen colours below are chosen to be read on it.
#define SCREEN_COLOR 0x14181e
#define TEXT_COLOR 0xc8ccd4

// The end of the output that is drawn. Laying out much more than this on every
// change costs more than the page is worth on this CPU.
#define SHOWN_MAX 6144
#define MAX_RUNS 400

// With the output scrolled to the end it follows new lines; scrolled up by the
// finger it stays where it is, until the page is scrolled to the end again.
#define FOLLOW_SLACK 12

// The sizes of the text, a step each way from the small buttons in the corner.
// With the monospaced face any size will do; the interface's own typeface, which
// stands in for what that face lacks, is cut at 14 and up, so below that it is
// the 14 that fills in.
static const int FONT_SIZES[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 28};
#define FONT_STEPS ((int)(sizeof(FONT_SIZES) / sizeof(FONT_SIZES[0])))
#define FONT_PX_DEFAULT 16
#define CORNER_BUTTON 48
#define CORNER_GAP 8
#define WIDTH_BUTTON_W 64

// How many characters wide the screen of the terminal is: 0 is as many as the
// box holds, wrapping at its edge; a number is that many, whatever the box, and
// the output scrolls sideways to show what is beyond it.
static const int WIDTHS[] = {0, 80, 100, 120, 160, 200};
#define WIDTH_STEPS ((int)(sizeof(WIDTHS) / sizeof(WIDTHS[0])))

static gui_config_t *g_cfg;
static lv_obj_t *out_box;
static lv_obj_t *out_text; // a span group: a stretch of text for each colour
static lv_obj_t *field;
static lv_obj_t *interrupt_btn;
static lv_obj_t *smaller_btn;
static lv_obj_t *larger_btn;
static lv_obj_t *width_label;
static lv_obj_t *key_row;
static keyboard_t *keyboard;
static lv_timer_t *poll_timer;

// What was typed before, newest last, for the up and down keys. `history_pos`
// is -1 while a new line is being typed, else the entry on show.
static char history[HISTORY_MAX][LINE_MAX_CHARS];
static int history_count;
static int history_pos = -1;
static char draft[LINE_MAX_CHARS];

static uint32_t shown_serial;
static bool follow = true;
static int font_step;
static int width_step;

static char shown[SHOWN_MAX + 1];
static terminal_run_t runs[MAX_RUNS];

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

// The 256 colours: the sixteen named ones, the 6x6x6 cube, the greys.
static lv_color_t palette_color(int index) {
	static const uint32_t named[16] = {
		0x3b4048, 0xe06c75, 0x98c379, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xabb2bf,
		0x5c6370, 0xff7b86, 0xb1e18b, 0xf2cf8a, 0x7cc0ff, 0xd68cf0, 0x6fd0dd, 0xffffff,
	};
	static const int level[6] = {0, 95, 135, 175, 215, 255};
	if (index < 16) {
		return lv_color_hex(named[index]);
	}
	if (index < 232) {
		int i = index - 16;
		return lv_color_make((uint8_t)level[i / 36], (uint8_t)level[(i / 6) % 6], (uint8_t)level[i % 6]);
	}
	int grey = 8 + (index - 232) * 10;
	return lv_color_make((uint8_t)grey, (uint8_t)grey, (uint8_t)grey);
}

// ---------------------------------------------------------------------------
// The typeface
// ---------------------------------------------------------------------------

static const lv_font_t *ui_font(int px) {
	if (px < 14) {
		return &font_ui_14;
	}
	switch (px) {
	case 14:
		return &font_ui_14;
	case 16:
		return &font_ui_16;
	case 18:
		return &font_ui_18;
	case 22:
		return &font_ui_22;
	case 24:
		return &font_ui_24;
	case 28:
		return &font_ui_28;
	default:
		return &font_ui_20;
	}
}

// The monospaced face: DejaVu Sans Mono, which ships with the interface's fonts
// as mono.ttf. A font.ttf in the card's Terminal folder is used in its place, for
// anyone who wants another. A face is cut at a size when that size is first
// wanted, and the interface's own typeface stands in for any glyph it lacks
// (CJK, mostly). With neither file the interface's typeface draws the text, whose
// letters are not all the same width, so columns will not line up.
static char mono_path[600];
static lv_font_t *mono_face[FONT_STEPS];

static void find_mono_font(void) {
	if (mono_path[0]) {
		return;
	}
	char candidate[600];
	const char *tools = terminal_tools_path(g_cfg->sd_root_path);
	if (tools[0]) {
		snprintf(candidate, sizeof(candidate), "%s/font.ttf", tools);
		if (access(candidate, R_OK) == 0) {
			snprintf(mono_path, sizeof(mono_path), "%s", candidate);
			return;
		}
	}
	if (access(SONIX_RESOURCE_DIR "/fonts/mono.ttf", R_OK) == 0) {
		snprintf(mono_path, sizeof(mono_path), "%s", SONIX_RESOURCE_DIR "/fonts/mono.ttf");
	}
}

static const lv_font_t *step_font(int step) {
	if (mono_path[0]) {
		if (!mono_face[step]) {
			mono_face[step] = lv_freetype_font_create(mono_path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
													  (uint32_t)FONT_SIZES[step], LV_FREETYPE_FONT_STYLE_NORMAL);
			if (mono_face[step]) {
				mono_face[step]->fallback = ui_font(FONT_SIZES[step]);
			}
		}
		if (mono_face[step]) {
			return mono_face[step];
		}
	}
	return ui_font(FONT_SIZES[step]);
}

// ---------------------------------------------------------------------------
// Layout and drawing
// ---------------------------------------------------------------------------

static bool wide_mode(void) { return WIDTHS[width_step] > 0; }

// What the text is drawn at, and so how wide a character is.
static int glyph_width(void) {
	int w = lv_font_get_glyph_width(step_font(font_step), '0', 0);
	return w < 1 ? 1 : w;
}

// What programs are told the terminal measures: the width chosen, or as many
// characters across as the box holds at this size; and as many lines.
static void report_size(void) {
	const lv_font_t *font = step_font(font_step);
	int line = lv_font_get_line_height(font);
	if (line < 1) {
		return;
	}
	int columns = WIDTHS[width_step];
	if (columns == 0) {
		columns = (lv_obj_get_width(out_box) - 2 * BOX_PAD) / glyph_width();
	}
	int height = lv_obj_get_height(out_box) - 2 * BOX_PAD;
	terminal_set_size(columns, height / line);
}

// The text's own width follows the terminal's: the box's, wrapping at its edge,
// or exactly as many characters as were asked for, with the box scrolling over
// it sideways.
static void apply_width(void) {
	if (wide_mode()) {
		lv_obj_set_width(out_text, WIDTHS[width_step] * glyph_width() + 2);
		lv_obj_set_scroll_dir(out_box, LV_DIR_ALL);
	} else {
		lv_obj_set_width(out_text, lv_pct(100));
		lv_obj_set_scroll_dir(out_box, LV_DIR_VER);
		lv_obj_scroll_to_x(out_box, 0, LV_ANIM_OFF);
	}
}

// The output box ends where the input line begins, and the line sits on the
// keyboard, or on the bottom of the screen with the keyboard put away.
static void layout(void) {
	int top = settingsrow_content_top(g_cfg);
	int screen_w = g_cfg->screen_width;
	int screen_h = g_cfg->screen_height;

	int bottom = screen_h - (keyboard_is_visible(keyboard) ? TERM_KEYBOARD_H : 0);
	int line_y = bottom - FIELD_H - GAP;

	lv_obj_set_pos(field, MARGIN, line_y);
	lv_obj_set_pos(interrupt_btn, screen_w - MARGIN - INTERRUPT_W, line_y);

	// The row of keys the on-screen keyboard has none of sits between the output
	// and the line, and goes with the keyboard.
	bool keys = keyboard_is_visible(keyboard);
	int keys_y = line_y - KEYS_GAP - KEYS_H;
	lv_obj_set_hidden(key_row, !keys);
	if (keys) {
		lv_obj_set_pos(key_row, MARGIN, keys_y);
	}

	lv_obj_set_pos(out_box, MARGIN, top);
	lv_obj_set_size(out_box, screen_w - 2 * MARGIN, (keys ? keys_y - KEYS_GAP : line_y - GAP) - top);
	report_size();
}

static void scroll_to_end(void) {
	lv_obj_update_layout(out_box);
	lv_obj_scroll_to_y(out_box, LV_COORD_MAX, LV_ANIM_OFF);
}

static void refresh(bool force) {
	uint32_t serial = terminal_serial();
	if (!force && serial == shown_serial) {
		return;
	}
	shown_serial = serial;

	bool at_end = lv_obj_get_scroll_bottom(out_box) <= FOLLOW_SLACK;

	size_t run_count = 0;
	terminal_snapshot(shown, sizeof(shown), runs, MAX_RUNS, &run_count);

	// Rebuilt whole: a span for each stretch of one colour.
	while (lv_spangroup_get_span_count(out_text) > 0) {
		lv_spangroup_delete_span(out_text, lv_spangroup_get_child(out_text, 0));
	}
	size_t offset = 0;
	for (size_t i = 0; i < run_count; i++) {
		size_t len = runs[i].length;
		if (len == 0) {
			continue;
		}
		char kept = shown[offset + len];
		shown[offset + len] = '\0';

		lv_span_t *span = lv_spangroup_add_span(out_text);
		lv_span_set_text(span, shown + offset);
		if (runs[i].color != TERMINAL_DEFAULT_COLOR) {
			lv_style_set_text_color(lv_span_get_style(span), palette_color(runs[i].color));
		}

		shown[offset + len] = kept;
		offset += len;
	}
	lv_spangroup_refresh(out_text);

	if (follow || at_end) {
		scroll_to_end();
		follow = true;
	}
}

static void paint_buttons(void) {
	lv_obj_set_style_opa(smaller_btn, font_step > 0 ? LV_OPA_COVER : LV_OPA_30, 0);
	lv_obj_set_style_opa(larger_btn, font_step < FONT_STEPS - 1 ? LV_OPA_COVER : LV_OPA_30, 0);
	if (wide_mode()) {
		lv_label_set_text_fmt(width_label, "%d", WIDTHS[width_step]);
	} else {
		lv_label_set_text(width_label, tr("terminal_fit"));
	}
}

// Everything that depends on the size of the text or the width of the screen.
static void apply_metrics(void) {
	lv_obj_set_style_text_font(out_text, step_font(font_step), 0);
	apply_width();
	paint_buttons();
	report_size();
	refresh(true);
}

static void font_step_to(int step) {
	if (step < 0 || step >= FONT_STEPS || step == font_step) {
		return;
	}
	font_step = step;
	config_set_int("terminal", "font_px", FONT_SIZES[font_step]);
	config_save();
	apply_metrics();
}

static void smaller_cb(lv_event_t *e) {
	(void)e;
	font_step_to(font_step - 1);
}

static void larger_cb(lv_event_t *e) {
	(void)e;
	font_step_to(font_step + 1);
}

// Fit, then each width in turn, then back to fit.
static void width_cb(lv_event_t *e) {
	(void)e;
	width_step = (width_step + 1) % WIDTH_STEPS;
	config_set_int("terminal", "columns", WIDTHS[width_step]);
	config_save();
	apply_metrics();
}

static void scrolled_cb(lv_event_t *e) {
	(void)e;
	// Follows again once the finger has brought it back to the end.
	follow = lv_obj_get_scroll_bottom(out_box) <= FOLLOW_SLACK;
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	refresh(false);
}

static void keyboard_show(bool visible) {
	keyboard_set_visible(keyboard, visible);
	layout();
	if (follow) {
		scroll_to_end();
	}
}

static void field_clicked_cb(lv_event_t *e) {
	(void)e;
	keyboard_show(true);
}

static void output_clicked_cb(lv_event_t *e) {
	(void)e;
	keyboard_show(false); // the lines get the room the keys had
}

static void history_push(const char *line) {
	if (!line[0] || strlen(line) >= LINE_MAX_CHARS) {
		return;
	}
	if (history_count > 0 && !strcmp(history[history_count - 1], line)) {
		return;
	}
	if (history_count == HISTORY_MAX) {
		memmove(history[0], history[1], sizeof(history[0]) * (HISTORY_MAX - 1));
		history_count--;
	}
	snprintf(history[history_count++], LINE_MAX_CHARS, "%s", line);
}

static void run_cb(lv_event_t *e) {
	(void)e;
	history_push(lv_textarea_get_text(field));
	history_pos = -1;
	terminal_run(lv_textarea_get_text(field), g_cfg->sd_root_path);
	lv_textarea_set_text(field, "");
	follow = true;
	refresh(true);
}

static void interrupt_cb(lv_event_t *e) {
	(void)e;
	terminal_interrupt();
	refresh(true);
}

// The keys of the row. The shell is fed whole lines, so the arrows work on the
// line being typed: up and down go through what was run before, left and right
// move the caret, and Tab completes the last word.
typedef enum { KEY_TAB, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_TEXT } key_kind_t;

typedef struct {
	key_kind_t kind;
	const char *text; // KEY_TEXT: what it types
} key_def_t;

static void dark_field(lv_obj_t *obj);

static void history_show(int pos) {
	if (pos < 0) {
		history_pos = -1;
		lv_textarea_set_text(field, draft);
		return;
	}
	history_pos = pos;
	lv_textarea_set_text(field, history[pos]);
}

static void key_cb(lv_event_t *e) {
	const key_def_t *key = lv_event_get_user_data(e);
	switch (key->kind) {
	case KEY_UP:
		if (history_count == 0) {
			break;
		}
		if (history_pos < 0) {
			snprintf(draft, sizeof(draft), "%s", lv_textarea_get_text(field));
			history_show(history_count - 1);
		} else if (history_pos > 0) {
			history_show(history_pos - 1);
		}
		break;
	case KEY_DOWN:
		if (history_pos >= 0) {
			history_show(history_pos + 1 < history_count ? history_pos + 1 : -1);
		}
		break;
	case KEY_LEFT:
		lv_textarea_cursor_left(field);
		break;
	case KEY_RIGHT:
		lv_textarea_cursor_right(field);
		break;
	case KEY_TAB: {
		char completed[LINE_MAX_CHARS];
		const char *line = lv_textarea_get_text(field);
		if (terminal_complete(line, g_cfg->sd_root_path, completed, sizeof(completed))) {
			lv_textarea_set_text(field, completed);
		}
		refresh(true);
		break;
	}
	case KEY_TEXT:
		lv_textarea_add_text(field, key->text);
		break;
	}
}

static void build_key_row(lv_obj_t *parent, int width) {
	static const key_def_t keys[] = {
		{KEY_TAB, NULL},   {KEY_UP, NULL},	  {KEY_DOWN, NULL},	 {KEY_LEFT, NULL}, {KEY_RIGHT, NULL},
		{KEY_TEXT, "|"},   {KEY_TEXT, "/"},	  {KEY_TEXT, "~"},	 {KEY_TEXT, "-"},
	};
	const int count = (int)(sizeof(keys) / sizeof(keys[0]));

	key_row = lv_obj_create(parent);
	lv_obj_remove_style_all(key_row);
	lv_obj_set_size(key_row, width, KEYS_H);
	lv_obj_set_scrollable(key_row, false);
	lv_obj_set_flex_flow(key_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(key_row, 4, 0);

	// Tab is a little wider; the others share the rest.
	int key_w = (width - 4 * (count - 1)) / (count + 1);
	for (int i = 0; i < count; i++) {
		lv_obj_t *btn = lv_btn_create(key_row);
		lv_obj_set_size(btn, keys[i].kind == KEY_TAB ? 2 * key_w : key_w, KEYS_H);
		lv_obj_add_style(btn, &theme_style_card, 0);
		dark_field(btn);
		lv_obj_set_style_radius(btn, 10, 0);
		lv_obj_set_style_border_width(btn, 0, 0);
		lv_obj_set_style_shadow_width(btn, 0, 0);
		lv_obj_set_style_pad_all(btn, 0, 0);
		lv_obj_add_event_cb(btn, key_cb, LV_EVENT_CLICKED, (void *)&keys[i]);

		const lv_image_dsc_t *arrow = NULL;
		switch (keys[i].kind) {
		case KEY_UP:
			arrow = &icon_chevron_up;
			break;
		case KEY_DOWN:
			arrow = &icon_chevron_down;
			break;
		case KEY_LEFT:
			arrow = &icon_chevron_left;
			break;
		case KEY_RIGHT:
			arrow = &icon_chevron_right;
			break;
		default:
			break;
		}
		if (arrow) {
			lv_obj_t *img = lv_image_create(btn);
			lv_image_set_src(img, arrow);
			lv_obj_set_style_image_recolor(img, lv_color_hex(TEXT_COLOR), 0);
			lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
			lv_obj_center(img);
		} else {
			lv_obj_t *label = lv_label_create(btn);
			lv_label_set_text(label, keys[i].kind == KEY_TAB ? "Tab" : keys[i].text);
			lv_obj_set_style_text_color(label, lv_color_hex(TEXT_COLOR), 0);
			lv_obj_set_style_text_font(label, &font_ui_20, 0);
			lv_obj_center(label);
		}
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	// The card may have been put in, or the folder switched on, since last time.
	terminal_tools_prepare(g_cfg->sd_root_path);
	find_mono_font();
	terminal_open(g_cfg->sd_root_path);

	// A sideways drag scrolls the output here. The back arrow is the way out.
	back_gesture_blocked(true);

	keyboard_reset(keyboard);
	keyboard_show(true);
	apply_metrics();
	follow = true;
	scroll_to_end();
	lv_timer_resume(poll_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	back_gesture_blocked(false);
	lv_timer_pause(poll_timer);
	keyboard_set_visible(keyboard, false);
}

// A key of the corner: small, the theme's card, on the heading's line, which is
// centred on the back arrow.
static lv_obj_t *corner_button(const char *text, int x, int width, lv_obj_t **label_out, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(terminalpage_screen);
	lv_obj_set_size(btn, width, CORNER_BUTTON);
	lv_obj_add_style(btn, &theme_style_card, 0);
	lv_obj_set_style_radius(btn, 12, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_set_pos(btn, x, g_cfg->top_bar_height + g_cfg->padding + (56 - CORNER_BUTTON) / 2);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, text);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22_bold, 0);
	lv_obj_center(label);
	if (label_out) {
		*label_out = label;
	}
	return btn;
}

static void dark_field(lv_obj_t *obj) {
	lv_obj_set_style_bg_color(obj, lv_color_hex(SCREEN_COLOR), 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(obj, lv_color_hex(TEXT_COLOR), 0);
}

void terminalpage_init(gui_config_t *cfg) {
	g_cfg = cfg;

	// What was chosen last time: a size and a width, each looked up in its list.
	long px = config_get_int("terminal", "font_px", FONT_PX_DEFAULT);
	font_step = 0;
	for (int i = 0; i < FONT_STEPS; i++) {
		if (FONT_SIZES[i] <= px) {
			font_step = i;
		}
	}
	long columns = config_get_int("terminal", "columns", 0);
	width_step = 0;
	for (int i = 0; i < WIDTH_STEPS; i++) {
		if (WIDTHS[i] == columns) {
			width_step = i;
		}
	}

	lv_obj_add_style(terminalpage_screen, &theme_style_screen, 0);
	lv_obj_t *title = settingsrow_title(terminalpage_screen, cfg, "terminal");
	settingsrow_title_corner_slots(title, cfg, 3);

	// The output: a dark screen that scrolls, with the text in it as one group
	// of spans, one for each colour.
	out_box = lv_obj_create(terminalpage_screen);
	lv_obj_add_style(out_box, &theme_style_card, 0);
	dark_field(out_box);
	lv_obj_set_style_radius(out_box, 12, 0);
	lv_obj_set_style_border_width(out_box, 0, 0);
	lv_obj_set_style_shadow_width(out_box, 0, 0);
	lv_obj_set_style_pad_all(out_box, BOX_PAD, 0);
	lv_obj_set_scroll_dir(out_box, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(out_box, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(out_box, scrolled_cb, LV_EVENT_SCROLL_END, NULL);
	lv_obj_add_event_cb(out_box, output_clicked_cb, LV_EVENT_CLICKED, NULL);

	out_text = lv_spangroup_create(out_box);
	lv_obj_set_width(out_text, lv_pct(100));
	lv_obj_set_height(out_text, LV_SIZE_CONTENT);
	lv_obj_set_style_text_color(out_text, lv_color_hex(TEXT_COLOR), 0);
	lv_obj_set_style_text_font(out_text, ui_font(FONT_SIZES[font_step]), 0);

	// The input line, in the shape of the search page's.
	field = lv_textarea_create(terminalpage_screen);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr("terminal_command"));
	lv_obj_set_size(field, cfg->screen_width - 2 * MARGIN - INTERRUPT_W - GAP, FIELD_H);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_style(field, &theme_style_card, 0);
	dark_field(field);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_20, 0);
	lv_obj_add_event_cb(field, field_clicked_cb, LV_EVENT_CLICKED, NULL);
	keyboard_style_caret(field);

	interrupt_btn = lv_btn_create(terminalpage_screen);
	lv_obj_set_size(interrupt_btn, INTERRUPT_W, FIELD_H);
	lv_obj_add_style(interrupt_btn, &theme_style_card, 0);
	dark_field(interrupt_btn);
	lv_obj_set_style_radius(interrupt_btn, 12, 0);
	lv_obj_set_style_border_width(interrupt_btn, 0, 0);
	lv_obj_set_style_shadow_width(interrupt_btn, 0, 0);
	lv_obj_set_style_pad_all(interrupt_btn, 0, 0);
	lv_obj_add_event_cb(interrupt_btn, interrupt_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *interrupt_label = lv_label_create(interrupt_btn);
	lv_label_set_text(interrupt_label, "^C");
	lv_obj_set_style_text_color(interrupt_label, lv_color_hex(TEXT_COLOR), 0);
	lv_obj_set_style_text_font(interrupt_label, &font_ui_22, 0);
	lv_obj_center(interrupt_label);

	// The header's right-hand side: how wide the screen of the terminal is, then
	// the two keys for the size of the text, the larger one at the very edge.
	int larger_x = cfg->screen_width - cfg->padding - CORNER_BUTTON;
	int smaller_x = larger_x - CORNER_GAP - CORNER_BUTTON;
	int width_x = smaller_x - CORNER_GAP - WIDTH_BUTTON_W;
	larger_btn = corner_button("A+", larger_x, CORNER_BUTTON, NULL, larger_cb);
	smaller_btn = corner_button("A-", smaller_x, CORNER_BUTTON, NULL, smaller_cb);
	corner_button("", width_x, WIDTH_BUTTON_W, &width_label, width_cb);
	paint_buttons();

	build_key_row(terminalpage_screen, cfg->screen_width - 2 * MARGIN);

	keyboard = keyboard_create(terminalpage_screen, cfg->screen_width, TERM_KEYBOARD_H, field, NULL, "terminal_run", run_cb,
							   NULL);
	layout();

	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	lv_obj_add_event_cb(terminalpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(terminalpage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
