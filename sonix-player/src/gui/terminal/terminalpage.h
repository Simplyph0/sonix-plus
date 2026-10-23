#ifndef GUI_TERMINALPAGE_H
#define GUI_TERMINALPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Terminal tile in More: a shell on the player, typed into with the same
// on-screen keyboard as everywhere else (so the layout chosen in Settings is
// the one used here) and left by the back arrow like every other page.
//
// The output is read-only text above the input line. Tapping it puts the
// keyboard away for more room; tapping the line brings it back. The ^C key
// next to the line interrupts what is running -- twice in a row ends the shell.
//
// What the shell can run, and where more programs go, is described in
// system/terminal/terminal.h. A typeface of the user's own for the page can be
// put on the card as Terminal/font.ttf.
extern lv_obj_t *terminalpage_screen;

void terminalpage_init(gui_config_t *cfg);

#endif /* GUI_TERMINALPAGE_H */
