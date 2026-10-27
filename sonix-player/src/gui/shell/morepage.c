#include "morepage.h"

#include "lvgl/lvgl.h"

#include "src/gui/apps/appfolders.h"
#include "src/gui/apps/rompage.h"
#include "src/gui/audio/dacpage.h"
#include "src/gui/audio/metronomepage.h"
#include "src/gui/calculator/calculatorpage.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/games/breakout.h"
#include "src/gui/games/game2048.h"
#include "src/gui/games/snake.h"
#include "src/gui/library/filespage.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/terminal/terminalpage.h"

lv_obj_t *morepage_screen;
lv_obj_t *gamespage_screen;
lv_obj_t *emulatorspage_screen;

// What the folders hold of their own. The tiles of apps from the card follow
// them (gui/apps/appfolders.c).
static const grid_entry_t games_entries[] = {
	// An action: the artwork is read from the resource tree on the way in.
	{"flappy_bird", &icon_menu_flappy_bird, NULL, flappybird_open},
	{"snake", &icon_menu_snake, &snake_screen, NULL},
	{"game2048", &icon_menu_game2048, &game2048_screen, NULL},
	{"breakout", &icon_menu_breakout, &breakout_screen, NULL},
};

static const grid_entry_t emulators_entries[] = {
#if GB_CORE
	// The Game Boy emulator. Present only when the binary was built with GB=1
	// (see the Makefile).
	{"gearboy", &icon_menu_gearboy, &gearboypage_screen, NULL},
#endif
};

static const grid_entry_t more_entries[] = {
	{"games", &icon_menu_games, &gamespage_screen, NULL},
	{"emulators", &icon_menu_emulators, &emulatorspage_screen, NULL},
	{"dac", &icon_menu_dac, &dacpage_screen, NULL},
	// An action rather than a screen: the shelf reads the Ebook folder again
	// every time it is opened, so a book copied onto the card turns up without a
	// restart.
	{"books", &icon_menu_books, NULL, ebookpage_open},
	// An action for the same reason the shelf is one: the listing is read when
	// the page is entered, so a card that changed under the player shows what is
	// on it now.
	{"file_explorer", &icon_menu_file_explorer, NULL, filespage_open},
	{"metronome", &icon_menu_metronome, &metronomepage_screen, NULL},
	{"calculator", &icon_menu_calculator, &calculatorpage_screen, NULL},
	// A shell. Programs for it go on the card, see system/terminal/terminal.h.
	{"terminal", &icon_menu_terminal, &terminalpage_screen, NULL},
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

void morepage_init(gui_config_t *cfg) {
	// All three are filled at run time as well, from the Apps folder of the card.
	appfolder_create(gamespage_screen, cfg, "games", APP_FOLDER_GAMES, games_entries, COUNT(games_entries));
	appfolder_create(emulatorspage_screen, cfg, "emulators", APP_FOLDER_EMULATORS, emulators_entries,
					 COUNT(emulators_entries));
	appfolder_create(morepage_screen, cfg, "more", APP_FOLDER_MORE, more_entries, COUNT(more_entries));
}
