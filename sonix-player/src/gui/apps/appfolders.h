#ifndef GUI_APPFOLDERS_H
#define GUI_APPFOLDERS_H

#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/gui.h"
#include "src/system/apps/apps.h"

#include "lvgl/lvgl.h"

// A page of tiles that is part built in and part from the card: More, and the
// Games and Emulators folders inside it.
//
// The built-in tiles are given when the page is made. The rest are the apps of
// the matching folder (system/apps), read again every time the page is opened,
// so an app copied onto the card shows up without a restart. Tiles run on in the
// same grid, two across, scrolling when there are more than six.

typedef struct appfolder appfolder_t;

// Builds the page on `screen`, titled `title` (a language tag). `builtin` and its
// `count` entries must stay valid as long as the page does.
appfolder_t *appfolder_create(lv_obj_t *screen, gui_config_t *cfg, const char *title, app_folder_t folder,
							  const grid_entry_t *builtin, int count);

#endif /* GUI_APPFOLDERS_H */
