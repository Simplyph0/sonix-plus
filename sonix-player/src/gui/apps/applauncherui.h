#ifndef GUI_APPLAUNCHERUI_H
#define GUI_APPLAUNCHERUI_H

#include "src/gui/shell/gui.h"
#include "src/system/apps/apps.h"

// Starting an app from a tile or from the list of files, and settling the
// interface when it comes back.
//
// The app ran with the touch panel to itself, so LVGL's own reader has a queue
// of touches it never saw, and the last finger of the app -- the tap on its exit
// key -- is still on its way. Given back at once they would land on whatever the
// player shows underneath. So for a moment after the app ends a transparent
// layer on top of everything takes them.

void applauncherui_init(gui_config_t *cfg);

// Starts `app`, opening `rom` (a path, or NULL). Tells the user when it cannot.
void applauncherui_start(const app_t *app, const char *rom);

#endif /* GUI_APPLAUNCHERUI_H */
