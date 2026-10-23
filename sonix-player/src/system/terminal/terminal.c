#ifndef _GNU_SOURCE
#define _GNU_SOURCE // pipe2, ptsname_r
#endif

#include "terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/device/system.h"

extern char **environ;

// ---------------------------------------------------------------------------
// The output: a grid of cells
// ---------------------------------------------------------------------------
//
// What the shell and its programs print is applied to lines of cells the way a
// terminal would: text goes in at the cursor, a carriage return and the cursor
// movements put the cursor elsewhere, the erase sequences blank cells, and the
// colour sequences set the colour of what is written next. Only the end of the
// output is kept: MAX_LINES lines, then the oldest goes.
//
// There is no screen to scroll and no home position, only the lines and a
// cursor in them. That is enough for everything a program prints and leaves
// behind; absolute positioning (a program drawing on the whole screen) is not
// followed past its column.
//
// Everything here is under `lock`, which the reader thread and the page share.

#define MAX_LINES 300
#define MAX_COLS 240 // a longer line carries on in the next one

#define COLOR_DEFAULT TERMINAL_DEFAULT_COLOR
#define ATTR_DEFAULT ((uint16_t)0xFFFF) // foreground in the low byte, background in the high

typedef struct {
	uint32_t ch;
	uint16_t attr;
} cell_t;

typedef struct {
	cell_t *cells;
	uint16_t length;
	uint16_t capacity;
} line_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static line_t lines[MAX_LINES]; // a ring: line_first is the oldest
static int line_first;
static int line_count = 1;
static int cur_row;
static int cur_col;
static uint16_t cur_attr = ATTR_DEFAULT;
static bool cur_bold;
static uint32_t serial;

static line_t *line_at(int row) { return &lines[(line_first + row) % MAX_LINES]; }

static void line_free(line_t *line) {
	free(line->cells);
	line->cells = NULL;
	line->length = 0;
	line->capacity = 0;
}

static void grid_reset(void) {
	for (int i = 0; i < line_count; i++) {
		line_free(line_at(i));
	}
	line_first = 0;
	line_count = 1;
	cur_row = 0;
	cur_col = 0;
	cur_attr = ATTR_DEFAULT;
	cur_bold = false;
	serial++;
}

// The cursor goes to the start of the next line, which is added if there is none;
// at MAX_LINES the oldest line is dropped to make room.
static void line_feed(void) {
	cur_col = 0;
	if (cur_row < line_count - 1) {
		cur_row++;
		return;
	}
	if (line_count == MAX_LINES) {
		line_free(&lines[line_first]);
		line_first = (line_first + 1) % MAX_LINES;
		line_count--;
	}
	line_count++;
	cur_row = line_count - 1;
	line_free(line_at(cur_row));
}

static bool line_reserve(line_t *line, int cells) {
	if (cells <= line->capacity) {
		return true;
	}
	int capacity = line->capacity ? line->capacity : 16;
	while (capacity < cells) {
		capacity *= 2;
	}
	if (capacity > MAX_COLS + 1) {
		capacity = MAX_COLS + 1;
	}
	cell_t *grown = realloc(line->cells, (size_t)capacity * sizeof(cell_t));
	if (!grown) {
		return false;
	}
	line->cells = grown;
	line->capacity = (uint16_t)capacity;
	return true;
}

static void line_pad(line_t *line, int to) {
	for (int i = line->length; i < to; i++) {
		line->cells[i].ch = ' ';
		line->cells[i].attr = ATTR_DEFAULT;
	}
	if (to > line->length) {
		line->length = (uint16_t)to;
	}
}

static void put_cell(uint32_t ch) {
	if (cur_col >= MAX_COLS) {
		line_feed();
	}
	line_t *line = line_at(cur_row);
	if (!line_reserve(line, cur_col + 1)) {
		return;
	}
	line_pad(line, cur_col);
	line->cells[cur_col].ch = ch;
	line->cells[cur_col].attr = cur_attr;
	if (line->length < cur_col + 1) {
		line->length = (uint16_t)(cur_col + 1);
	}
	cur_col++;
}

// ---------------------------------------------------------------------------
// Reading what the programs write
// ---------------------------------------------------------------------------

typedef enum {
	P_TEXT,
	P_ESC,
	P_CSI,
	P_OSC,
	P_OSC_ESC,
	P_CHARSET,
} parse_state_t;

#define MAX_PARAMS 16

typedef struct {
	parse_state_t state;
	int params[MAX_PARAMS];
	int param_count;
	int number;
	bool have_number;
	bool private_mode; // CSI ? ... : modes of the screen, not followed
	uint32_t code_point;
	int utf8_left;
} parser_t;

static parser_t local_parser; // for what the player writes itself

// A colour of the 256-colour palette nearest to r, g, b.
static int rgb_to_palette(int r, int g, int b) {
	if (r == g && g == b) {
		if (r < 8) {
			return 16;
		}
		if (r > 248) {
			return 231;
		}
		return 232 + (r - 8) / 10;
	}
	int ri = (r * 5 + 127) / 255;
	int gi = (g * 5 + 127) / 255;
	int bi = (b * 5 + 127) / 255;
	return 16 + 36 * ri + 6 * gi + bi;
}

// 0xFF is "default" in a cell, so the palette's own last colour, a near-white
// grey, is drawn as the one before it.
static void set_fg(int color) {
	if (color == 255) {
		color = 254;
	}
	cur_attr = (uint16_t)((cur_attr & 0xFF00) | (color & 0xFF));
}

static void set_bg(int color) {
	if (color == 255) {
		color = 254;
	}
	cur_attr = (uint16_t)((cur_attr & 0x00FF) | ((color & 0xFF) << 8));
}

static void sgr(const int *p, int n) {
	static const int reset[1] = {0};
	if (n == 0) {
		p = reset; // a bare ESC [ m is a reset
		n = 1;
	}
	for (int i = 0; i < n; i++) {
		int c = p[i];
		if (c == 0) {
			cur_attr = ATTR_DEFAULT;
			cur_bold = false;
		} else if (c == 1) {
			cur_bold = true;
			int fg = cur_attr & 0xFF;
			if (fg < 8) {
				set_fg(fg + 8);
			}
		} else if (c == 22) {
			cur_bold = false;
		} else if (c >= 30 && c <= 37) {
			set_fg(c - 30 + (cur_bold ? 8 : 0));
		} else if (c == 39) {
			cur_attr |= 0x00FF;
		} else if (c >= 40 && c <= 47) {
			set_bg(c - 40);
		} else if (c == 49) {
			cur_attr |= 0xFF00;
		} else if (c >= 90 && c <= 97) {
			set_fg(c - 90 + 8);
		} else if (c >= 100 && c <= 107) {
			set_bg(c - 100 + 8);
		} else if (c == 38 || c == 48) {
			int color = -1;
			if (i + 2 < n && p[i + 1] == 5) {
				color = p[i + 2] & 0xFF;
				i += 2;
			} else if (i + 4 < n && p[i + 1] == 2) {
				color = rgb_to_palette(p[i + 2] & 0xFF, p[i + 3] & 0xFF, p[i + 4] & 0xFF);
				i += 4;
			}
			if (color >= 0) {
				if (c == 38) {
					set_fg(color);
				} else {
					set_bg(color);
				}
			}
		}
	}
}

static int param(const parser_t *p, int index, int fallback) {
	if (index < p->param_count && p->params[index] > 0) {
		return p->params[index];
	}
	return fallback;
}

static void erase_in_line(int mode) {
	line_t *line = line_at(cur_row);
	if (mode == 0) {
		if (cur_col < line->length) {
			line->length = (uint16_t)cur_col;
		}
	} else if (mode == 1) {
		int to = cur_col + 1 < line->length ? cur_col + 1 : line->length;
		for (int i = 0; i < to; i++) {
			line->cells[i].ch = ' ';
			line->cells[i].attr = ATTR_DEFAULT;
		}
	} else {
		line->length = 0;
	}
}

static void erase_in_display(int mode) {
	if (mode == 0) {
		erase_in_line(0);
		while (line_count - 1 > cur_row) {
			line_free(line_at(line_count - 1));
			line_count--;
		}
	} else if (mode >= 2) {
		grid_reset();
	}
}

static void csi_dispatch(parser_t *p, char final) {
	if (p->private_mode) {
		return;
	}
	switch (final) {
	case 'm':
		sgr(p->params, p->param_count);
		break;
	case 'A': // up
		cur_row -= param(p, 0, 1);
		if (cur_row < 0) {
			cur_row = 0;
		}
		break;
	case 'B': // down, adding lines at the end
		for (int n = param(p, 0, 1); n > 0; n--) {
			int col = cur_col;
			line_feed();
			cur_col = col;
		}
		break;
	case 'C': // forward
		cur_col += param(p, 0, 1);
		if (cur_col > MAX_COLS - 1) {
			cur_col = MAX_COLS - 1;
		}
		break;
	case 'D': // back
		cur_col -= param(p, 0, 1);
		if (cur_col < 0) {
			cur_col = 0;
		}
		break;
	case 'E': // down to the start of a line
		for (int n = param(p, 0, 1); n > 0; n--) {
			line_feed();
		}
		break;
	case 'F': // up to the start of a line
		cur_row -= param(p, 0, 1);
		if (cur_row < 0) {
			cur_row = 0;
		}
		cur_col = 0;
		break;
	case 'G': // to a column
		cur_col = param(p, 0, 1) - 1;
		if (cur_col > MAX_COLS - 1) {
			cur_col = MAX_COLS - 1;
		}
		break;
	case 'H':
	case 'f': // the column only, there being no screen to count rows on
		cur_col = param(p, 1, 1) - 1;
		if (cur_col > MAX_COLS - 1) {
			cur_col = MAX_COLS - 1;
		}
		break;
	case 'J':
		erase_in_display(p->param_count > 0 ? p->params[0] : 0);
		break;
	case 'K':
		erase_in_line(p->param_count > 0 ? p->params[0] : 0);
		break;
	case 'X': { // erase characters
		line_t *line = line_at(cur_row);
		for (int i = cur_col; i < cur_col + param(p, 0, 1) && i < line->length; i++) {
			line->cells[i].ch = ' ';
			line->cells[i].attr = ATTR_DEFAULT;
		}
		break;
	}
	default:
		break;
	}
}

static void put_code_point(uint32_t cp) {
	if (cp == '\n') {
		line_feed();
	} else if (cp == '\r') {
		cur_col = 0;
	} else if (cp == '\b') {
		if (cur_col > 0) {
			cur_col--;
		}
	} else if (cp == '\t') {
		cur_col = (cur_col / 8 + 1) * 8;
		if (cur_col > MAX_COLS - 1) {
			cur_col = MAX_COLS - 1;
		}
	} else if (cp >= 0x20 && cp != 0x7f && !(cp >= 0x80 && cp < 0xa0)) {
		put_cell(cp);
	}
}

static void feed(parser_t *p, unsigned char c) {
	switch (p->state) {
	case P_ESC:
		if (c == '[') {
			p->state = P_CSI;
			p->param_count = 0;
			p->number = 0;
			p->have_number = false;
			p->private_mode = false;
		} else if (c == ']') {
			p->state = P_OSC;
		} else if (c == '(' || c == ')') {
			p->state = P_CHARSET;
		} else {
			p->state = P_TEXT; // a two-byte sequence, nothing to show
		}
		return;
	case P_CSI:
		if (c >= '0' && c <= '9') {
			p->number = p->number * 10 + (c - '0');
			if (p->number > 9999) {
				p->number = 9999;
			}
			p->have_number = true;
		} else if (c == ';') {
			if (p->param_count < MAX_PARAMS) {
				p->params[p->param_count++] = p->have_number ? p->number : 0;
			}
			p->number = 0;
			p->have_number = false;
		} else if (c >= 0x3c && c <= 0x3f) {
			p->private_mode = true;
		} else if (c >= 0x40 && c <= 0x7e) {
			if ((p->have_number || p->param_count > 0) && p->param_count < MAX_PARAMS) {
				p->params[p->param_count++] = p->have_number ? p->number : 0;
			}
			csi_dispatch(p, (char)c);
			p->state = P_TEXT;
		} else if (c < 0x20) {
			p->state = P_TEXT; // broken off
		}
		return;
	case P_OSC:
		if (c == 0x07) {
			p->state = P_TEXT;
		} else if (c == 0x1b) {
			p->state = P_OSC_ESC;
		}
		return;
	case P_OSC_ESC:
		p->state = P_TEXT; // the backslash that ends it, or whatever came
		return;
	case P_CHARSET:
		p->state = P_TEXT;
		return;
	case P_TEXT:
		break;
	}

	if (p->utf8_left > 0) {
		if ((c & 0xc0) == 0x80) {
			p->code_point = (p->code_point << 6) | (c & 0x3f);
			if (--p->utf8_left == 0) {
				put_code_point(p->code_point);
			}
			return;
		}
		p->utf8_left = 0; // cut short: this byte starts something else
	}

	if (c == 0x1b) {
		p->state = P_ESC;
	} else if (c < 0x80) {
		put_code_point(c);
	} else if ((c & 0xe0) == 0xc0) {
		p->code_point = c & 0x1f;
		p->utf8_left = 1;
	} else if ((c & 0xf0) == 0xe0) {
		p->code_point = c & 0x0f;
		p->utf8_left = 2;
	} else if ((c & 0xf8) == 0xf0) {
		p->code_point = c & 0x07;
		p->utf8_left = 3;
	}
}

// With `lock` held. For what the player puts in the output itself: the line
// that was typed, the greeting, the notes.
static void write_local(const char *s) {
	for (; *s; s++) {
		feed(&local_parser, (unsigned char)*s);
	}
	serial++;
}

static void write_local_fmt(const char *fmt, const char *arg) {
	char line[640];
	snprintf(line, sizeof(line), fmt, arg);
	write_local(line);
}

// A note starts its own line, in the default colour.
static void write_note(const char *s) {
	cur_attr = ATTR_DEFAULT;
	cur_bold = false;
	if (cur_col != 0) {
		line_feed();
	}
	write_local(s);
}

// ---------------------------------------------------------------------------
// Handing the output to the page
// ---------------------------------------------------------------------------

static size_t utf8_length(uint32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

static size_t utf8_put(char *out, uint32_t cp) {
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

// What a cell is drawn as: a space with a background colour is a block in that
// colour, since the page has no background to give it.
static uint32_t cell_glyph(const cell_t *cell, int *color) {
	int fg = cell->attr & 0xFF;
	int bg = cell->attr >> 8;
	if (cell->ch == ' ' && bg != COLOR_DEFAULT) {
		*color = bg;
		return 0x2588;
	}
	*color = fg;
	return cell->ch;
}

// How many cells of a line are worth drawing: the blanks at its end with no
// background are not.
static int visible_cells(const line_t *line) {
	int n = line->length;
	while (n > 0 && line->cells[n - 1].ch == ' ' && (line->cells[n - 1].attr >> 8) == COLOR_DEFAULT) {
		n--;
	}
	return n;
}

static size_t line_bytes(const line_t *line) {
	size_t bytes = 0;
	int n = visible_cells(line);
	for (int i = 0; i < n; i++) {
		int color;
		bytes += utf8_length(cell_glyph(&line->cells[i], &color));
	}
	return bytes;
}

size_t terminal_snapshot(char *out, size_t size, terminal_run_t *runs, size_t max_runs, size_t *run_count) {
	if (run_count) {
		*run_count = 0;
	}
	if (!out || size == 0) {
		return 0;
	}
	out[0] = '\0';

	pthread_mutex_lock(&lock);

	// From the end back, as many lines as fit; a single line too long for the
	// space is left out rather than cut.
	int first = line_count;
	size_t total = 0;
	while (first > 0) {
		size_t bytes = line_bytes(line_at(first - 1)) + (first == line_count ? 0 : 1);
		if (total + bytes > size - 1) {
			break;
		}
		total += bytes;
		first--;
	}

	size_t n = 0;
	size_t count = 0;
	int run_color = COLOR_DEFAULT;
	if (runs && max_runs > 0) {
		// Something covers the text from its first byte, even a first line that
		// is empty.
		runs[count].color = COLOR_DEFAULT;
		runs[count].length = 0;
		count++;
	}
	for (int row = first; row < line_count; row++) {
		const line_t *line = line_at(row);
		int cells = visible_cells(line);
		for (int i = 0; i < cells; i++) {
			int color;
			uint32_t cp = cell_glyph(&line->cells[i], &color);
			if (runs && color != run_color && count < max_runs) {
				runs[count].color = (uint16_t)color;
				runs[count].length = 0;
				count++;
				run_color = color;
			}
			size_t len = utf8_put(out + n, cp);
			n += len;
			if (runs && count > 0) {
				runs[count - 1].length = (uint16_t)(runs[count - 1].length + len);
			}
		}
		if (row != line_count - 1) {
			out[n++] = '\n';
			if (runs && count > 0) {
				runs[count - 1].length++;
			}
		}
	}
	out[n] = '\0';
	if (run_count) {
		*run_count = count;
	}

	pthread_mutex_unlock(&lock);
	return n;
}

// ---------------------------------------------------------------------------
// The folder on the card
// ---------------------------------------------------------------------------

#define CFG_SECTION "terminal"
#define CFG_TOOLS "tools"

bool terminal_tools_enabled(void) { return config_get_bool(CFG_SECTION, CFG_TOOLS, false); }

static bool make_dir(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST; }

static const char README[] =
	"Terminal folder\r\n"
	"===============\r\n"
	"\r\n"
	"The Terminal app on the player runs the programs in this folder, because the\r\n"
	"player's own filesystem is read-only.\r\n"
	"\r\n"
	"  bin/   Copy programs here. It is first on the Terminal's PATH, so a file\r\n"
	"         named fastfetch in bin is started by typing fastfetch.\r\n"
	"  lib/   Shared libraries those programs need (LD_LIBRARY_PATH).\r\n"
	"\r\n"
	"A program has to be built for this player: 32-bit little-endian MIPS\r\n"
	"(mipsel, MIPS32r2), either linked statically or against glibc 2.22.\r\n"
	"\r\n"
	"The Terminal starts in this folder. font.ttf, if you put one here, is used\r\n"
	"as the Terminal's typeface instead of the built-in one (DejaVu Sans Mono);\r\n"
	"it should be monospaced.\r\n";

bool terminal_tools_prepare(const char *sd_root) {
	if (!terminal_tools_enabled() || !sd_root || !sd_root[0]) {
		return false;
	}

	// With no card the mount point is only an empty directory on the player's
	// own flash: a folder made there would sit under the card, out of sight,
	// from the day one is put in.
	if (!storage_card_attached() || !storage_sd_writable()) {
		return false;
	}

	char dir[512];
	if (snprintf(dir, sizeof(dir), "%s/" TERMINAL_FOLDER, sd_root) >= (int)sizeof(dir) || !make_dir(dir)) {
		return false;
	}

	char sub[544];
	snprintf(sub, sizeof(sub), "%s/bin", dir);
	bool ok = make_dir(sub);
	snprintf(sub, sizeof(sub), "%s/lib", dir);
	ok = make_dir(sub) && ok;

	// Written once: whoever edits it keeps their edit.
	snprintf(sub, sizeof(sub), "%s/README.txt", dir);
	FILE *f = fopen(sub, "wx");
	if (f) {
		fputs(README, f);
		fclose(f);
	}
	return ok;
}

const char *terminal_tools_path(const char *sd_root) {
	static char path[512];
	path[0] = '\0';
	if (terminal_tools_enabled() && sd_root && sd_root[0]) {
		snprintf(path, sizeof(path), "%s/" TERMINAL_FOLDER, sd_root);
	}
	return path;
}

// ---------------------------------------------------------------------------
// The shell
// ---------------------------------------------------------------------------

#define DEFAULT_PATH "/bin:/sbin:/usr/bin:/usr/sbin"
// A second Control-C inside this long means the first did nothing: the program
// ignores it, and the shell is taken down with it.
#define INTERRUPT_AGAIN_MS 1500

static pid_t shell_pid = -1;
static int shell_in = -1;
static int shell_master = -1; // the terminal the output comes back through
static unsigned generation;	  // bumped when a shell is ended on purpose
static uint32_t last_interrupt_ms;
static int term_columns = 40;
static int term_rows = 12;

typedef struct {
	int fd;
	pid_t pid;
	unsigned gen;
} reader_arg_t;

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

// With `lock` held.
static void apply_size(int master) {
	struct winsize ws;
	memset(&ws, 0, sizeof(ws));
	ws.ws_col = (unsigned short)term_columns;
	ws.ws_row = (unsigned short)term_rows;
	ioctl(master, TIOCSWINSZ, &ws);
}

void terminal_set_size(int columns, int rows) {
	if (columns < 10) {
		columns = 10;
	}
	if (columns > MAX_COLS) {
		columns = MAX_COLS;
	}
	if (rows < 4) {
		rows = 4;
	}
	pthread_mutex_lock(&lock);
	bool changed = columns != term_columns || rows != term_rows;
	term_columns = columns;
	term_rows = rows;
	if (changed && shell_master >= 0) {
		apply_size(shell_master);
	}
	pthread_mutex_unlock(&lock);
}

static void *reader_main(void *arg) {
	reader_arg_t a = *(reader_arg_t *)arg;
	free(arg);

	parser_t parser;
	memset(&parser, 0, sizeof(parser));
	char buf[1024];
	for (;;) {
		ssize_t n = read(a.fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break; // EIO is what a terminal reads once nothing holds its other end
		}
		pthread_mutex_lock(&lock);
		for (ssize_t i = 0; i < n; i++) {
			feed(&parser, (unsigned char)buf[i]);
		}
		serial++;
		pthread_mutex_unlock(&lock);
	}
	waitpid(a.pid, NULL, 0);

	pthread_mutex_lock(&lock);
	if (shell_master == a.fd) {
		shell_master = -1;
	}
	close(a.fd);
	if (a.gen == generation && shell_pid == a.pid) {
		if (shell_in >= 0) {
			close(shell_in);
			shell_in = -1;
		}
		shell_pid = -1;
		write_note("[shell exited]\n");
	}
	pthread_mutex_unlock(&lock);
	return NULL;
}

// The player's environment with the shell's own few things on top.
static char **build_env(const char *tools) {
	size_t count = 0;
	while (environ && environ[count]) {
		count++;
	}
	char **env = calloc(count + 8, sizeof(char *));
	if (!env) {
		return NULL;
	}

	size_t n = 0;
	for (size_t i = 0; i < count; i++) {
		const char *e = environ[i];
		if (!strncmp(e, "PATH=", 5) || !strncmp(e, "HOME=", 5) || !strncmp(e, "TERM=", 5) ||
			!strncmp(e, "LD_LIBRARY_PATH=", 16) || !strncmp(e, "PS1=", 4) || !strncmp(e, "COLORTERM=", 10)) {
			continue;
		}
		env[n++] = strdup(e);
	}

	char line[1100];
	const char *inherited_path = getenv("PATH");
	if (!inherited_path || !inherited_path[0]) {
		inherited_path = DEFAULT_PATH;
	}
	if (tools[0]) {
		snprintf(line, sizeof(line), "PATH=%s/bin:%s", tools, inherited_path);
		env[n++] = strdup(line);
		const char *ld = getenv("LD_LIBRARY_PATH");
		if (ld && ld[0]) {
			snprintf(line, sizeof(line), "LD_LIBRARY_PATH=%s/lib:%s", tools, ld);
		} else {
			snprintf(line, sizeof(line), "LD_LIBRARY_PATH=%s/lib", tools);
		}
		env[n++] = strdup(line);
		snprintf(line, sizeof(line), "HOME=%s", tools);
		env[n++] = strdup(line);
	} else {
		snprintf(line, sizeof(line), "PATH=%s", inherited_path);
		env[n++] = strdup(line);
		env[n++] = strdup("HOME=/tmp");
	}
	env[n++] = strdup("TERM=xterm-256color");
	env[n] = NULL;
	return env;
}

static void free_env(char **env) {
	for (size_t i = 0; env && env[i]; i++) {
		free(env[i]);
	}
	free(env);
}

// A pseudo-terminal for the shell's output. The slave is returned for the shell
// to be given as its stdout and stderr; the master is what the page reads.
static bool open_terminal(int *master, int *slave) {
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0) {
		return false;
	}
	char name[64];
	if (grantpt(m) != 0 || unlockpt(m) != 0 || ptsname_r(m, name, sizeof(name)) != 0) {
		close(m);
		return false;
	}
	int s = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (s < 0) {
		close(m);
		return false;
	}
	fcntl(m, F_SETFD, FD_CLOEXEC);

	// Nothing is typed at it, so there is nothing to echo or line-edit; output
	// keeps its newline to carriage-return-and-newline translation, which the
	// grid takes in stride.
	struct termios tio;
	if (tcgetattr(s, &tio) == 0) {
		tio.c_lflag &= ~(tcflag_t)(ECHO | ICANON);
		tcsetattr(s, TCSANOW, &tio);
	}
	*master = m;
	*slave = s;
	return true;
}

// With `lock` held.
static bool spawn_shell(const char *sd_root) {
	const char *tools = terminal_tools_path(sd_root);
	if (tools[0]) {
		terminal_tools_prepare(sd_root);
	}

	// Where it starts: the folder, else the card, else somewhere that is there.
	// PATH names the folder whether or not the card is in -- a program is looked
	// up when it is run, so a card put in later is found by the shell already
	// open -- but the shell itself starts on the card only if there is one: its
	// mount point without a card is a directory of the player's own flash.
	char start[512];
	struct stat st;
	if (!storage_card_attached()) {
		snprintf(start, sizeof(start), "/tmp");
	} else if (tools[0] && stat(tools, &st) == 0) {
		snprintf(start, sizeof(start), "%s", tools);
	} else if (sd_root && sd_root[0] && stat(sd_root, &st) == 0) {
		snprintf(start, sizeof(start), "%s", sd_root);
	} else {
		snprintf(start, sizeof(start), "/tmp");
	}

	int in_pipe[2];
	if (pipe2(in_pipe, O_CLOEXEC) != 0) {
		return false;
	}
	int master;
	int slave;
	if (!open_terminal(&master, &slave)) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		write_note("[could not open a terminal for the shell]\n");
		return false;
	}
	apply_size(master);

	char **env = build_env(tools);
	if (!env) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(master);
		close(slave);
		return false;
	}

	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attr;
	posix_spawn_file_actions_init(&actions);
	posix_spawnattr_init(&attr);

	posix_spawn_file_actions_adddup2(&actions, in_pipe[0], 0);
	posix_spawn_file_actions_adddup2(&actions, slave, 1);
	posix_spawn_file_actions_adddup2(&actions, slave, 2);

	// Whatever else the player has open (the audio device, the database, the
	// card's files) is not the shell's to hold on to.
	long max_fd = sysconf(_SC_OPEN_MAX);
	if (max_fd < 0 || max_fd > 1024) {
		max_fd = 1024;
	}
	for (int fd = 3; fd < max_fd; fd++) {
		if (fd == in_pipe[0] || fd == in_pipe[1] || fd == master || fd == slave) {
			continue;
		}
		if (fcntl(fd, F_GETFD) >= 0) {
			posix_spawn_file_actions_addclose(&actions, fd);
		}
	}

	// A group of its own, so Control-C reaches the shell and what it started
	// and nothing of the player's; the signal state of a fresh program.
	sigset_t none;
	sigset_t defaults;
	sigemptyset(&none);
	sigemptyset(&defaults);
	sigaddset(&defaults, SIGPIPE);
	sigaddset(&defaults, SIGINT);
	sigaddset(&defaults, SIGTERM);
	sigaddset(&defaults, SIGQUIT);
	sigaddset(&defaults, SIGHUP);
	sigaddset(&defaults, SIGUSR1);
	sigaddset(&defaults, SIGUSR2);
	sigaddset(&defaults, SIGALRM);
	posix_spawnattr_setsigmask(&attr, &none);
	posix_spawnattr_setsigdefault(&attr, &defaults);
	posix_spawnattr_setpgroup(&attr, 0);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);

	char *argv[] = {"sh", "-c", "cd \"$1\" 2>/dev/null; exec /bin/sh", "sh", start, NULL};
	pid_t pid;
	int rc = posix_spawn(&pid, "/bin/sh", &actions, &attr, argv, env);

	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attr);
	free_env(env);
	close(in_pipe[0]);
	close(slave);

	if (rc != 0) {
		close(in_pipe[1]);
		close(master);
		write_local_fmt("[could not start the shell: %s]\n", strerror(rc));
		return false;
	}

	// The shell must outlive Control-C: with this trap the signal ends what is
	// running and the shell, which gets it too, only goes on to the next line.
	// ls draws in colour, which BusyBox does only when asked.
	const char init[] = "trap : INT; alias ls='ls --color=auto'\n";
	if (write(in_pipe[1], init, sizeof(init) - 1) < 0) {
		// Nothing useful to do: the shell will say so itself.
	}
	fcntl(in_pipe[1], F_SETFL, fcntl(in_pipe[1], F_GETFL) | O_NONBLOCK);

	reader_arg_t *arg = malloc(sizeof(*arg));
	pthread_t thread;
	bool reading = false;
	if (arg) {
		arg->fd = master;
		arg->pid = pid;
		arg->gen = generation;
		reading = pthread_create(&thread, NULL, reader_main, arg) == 0;
	}
	if (!reading) {
		kill(-pid, SIGKILL);
		close(in_pipe[1]);
		close(master);
		waitpid(pid, NULL, 0);
		free(arg);
		return false;
	}
	pthread_detach(thread);

	shell_pid = pid;
	shell_in = in_pipe[1];
	shell_master = master;

	if (line_count == 1 && line_at(0)->length == 0) {
		write_note("Sonix terminal\n");
		if (tools[0]) {
			write_local_fmt("Programs in %s/bin run by name.\n", tools);
		} else {
			write_local("Turn on Developer options > Terminal binaries to add your own programs.\n");
		}
		write_local("\n");
	} else {
		write_note("[new shell]\n");
	}
	return true;
}

static void end_shell_locked(void) {
	if (shell_pid > 0) {
		generation++; // the reader that is about to see the end of it stays quiet
		kill(-shell_pid, SIGKILL);
		shell_pid = -1;
	}
	if (shell_in >= 0) {
		close(shell_in);
		shell_in = -1;
	}
}

bool terminal_open(const char *sd_root) {
	pthread_mutex_lock(&lock);
	bool ok = shell_pid > 0 || spawn_shell(sd_root);
	pthread_mutex_unlock(&lock);
	return ok;
}

static bool write_all_locked(const char *s, size_t n) {
	while (n > 0) {
		ssize_t w = write(shell_in, s, n);
		if (w < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false; // EPIPE: the shell is gone; EAGAIN: it is not listening
		}
		s += w;
		n -= (size_t)w;
	}
	return true;
}

void terminal_run(const char *line, const char *sd_root) {
	char cmd[1024];
	size_t n = 0;
	for (; line && line[n] && line[n] != '\n' && line[n] != '\r' && n < sizeof(cmd) - 2; n++) {
		cmd[n] = line[n];
	}
	cmd[n] = '\0';

	pthread_mutex_lock(&lock);

	if (!strcmp(cmd, "clear")) {
		grid_reset();
		pthread_mutex_unlock(&lock);
		return;
	}

	// A new shell announces itself above the line that needed it.
	bool started = shell_pid > 0 || spawn_shell(sd_root);

	write_note("$ ");
	// What was typed is shown as it is: a control character in it is not an
	// instruction to the screen.
	for (size_t i = 0; cmd[i]; i++) {
		if ((unsigned char)cmd[i] >= 0x20 && cmd[i] != 0x7f) {
			feed(&local_parser, (unsigned char)cmd[i]);
		}
	}
	local_parser.state = P_TEXT;
	write_local("\n");

	if (!started) {
		pthread_mutex_unlock(&lock);
		return;
	}

	cmd[n++] = '\n';
	if (!write_all_locked(cmd, n)) {
		// A shell that has exited while something it started held the output
		// open is the usual reason: begin another and try once more.
		end_shell_locked();
		if (spawn_shell(sd_root) && !write_all_locked(cmd, n)) {
			write_note("[the shell is not taking input]\n");
		}
	}
	pthread_mutex_unlock(&lock);
}

void terminal_interrupt(void) {
	pthread_mutex_lock(&lock);
	if (shell_pid > 0) {
		uint32_t now = now_ms();
		bool again = last_interrupt_ms && now - last_interrupt_ms < INTERRUPT_AGAIN_MS;
		last_interrupt_ms = now;
		if (again) {
			end_shell_locked();
			write_note("^C\n[shell stopped]\n");
			last_interrupt_ms = 0;
		} else {
			kill(-shell_pid, SIGINT);
			write_note("^C\n");
		}
	}
	pthread_mutex_unlock(&lock);
}

void terminal_clear(void) {
	pthread_mutex_lock(&lock);
	grid_reset();
	pthread_mutex_unlock(&lock);
}

void terminal_close(void) {
	pthread_mutex_lock(&lock);
	end_shell_locked();
	pthread_mutex_unlock(&lock);
}

bool terminal_running(void) {
	pthread_mutex_lock(&lock);
	bool running = shell_pid > 0;
	pthread_mutex_unlock(&lock);
	return running;
}

uint32_t terminal_serial(void) {
	pthread_mutex_lock(&lock);
	uint32_t s = serial;
	pthread_mutex_unlock(&lock);
	return s;
}

void terminal_tools_set_enabled(bool on, const char *sd_root) {
	config_set_bool(CFG_SECTION, CFG_TOOLS, on);
	config_save();
	if (on) {
		terminal_tools_prepare(sd_root);
	}
	// The PATH is fixed when a shell starts, so the next line gets a new one.
	terminal_close();
}
