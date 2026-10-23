#include "calculatorpage.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/calc/calc.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *calculatorpage_screen;

// ---------------------------------------------------------------------------
// The keys
// ---------------------------------------------------------------------------

typedef enum {
	K_DIGIT,
	K_DOT,
	K_ADD,
	K_SUB,
	K_MUL,
	K_DIV,
	K_EQUALS,
	K_CLEAR,
	K_NEGATE,
	K_PERCENT,
	K_LPAREN,
	K_RPAREN,
	K_MC,
	K_MPLUS,
	K_MMINUS,
	K_MR,
	K_SECOND,
	K_SQUARE,
	K_CUBE,
	K_POWER,
	K_EXP,
	K_EXP10,
	K_RECIPROCAL,
	K_SQRT,
	K_CBRT,
	K_ROOT,
	K_LN,
	K_LOG,
	K_FACTORIAL,
	K_SIN,
	K_COS,
	K_TAN,
	K_E,
	K_EE,
	K_ANGLE,
	K_SINH,
	K_COSH,
	K_TANH,
	K_PI,
	K_RAND,
} key_id_t;

typedef enum {
	KIND_DIGIT,	   // the number keys: the card colour
	KIND_FUNCTION, // clear, sign, percent and the scientific keys: a step apart from it
	KIND_OPERATOR, // the four operations and equals: the accent
} key_kind_t;

typedef struct {
	key_id_t id;
	int digit;
	const char *label;
	const char *label_second; // what the key says, and does, after 2nd; NULL for none
	key_kind_t kind;
	int span; // columns of the basic pad it takes
	lv_obj_t *btn;
	lv_obj_t *label_obj;
} calc_key_t;

#define BASIC_COLUMNS 4
#define BASIC_ROWS 5
#define SCI_COLUMNS 6
#define SCI_ROWS 5

static calc_key_t basic_keys[] = {
	{K_CLEAR, 0, "AC", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_NEGATE, 0, "+/-", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_PERCENT, 0, "%", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_DIV, 0, "\xc3\xb7", NULL, KIND_OPERATOR, 1, NULL, NULL},
	{K_DIGIT, 7, "7", NULL, KIND_DIGIT, 1, NULL, NULL},
	{K_DIGIT, 8, "8", NULL, KIND_DIGIT, 1, NULL, NULL},
	{K_DIGIT, 9, "9", NULL, KIND_DIGIT, 1, NULL, NULL},
	{K_MUL, 0, "\xc3\x97", NULL, KIND_OPERATOR, 1, NULL, NULL},
	{K_DIGIT, 4, "4", NULL, KIND_DIGIT, 1, NULL, NULL},
	{K_DIGIT, 5, "5", NULL, KIND_DIGIT, 1, NULL, NULL},
	{K_DIGIT, 6, "6", NULL, KIND_DIGIT, 1, NULL, NULL},
	{K_SUB, 0, "\xe2\x88\x92", NULL, KIND_OPERATOR, 1, NULL, NULL},
	{K_DIGIT, 1, "1", NULL, KIND_DIGIT, 1, NULL, NULL},
	{K_DIGIT, 2, "2", NULL, KIND_DIGIT, 1, NULL, NULL},
	{K_DIGIT, 3, "3", NULL, KIND_DIGIT, 1, NULL, NULL},
	{K_ADD, 0, "+", NULL, KIND_OPERATOR, 1, NULL, NULL},
	{K_DIGIT, 0, "0", NULL, KIND_DIGIT, 2, NULL, NULL},
	{K_DOT, 0, ".", NULL, KIND_DIGIT, 1, NULL, NULL},
	{K_EQUALS, 0, "=", NULL, KIND_OPERATOR, 1, NULL, NULL},
};
#define BASIC_COUNT ((int)(sizeof(basic_keys) / sizeof(basic_keys[0])))

// ² ³ ʸ ˣ and the rest are in the interface's typeface; the cube root's own sign
// is not, so it is written as a cube and a root.
static calc_key_t sci_keys[] = {
	{K_LPAREN, 0, "(", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_RPAREN, 0, ")", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_MC, 0, "mc", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_MPLUS, 0, "m+", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_MMINUS, 0, "m\xe2\x88\x92", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_MR, 0, "mr", NULL, KIND_FUNCTION, 1, NULL, NULL},

	{K_SECOND, 0, "2nd", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_SQUARE, 0, "x\xc2\xb2", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_CUBE, 0, "x\xc2\xb3", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_POWER, 0, "x\xca\xb8", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_EXP, 0, "e\xcb\xa3", "2\xcb\xa3", KIND_FUNCTION, 1, NULL, NULL},
	{K_EXP10, 0, "10\xcb\xa3", NULL, KIND_FUNCTION, 1, NULL, NULL},

	{K_RECIPROCAL, 0, "1/x", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_SQRT, 0, "\xe2\x88\x9a" "x", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_CBRT, 0, "\xc2\xb3\xe2\x88\x9a" "x", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_ROOT, 0, "\xca\xb8\xe2\x88\x9a" "x", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_LN, 0, "ln", "log\xe2\x82\x82", KIND_FUNCTION, 1, NULL, NULL},
	{K_LOG, 0, "log", NULL, KIND_FUNCTION, 1, NULL, NULL},

	{K_FACTORIAL, 0, "x!", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_SIN, 0, "sin", "sin\xe2\x81\xbb\xc2\xb9", KIND_FUNCTION, 1, NULL, NULL},
	{K_COS, 0, "cos", "cos\xe2\x81\xbb\xc2\xb9", KIND_FUNCTION, 1, NULL, NULL},
	{K_TAN, 0, "tan", "tan\xe2\x81\xbb\xc2\xb9", KIND_FUNCTION, 1, NULL, NULL},
	{K_E, 0, "e", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_EE, 0, "EE", NULL, KIND_FUNCTION, 1, NULL, NULL},

	{K_ANGLE, 0, "Rad", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_SINH, 0, "sinh", "sinh\xe2\x81\xbb\xc2\xb9", KIND_FUNCTION, 1, NULL, NULL},
	{K_COSH, 0, "cosh", "cosh\xe2\x81\xbb\xc2\xb9", KIND_FUNCTION, 1, NULL, NULL},
	{K_TANH, 0, "tanh", "tanh\xe2\x81\xbb\xc2\xb9", KIND_FUNCTION, 1, NULL, NULL},
	{K_PI, 0, "\xcf\x80", NULL, KIND_FUNCTION, 1, NULL, NULL},
	{K_RAND, 0, "Rand", NULL, KIND_FUNCTION, 1, NULL, NULL},
};
#define SCI_COUNT ((int)(sizeof(sci_keys) / sizeof(sci_keys[0])))

// ---------------------------------------------------------------------------
// The state
// ---------------------------------------------------------------------------
//
// What has been typed is in two parts. `expr` holds everything up to the number
// being typed, in plain ASCII for the evaluator (digits . E + - * / ^ r and
// brackets); `entry` is that number, as typed. A key that works on one number
// (a function, percent, a constant) works on the entry and leaves its result
// there, and the next digit starts a new number in its place.

#define EXPR_MAX 128
#define ENTRY_MAX 32
#define DIGITS_MAX 15

static gui_config_t *g_cfg;
static char expr[EXPR_MAX];
static char entry[ENTRY_MAX];
static bool entry_is_result; // the entry is something worked out, not typed
static bool error;
static char shown_expr[EXPR_MAX * 2 + 8]; // what was just worked out, shown above the answer
static bool second;
static bool degrees;
static bool scientific;
static double memory;
static bool has_memory;

static lv_obj_t *expr_label;
static lv_obj_t *display_label;
static lv_obj_t *mode_btn;
static lv_obj_t *mode_label;
static lv_obj_t *back_key;

// ---------------------------------------------------------------------------
// Doing the sums
// ---------------------------------------------------------------------------

static bool is_operator_char(char c) { return c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == 'r'; }

static void clear_all(void) {
	expr[0] = '\0';
	entry[0] = '\0';
	entry_is_result = false;
	error = false;
	shown_expr[0] = '\0';
}

// A number as the entry holds it: all the digits it has, so a chain of sums does
// not lose anything to the ten the display shows.
static void set_entry(double value) {
	snprintf(entry, sizeof(entry), "%.12g", value);
	entry_is_result = true;
}

// A typed entry with what cannot be a number cut off its end ("5E", "5E-").
static void clean_entry(char *text) {
	size_t n = strlen(text);
	while (n > 0 && (text[n - 1] == 'E' || ((text[n - 1] == '-' || text[n - 1] == '+') && n > 1 && text[n - 2] == 'E'))) {
		text[--n] = '\0';
	}
	if (strcmp(text, "-") == 0) {
		text[0] = '\0';
	}
}

// The number the next function works on: the entry, or if there is none the
// number the pending sum has just got to.
static bool current_value(double *out) {
	char text[ENTRY_MAX];
	if (entry[0]) {
		snprintf(text, sizeof(text), "%s", entry);
		clean_entry(text);
		return calc_eval(text[0] ? text : "0", out);
	}
	if (!expr[0]) {
		*out = 0;
		return true;
	}
	// "5+" works on 5: the sum without its last operator.
	char head[EXPR_MAX];
	snprintf(head, sizeof(head), "%s", expr);
	size_t n = strlen(head);
	while (n > 0 && (is_operator_char(head[n - 1]) || head[n - 1] == '(')) {
		head[--n] = '\0';
	}
	if (n == 0) {
		*out = 0;
		return true;
	}
	char closed[EXPR_MAX + 16];
	int open = 0;
	for (size_t i = 0; i < n; i++) {
		open += head[i] == '(' ? 1 : head[i] == ')' ? -1 : 0;
	}
	snprintf(closed, sizeof(closed), "%s", head);
	for (; open > 0; open--) {
		strncat(closed, ")", sizeof(closed) - strlen(closed) - 1);
	}
	return calc_eval(closed, out);
}

// Moves the number being typed into the expression.
static void commit_entry(void) {
	if (!entry[0]) {
		return;
	}
	char text[ENTRY_MAX];
	snprintf(text, sizeof(text), "%s", entry);
	clean_entry(text);
	if (text[0]) {
		size_t n = strlen(expr);
		// A number straight after a closing bracket is multiplied by it.
		if (n > 0 && expr[n - 1] == ')' && n + 1 < sizeof(expr)) {
			expr[n++] = '*';
			expr[n] = '\0';
		}
		strncat(expr, text, sizeof(expr) - strlen(expr) - 1);
	}
	entry[0] = '\0';
	entry_is_result = false;
}

static void press_digit(int digit) {
	if (error) {
		clear_all();
	}
	if (entry_is_result) {
		entry[0] = '\0';
		entry_is_result = false;
	}
	shown_expr[0] = '\0';

	size_t n = strlen(entry);
	char *e = strchr(entry, 'E');
	if (e) {
		if (strlen(e + 1) - (e[1] == '-' || e[1] == '+' ? 1 : 0) >= 3) {
			return; // an exponent of three digits is plenty
		}
	} else {
		int digits = 0;
		for (size_t i = 0; i < n; i++) {
			digits += entry[i] >= '0' && entry[i] <= '9';
		}
		if (digits >= DIGITS_MAX) {
			return;
		}
		// 0 followed by a digit is that digit, but 0.5 stays 0.5.
		if (strcmp(entry, "0") == 0) {
			n = 0;
		} else if (strcmp(entry, "-0") == 0) {
			n = 1;
		}
	}
	if (n + 2 < sizeof(entry)) {
		entry[n] = (char)('0' + digit);
		entry[n + 1] = '\0';
	}
}

static void press_dot(void) {
	if (error) {
		clear_all();
	}
	if (entry_is_result || !entry[0]) {
		entry[0] = '\0';
		entry_is_result = false;
		snprintf(entry, sizeof(entry), "0.");
		shown_expr[0] = '\0';
		return;
	}
	if (strchr(entry, '.') || strchr(entry, 'E')) {
		return;
	}
	size_t n = strlen(entry);
	if (n + 2 < sizeof(entry)) {
		entry[n] = '.';
		entry[n + 1] = '\0';
	}
}

static void press_operator(char op) {
	if (error) {
		clear_all();
	}
	shown_expr[0] = '\0';
	commit_entry();
	size_t n = strlen(expr);
	if (n == 0) {
		snprintf(expr, sizeof(expr), "0");
		n = 1;
	}
	if (expr[n - 1] == '(') {
		return;
	}
	if (is_operator_char(expr[n - 1])) {
		expr[n - 1] = op; // the new operator takes the place of the last
		return;
	}
	if (n + 1 < sizeof(expr)) {
		expr[n] = op;
		expr[n + 1] = '\0';
	}
}

static void press_lparen(void) {
	if (error) {
		clear_all();
	}
	shown_expr[0] = '\0';
	commit_entry();
	size_t n = strlen(expr);
	if (n > 0 && (expr[n - 1] == ')' || (expr[n - 1] >= '0' && expr[n - 1] <= '9')) && n + 1 < sizeof(expr)) {
		expr[n++] = '*';
		expr[n] = '\0';
	}
	if (n + 1 < sizeof(expr)) {
		expr[n] = '(';
		expr[n + 1] = '\0';
	}
}

static int open_brackets(void) {
	int open = 0;
	for (const char *p = expr; *p; p++) {
		open += *p == '(' ? 1 : *p == ')' ? -1 : 0;
	}
	return open;
}

static void press_rparen(void) {
	if (error || open_brackets() <= 0) {
		return;
	}
	commit_entry();
	size_t n = strlen(expr);
	if (n == 0 || is_operator_char(expr[n - 1]) || expr[n - 1] == '(') {
		return;
	}
	if (n + 1 < sizeof(expr)) {
		expr[n] = ')';
		expr[n + 1] = '\0';
	}
}

// The sum written for the line above the answer: the operators as they are
// printed, and "r" for the root.
static void pretty(const char *src, char *out, size_t size) {
	size_t n = 0;
	for (const char *p = src; *p && n + 4 < size; p++) {
		const char *s = NULL;
		switch (*p) {
		case '*':
			s = "\xc3\x97";
			break;
		case '/':
			s = "\xc3\xb7";
			break;
		case '-':
			s = "\xe2\x88\x92";
			break;
		case 'r':
			s = "\xca\xb8\xe2\x88\x9a";
			break;
		default:
			break;
		}
		if (s) {
			for (; *s && n + 1 < size; s++) {
				out[n++] = *s;
			}
		} else {
			out[n++] = *p;
		}
	}
	out[n] = '\0';
}

static void press_equals(void) {
	if (error) {
		return;
	}
	commit_entry();
	size_t n = strlen(expr);
	if (n == 0) {
		return;
	}
	while (n > 0 && (is_operator_char(expr[n - 1]) || expr[n - 1] == '(')) {
		expr[--n] = '\0';
	}
	for (int open = open_brackets(); open > 0 && n + 1 < sizeof(expr); open--) {
		expr[n++] = ')';
		expr[n] = '\0';
	}
	double value;
	if (n == 0 || !calc_eval(expr, &value)) {
		error = true;
		expr[0] = '\0';
		return;
	}
	char text[EXPR_MAX * 2 + 8];
	pretty(expr, text, sizeof(text) - 3);
	snprintf(shown_expr, sizeof(shown_expr), "%s =", text);
	expr[0] = '\0';
	set_entry(value);
}

static void press_function(calc_fn_t fn) {
	if (error) {
		clear_all();
	}
	double x;
	double y;
	if (!current_value(&x) || !calc_apply(fn, x, degrees, &y)) {
		error = true;
		return;
	}
	shown_expr[0] = '\0';
	set_entry(y);
}

static void press_percent(void) {
	if (error) {
		clear_all();
	}
	double x;
	if (!current_value(&x)) {
		error = true;
		return;
	}
	// After + or -, a percent is of what is already there: 200 + 10% is 200 + 20.
	size_t n = strlen(expr);
	if (entry[0] && n > 1 && (expr[n - 1] == '+' || expr[n - 1] == '-')) {
		char head[EXPR_MAX];
		snprintf(head, sizeof(head), "%s", expr);
		head[n - 1] = '\0';
		double base;
		if (calc_eval(head, &base)) {
			set_entry(base * x / 100.0);
			return;
		}
	}
	set_entry(x / 100.0);
}

static void press_constant(double value) {
	if (error) {
		clear_all();
	}
	shown_expr[0] = '\0';
	set_entry(value);
}

static void press_negate(void) {
	if (error) {
		clear_all();
	}
	if (!entry[0]) {
		snprintf(entry, sizeof(entry), "-0");
		entry_is_result = false;
		return;
	}
	char *e = strchr(entry, 'E');
	if (e) {
		// The sign of the exponent.
		if (e[1] == '-') {
			memmove(e + 1, e + 2, strlen(e + 2) + 1);
		} else if (strlen(entry) + 2 < sizeof(entry)) {
			memmove(e + 2, e + 1, strlen(e + 1) + 1);
			e[1] = '-';
		}
		return;
	}
	if (entry[0] == '-') {
		memmove(entry, entry + 1, strlen(entry));
	} else if (strlen(entry) + 2 < sizeof(entry)) {
		memmove(entry + 1, entry, strlen(entry) + 1);
		entry[0] = '-';
	}
}

static void press_exponent(void) {
	if (error || !entry[0] || strchr(entry, 'E')) {
		return;
	}
	entry_is_result = false;
	size_t n = strlen(entry);
	if (n + 2 < sizeof(entry)) {
		entry[n] = 'E';
		entry[n + 1] = '\0';
	}
}

static void press_clear(void) {
	if (entry[0] && !entry_is_result && !error) {
		entry[0] = '\0'; // C: only the number being typed
	} else {
		clear_all(); // AC
	}
}

static void press_backspace(void) {
	if (error) {
		clear_all();
		return;
	}
	if (entry[0] && !entry_is_result) {
		entry[strlen(entry) - 1] = '\0';
		clean_entry(entry);
		return;
	}
	if (entry[0]) {
		return; // a result is not typed, so there is nothing of it to take back
	}
	size_t n = strlen(expr);
	if (n > 0) {
		expr[n - 1] = '\0';
	}
}

static void memory_add(double sign) {
	double x;
	if (current_value(&x)) {
		memory += sign * x;
		has_memory = true;
	}
}

// ---------------------------------------------------------------------------
// Showing it
// ---------------------------------------------------------------------------

static const lv_font_t *const DISPLAY_FONTS[] = {&font_ui_72, &font_ui_64_bold, &font_ui_36_bold, &font_ui_32,
												 &font_ui_28, &font_ui_24,	   &font_ui_20};
#define DISPLAY_FONT_COUNT ((int)(sizeof(DISPLAY_FONTS) / sizeof(DISPLAY_FONTS[0])))

// The number the display shows when nothing is being typed: the one the pending
// sum last had.
static void trailing_number(char *out, size_t size) {
	size_t n = strlen(expr);
	while (n > 0 && (is_operator_char(expr[n - 1]) || expr[n - 1] == '(')) {
		n--;
	}
	if (n > 0 && expr[n - 1] == ')') {
		double v;
		if (current_value(&v)) {
			calc_format(v, out, size);
			return;
		}
	}
	size_t start = n;
	while (start > 0 && ((expr[start - 1] >= '0' && expr[start - 1] <= '9') || expr[start - 1] == '.' ||
						 expr[start - 1] == 'E' ||
						 ((expr[start - 1] == '-' || expr[start - 1] == '+') && start > 1 && expr[start - 2] == 'E'))) {
		start--;
	}
	if (start == n) {
		snprintf(out, size, "0");
		return;
	}
	char number[ENTRY_MAX];
	snprintf(number, sizeof(number), "%.*s", (int)(n - start), expr + start);
	calc_group_entry(number, out, size);
}

static void update_display(void) {
	char text[96];
	if (error) {
		snprintf(text, sizeof(text), "%s", tr("calculator_error"));
	} else if (entry[0] && entry_is_result) {
		double v = 0;
		if (calc_eval(entry, &v)) {
			calc_format(v, text, sizeof(text));
		} else {
			snprintf(text, sizeof(text), "%s", entry);
		}
	} else if (entry[0]) {
		calc_group_entry(entry, text, sizeof(text));
	} else {
		trailing_number(text, sizeof(text));
	}

	// The largest size the number fits at.
	int available = (int)g_cfg->screen_width - 2 * (int)g_cfg->padding;
	int first = scientific ? 2 : 0;
	const lv_font_t *font = DISPLAY_FONTS[DISPLAY_FONT_COUNT - 1];
	for (int i = first; i < DISPLAY_FONT_COUNT; i++) {
		lv_point_t size;
		lv_text_get_size(&size, text, DISPLAY_FONTS[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (size.x <= available) {
			font = DISPLAY_FONTS[i];
			break;
		}
	}
	lv_obj_set_style_text_font(display_label, font, 0);
	lv_label_set_text(display_label, text);

	// Above it: what was just worked out, or the sum so far.
	char line[EXPR_MAX * 2 + 16];
	if (shown_expr[0]) {
		snprintf(line, sizeof(line), "%s", shown_expr);
	} else {
		pretty(expr, line, sizeof(line));
	}
	// A long sum shows its end.
	size_t length = strlen(line);
	const size_t keep = 34;
	char shown[EXPR_MAX * 2 + 8];
	if (length > keep) {
		size_t from = length - keep;
		while (from < length && ((unsigned char)line[from] & 0xc0) == 0x80) {
			from++;
		}
		snprintf(shown, sizeof(shown), "\xe2\x80\xa6%s", line + from);
	} else {
		snprintf(shown, sizeof(shown), "%s", line);
	}
	if (has_memory) {
		snprintf(line, sizeof(line), "M  %s", shown);
		lv_label_set_text(expr_label, line);
	} else {
		lv_label_set_text(expr_label, shown);
	}
}

static const char *key_text(const calc_key_t *k) {
	if (k->id == K_CLEAR) {
		return (entry[0] && !entry_is_result && !error) ? "C" : "AC";
	}
	if (k->id == K_ANGLE) {
		return degrees ? "Deg" : "Rad";
	}
	return (second && k->label_second) ? k->label_second : k->label;
}

// Which operator key is lit: the one the sum is waiting on.
static key_id_t pending_operator(void) {
	if (entry[0]) {
		return K_DIGIT;
	}
	size_t n = strlen(expr);
	if (n == 0) {
		return K_DIGIT;
	}
	switch (expr[n - 1]) {
	case '+':
		return K_ADD;
	case '-':
		return K_SUB;
	case '*':
		return K_MUL;
	case '/':
		return K_DIV;
	default:
		return K_DIGIT;
	}
}

static lv_color_t function_color(void) { return theme()->surface_pressed; }
static lv_color_t accent_color(void) { return theme_accent_preset(theme_get_accent()); }

static void paint_key(calc_key_t *k, key_id_t lit) {
	if (!k->btn) {
		return;
	}
	lv_color_t fill;
	lv_color_t ink = theme()->text_primary;
	if (k->kind == KIND_OPERATOR) {
		bool is_lit = k->id == lit;
		fill = is_lit ? lv_color_white() : accent_color();
		ink = is_lit ? accent_color() : lv_color_white();
	} else if (k->kind == KIND_FUNCTION) {
		fill = function_color();
		if (k->id == K_SECOND && second) {
			fill = accent_color();
			ink = lv_color_white();
		}
	} else {
		fill = theme()->surface;
	}
	lv_obj_set_style_bg_color(k->btn, fill, 0);
	lv_obj_set_style_text_color(k->label_obj, ink, 0);
	lv_label_set_text(k->label_obj, key_text(k));
}

static void paint_all(void) {
	key_id_t lit = pending_operator();
	for (int i = 0; i < BASIC_COUNT; i++) {
		paint_key(&basic_keys[i], lit);
	}
	for (int i = 0; i < SCI_COUNT; i++) {
		paint_key(&sci_keys[i], lit);
	}
	lv_obj_set_style_bg_color(mode_btn, scientific ? accent_color() : function_color(), 0);
	lv_obj_set_style_text_color(mode_label, scientific ? lv_color_white() : theme()->text_primary, 0);
	lv_label_set_text(mode_label, tr(scientific ? "calculator_basic" : "calculator_scientific"));
	lv_obj_set_style_bg_color(back_key, function_color(), 0);
	update_display();
}

static void theme_refresh(void) {
	if (mode_btn) {
		paint_all();
	}
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

#define GAP 10
#define SCI_GAP 5
#define MODE_W 104
#define BACK_W 60
#define HEADER_H 40

static void place_key(calc_key_t *k, int x, int y, int w, int h) {
	lv_obj_set_pos(k->btn, x, y);
	lv_obj_set_size(k->btn, w, h);
	lv_obj_set_style_radius(k->btn, h / 2 < 30 ? h / 2 : 30, 0);
}

static void layout(void) {
	int width = (int)g_cfg->screen_width;
	int height = (int)g_cfg->screen_height;
	int pad = (int)g_cfg->padding;

	// Along the top, to the right of the back arrow: the key that takes the last
	// digit back, and the switch to the scientific keys.
	int header_y = (int)g_cfg->top_bar_height + pad + 8;
	lv_obj_set_pos(mode_btn, width - pad - MODE_W, header_y);
	lv_obj_set_size(mode_btn, MODE_W, HEADER_H);
	lv_obj_set_pos(back_key, width - pad - MODE_W - GAP - BACK_W, header_y);
	lv_obj_set_size(back_key, BACK_W, HEADER_H);

	int expr_y = header_y + HEADER_H + 4;
	lv_obj_set_pos(expr_label, pad, expr_y);
	lv_obj_set_width(expr_label, width - 2 * pad);

	int display_y = expr_y + 26;
	int display_h = scientific ? 52 : 84;
	lv_obj_set_pos(display_label, pad, display_y);
	lv_obj_set_size(display_label, width - 2 * pad, display_h);

	int top = display_y + display_h + 6;
	int bottom = height - 10;

	// The scientific keys take their share above the basic ones.
	int sci_h = 0;
	int sci_row_h = 0;
	if (scientific) {
		sci_row_h = 38;
		sci_h = SCI_ROWS * sci_row_h + (SCI_ROWS - 1) * SCI_GAP + 8;
	}
	int sci_w = (width - 2 * pad - (SCI_COLUMNS - 1) * SCI_GAP) / SCI_COLUMNS;
	for (int i = 0; i < SCI_COUNT; i++) {
		calc_key_t *k = &sci_keys[i];
		if (!scientific) {
			lv_obj_set_hidden(k->btn, true);
			continue;
		}
		lv_obj_set_hidden(k->btn, false);
		int col = i % SCI_COLUMNS;
		int row = i / SCI_COLUMNS;
		place_key(k, pad + col * (sci_w + SCI_GAP), top + row * (sci_row_h + SCI_GAP), sci_w, sci_row_h);
	}

	int basic_top = top + sci_h;
	int row_h = (bottom - basic_top - (BASIC_ROWS - 1) * GAP) / BASIC_ROWS;
	int col_w = (width - 2 * pad - (BASIC_COLUMNS - 1) * GAP) / BASIC_COLUMNS;
	int col = 0;
	int row = 0;
	for (int i = 0; i < BASIC_COUNT; i++) {
		calc_key_t *k = &basic_keys[i];
		int w = k->span * col_w + (k->span - 1) * GAP;
		place_key(k, pad + col * (col_w + GAP), basic_top + row * (row_h + GAP), w, row_h);
		col += k->span;
		if (col >= BASIC_COLUMNS) {
			col = 0;
			row++;
		}
	}
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static void save_settings(void) {
	config_set_bool("calculator", "scientific", scientific);
	config_set_bool("calculator", "degrees", degrees);
	config_save();
}

static void key_pressed(const calc_key_t *k) {
	switch (k->id) {
	case K_DIGIT:
		press_digit(k->digit);
		break;
	case K_DOT:
		press_dot();
		break;
	case K_ADD:
		press_operator('+');
		break;
	case K_SUB:
		press_operator('-');
		break;
	case K_MUL:
		press_operator('*');
		break;
	case K_DIV:
		press_operator('/');
		break;
	case K_POWER:
		press_operator('^');
		break;
	case K_ROOT:
		press_operator('r');
		break;
	case K_EQUALS:
		press_equals();
		break;
	case K_CLEAR:
		press_clear();
		break;
	case K_NEGATE:
		press_negate();
		break;
	case K_PERCENT:
		press_percent();
		break;
	case K_LPAREN:
		press_lparen();
		break;
	case K_RPAREN:
		press_rparen();
		break;
	case K_MC:
		memory = 0;
		has_memory = false;
		break;
	case K_MPLUS:
		memory_add(1);
		break;
	case K_MMINUS:
		memory_add(-1);
		break;
	case K_MR:
		if (has_memory) {
			press_constant(memory);
		}
		break;
	case K_SECOND:
		second = !second;
		break;
	case K_SQUARE:
		press_function(CALC_SQUARE);
		break;
	case K_CUBE:
		press_function(CALC_CUBE);
		break;
	case K_EXP:
		press_function(second ? CALC_EXP2 : CALC_EXP);
		break;
	case K_EXP10:
		press_function(CALC_EXP10);
		break;
	case K_RECIPROCAL:
		press_function(CALC_RECIPROCAL);
		break;
	case K_SQRT:
		press_function(CALC_SQRT);
		break;
	case K_CBRT:
		press_function(CALC_CBRT);
		break;
	case K_LN:
		press_function(second ? CALC_LOG2 : CALC_LN);
		break;
	case K_LOG:
		press_function(CALC_LOG10);
		break;
	case K_FACTORIAL:
		press_function(CALC_FACTORIAL);
		break;
	case K_SIN:
		press_function(second ? CALC_ASIN : CALC_SIN);
		break;
	case K_COS:
		press_function(second ? CALC_ACOS : CALC_COS);
		break;
	case K_TAN:
		press_function(second ? CALC_ATAN : CALC_TAN);
		break;
	case K_SINH:
		press_function(second ? CALC_ASINH : CALC_SINH);
		break;
	case K_COSH:
		press_function(second ? CALC_ACOSH : CALC_COSH);
		break;
	case K_TANH:
		press_function(second ? CALC_ATANH : CALC_TANH);
		break;
	case K_E:
		press_constant(2.718281828459045);
		break;
	case K_PI:
		press_constant(3.141592653589793);
		break;
	case K_RAND:
		press_constant((double)rand() / ((double)RAND_MAX + 1.0));
		break;
	case K_EE:
		press_exponent();
		break;
	case K_ANGLE:
		degrees = !degrees;
		save_settings();
		break;
	}
	paint_all();
}

static void key_cb(lv_event_t *e) {
	const calc_key_t *k = lv_event_get_user_data(e);
	key_pressed(k);
}

static void backspace_cb(lv_event_t *e) {
	(void)e;
	press_backspace();
	paint_all();
}

static void mode_cb(lv_event_t *e) {
	(void)e;
	scientific = !scientific;
	save_settings();
	layout();
	paint_all();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	layout();
	paint_all();
}

// ---------------------------------------------------------------------------
// Building the page
// ---------------------------------------------------------------------------

static lv_obj_t *make_button(lv_obj_t *parent, lv_obj_t **label_out, const lv_font_t *font) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_set_style_opa(btn, LV_OPA_70, LV_STATE_PRESSED);
	lv_obj_t *label = lv_label_create(btn);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_center(label);
	*label_out = label;
	return btn;
}

static void build_keys(calc_key_t *keys, int count, const lv_font_t *font, lv_obj_t *screen) {
	for (int i = 0; i < count; i++) {
		calc_key_t *k = &keys[i];
		k->btn = make_button(screen, &k->label_obj, font);
		lv_obj_add_event_cb(k->btn, key_cb, LV_EVENT_CLICKED, k);
	}
}

void calculatorpage_init(gui_config_t *cfg) {
	g_cfg = cfg;
	srand((unsigned)time(NULL));
	scientific = config_get_bool("calculator", "scientific", false);
	degrees = config_get_bool("calculator", "degrees", true);

	lv_obj_add_style(calculatorpage_screen, &theme_style_screen, 0);
	// No heading: the back arrow stays at the corner and the display takes the
	// rest of the width.

	expr_label = lv_label_create(calculatorpage_screen);
	lv_obj_add_style(expr_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(expr_label, &font_ui_20, 0);
	lv_obj_set_style_text_align(expr_label, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode(expr_label, LV_LABEL_LONG_CLIP);
	lv_label_set_text(expr_label, "");

	display_label = lv_label_create(calculatorpage_screen);
	lv_obj_add_style(display_label, &theme_style_text, 0);
	lv_obj_set_style_text_align(display_label, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode(display_label, LV_LABEL_LONG_CLIP);
	lv_label_set_text(display_label, "0");

	// The scientific keys first, so the basic ones are on top of them where they
	// would meet.
	build_keys(sci_keys, SCI_COUNT, &font_ui_18, calculatorpage_screen);
	build_keys(basic_keys, BASIC_COUNT, &font_ui_32, calculatorpage_screen);
	for (int i = 0; i < BASIC_COUNT; i++) {
		if (basic_keys[i].id == K_CLEAR || basic_keys[i].id == K_NEGATE || basic_keys[i].id == K_PERCENT) {
			lv_obj_set_style_text_font(basic_keys[i].label_obj, &font_ui_28, 0);
		}
	}

	mode_btn = make_button(calculatorpage_screen, &mode_label, &font_ui_20);
	lv_obj_add_event_cb(mode_btn, mode_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *back_icon;
	back_key = make_button(calculatorpage_screen, &back_icon, &font_ui_20);
	lv_obj_add_event_cb(back_key, backspace_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_style_radius(back_key, 12, 0);
	lv_obj_set_style_radius(mode_btn, 12, 0);
	lv_obj_delete(back_icon);
	lv_obj_t *icon = lv_image_create(back_key);
	lv_image_set_src(icon, &icon_delete);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);

	layout();
	paint_all();

	theme_register_refresh(theme_refresh);
	lv_obj_add_event_cb(calculatorpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(calculatorpage_screen);
}
