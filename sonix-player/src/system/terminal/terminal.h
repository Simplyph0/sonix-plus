#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The shell behind the Terminal page, and the folder on the card that stands in
// for the read-only root filesystem.
//
// The root filesystem cannot take new programs, so with the developer option
// "Terminal binaries" on the player keeps a folder on the microSD:
//
//     <card>/Terminal/bin     programs; first on the shell's PATH
//     <card>/Terminal/lib     shared libraries those programs need
//     <card>/Terminal/font.ttf (optional) a typeface for the page, in place of the
//                              built-in DejaVu Sans Mono
//
// The card is FAT, but it is mounted with the execute bits set and without
// noexec, so a MIPS binary copied into bin runs as it is. It has to be built
// for this device (32-bit little-endian MIPS, glibc 2.22 or static).
//
// The shell is one /bin/sh kept for as long as the player runs. Lines go in
// through a pipe, so it never prompts and nothing is echoed; what it and its
// programs print comes back through a pseudo-terminal, which is what makes a
// program believe it is on a screen: it draws in colour, sizes itself to the
// width it is told, and moves the cursor to lay out columns (fastfetch's logo
// and its lines are drawn that way). The output is kept as a grid of cells that
// those sequences are applied to. A program that wants the whole screen and
// reads single keys (vi, top) still will not work: nothing is sent back to it.

// The folder under the card, "Terminal". Used for the settings row's subtitle.
#define TERMINAL_FOLDER "Terminal"

bool terminal_tools_enabled(void);

// Turns the folder on or off, remembered in the config. Turning it on creates
// it (see terminal_tools_prepare); turning it off leaves it, and what the user
// put in it, alone -- it only stops being on the PATH. Takes effect for a shell
// started afterwards, so a running one is ended and comes back with the new
// PATH on the next line typed.
void terminal_tools_set_enabled(bool on, const char *sd_root);

// Makes the folder and its bin and lib, and a README saying what they are for,
// if the option is on and a card is in. Safe to call as often as you like; the
// card may not have been in at the time the option was switched. Returns false
// when the option is off, there is no writable card, or the folder could not be
// made.
bool terminal_tools_prepare(const char *sd_root);

// "<card>/Terminal" while the option is on, else "". Valid until the next call.
const char *terminal_tools_path(const char *sd_root);

// Starts the shell if none is running, and puts a short greeting in the
// output. False when it could not be started.
bool terminal_open(const char *sd_root);

// Runs one line: echoes "$ line" into the output and hands it to the shell,
// starting one first if the last has exited. "clear" empties the output
// instead.
void terminal_run(const char *line, const char *sd_root);

// Tab completion: completes the last word of `line` as a program (the first
// word, no slash) or as a path from where the shell is. On a unique match, or a
// longer start the matches share, the whole completed line goes in `out` and
// true is returned; when several remain they are listed in the output.
bool terminal_complete(const char *line, const char *sd_root, char *out, size_t size);

// Control-C: interrupts what the shell is running, and leaves the shell be.
void terminal_interrupt(void);

// Empties the output.
void terminal_clear(void);

// Ends the shell and everything it started.
void terminal_close(void);

bool terminal_running(void);

// How many characters wide and lines tall the page shows, told to programs as
// the size of their terminal. Applies to a shell already running too.
void terminal_set_size(int columns, int rows);

// Changes whenever the output does, so a page can ask cheaply whether to
// redraw.
uint32_t terminal_serial(void);

// The colour of a stretch of text, as a palette index of the usual 256 colours
// (0 to 15 the sixteen named ones, 16 to 231 the colour cube, 232 to 255 the
// greys), or TERMINAL_DEFAULT_COLOR for the page's own text colour.
#define TERMINAL_DEFAULT_COLOR 0xFF

typedef struct {
	uint16_t color;
	uint16_t length; // bytes of the text this colour covers
} terminal_run_t;

// Copies the end of the output: as much as fits in `size` bytes, always from the
// start of a line, NUL terminated, lines separated by '\n'. `runs` says how the
// text is coloured, in order, and together they cover all of it; `max_runs`
// bounds them, and past it the rest is one run in the last colour. A space
// that has a background colour comes out as a full block in that colour, since
// the page cannot paint a background behind text. Returns the length of the
// text.
size_t terminal_snapshot(char *out, size_t size, terminal_run_t *runs, size_t max_runs, size_t *run_count);

#endif /* TERMINAL_H */
