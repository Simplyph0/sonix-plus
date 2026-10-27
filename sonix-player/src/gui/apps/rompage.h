#ifndef GUI_ROMPAGE_H
#define GUI_ROMPAGE_H

#include "src/gui/shell/gui.h"
#include "src/system/apps/apps.h"

#include "lvgl/lvgl.h"

// The list of files for an app that opens one (an emulator): everything in the
// app's rom_dir on the card, in subfolders too, that has one of its extensions.
// Tapping a file starts the app with it. The folder is made when it is not
// there, so whoever opens the page for the first time can see where the files go.
extern lv_obj_t *rompage_screen;

void rompage_init(gui_config_t *cfg);

// Lists the files for `app` and opens the page.
void rompage_open(const app_t *app);

#endif /* GUI_ROMPAGE_H */
