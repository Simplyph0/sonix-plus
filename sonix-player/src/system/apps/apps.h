#ifndef APPS_H
#define APPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Apps that live on the card, not in the firmware.
//
// The root filesystem is read-only and has about two megabytes to spare, so
// anything big -- an emulator, say -- cannot go in it. Instead the player looks
// in a folder of the card:
//
//     <card>/Apps/<name>/manifest.ini     what the app is called, where it goes
//     <card>/Apps/<name>/icon.png         the tile, 112x112 (optional)
//     <card>/Apps/<name>/<program>        a MIPS program, started when it is tapped
//
// Each app brings its own folder, so installing one is copying a folder and
// removing it is deleting one. The folders Games and Emulators in More, and
// More itself, are filled from the manifests every time they are opened.
//
// An app is a separate program, not a plug-in: the player starts it as a child
// process, hands it the screen, the touch panel and the sound, and takes them
// back when it exits. Nothing is linked into the player, so an app can be under
// any licence. How an app uses the screen, touch and sound is in APPS.md.
//
// manifest.ini, one section:
//
//     [app]
//     name = NES                  shown under the tile (required)
//     exec = nes                  the program, inside the app's folder (required)
//     folder = Emulators          Games, Emulators or More (default More)
//     icon = icon.png             a PNG, scaled to 112x112 if it is another size
//     version = 1.0
//     author = Somebody
//     description = A line about it
//     rom_dir = Games/NES         for an emulator: the folder of the card to list...
//     rom_ext = nes,zip           ...and the extensions that count, without the dot
//
// With rom_dir the player shows the files to choose from and starts the program
// with the chosen file as its only argument; without it the program starts as it is.

#define APPS_FOLDER "Apps"
#define APPS_MAX 64

typedef enum {
	APP_FOLDER_GAMES,
	APP_FOLDER_EMULATORS,
	APP_FOLDER_MORE,
} app_folder_t;

typedef struct {
	char id[48]; // the app's folder name
	char name[64];
	app_folder_t folder;
	char dir[512];	// absolute path of the app's folder
	char exec[640]; // absolute path of the program
	char version[24];
	char author[48];
	char description[160];
	char rom_dir[128]; // "" for an app that is not given a file
	char rom_exts[64];
	uint8_t *icon; // 112x112 ARGB8888 (as LVGL wants it), or NULL
	int icon_w;
	int icon_h;
} app_t;

#define APPS_ICON_SIZE 112

// Makes <card>/Apps and a README in it when there is a writable card. Safe to
// call every time: the card may not have been in before.
bool apps_prepare(const char *sd_root);

// Reads every manifest again, replacing what was known; the app_t pointers of
// the earlier scan stop being valid. Returns how many apps there are.
int apps_scan(const char *sd_root);

int apps_count(void);
const app_t *apps_at(int index);

// The apps that go in `folder`, in name order. Returns how many were written.
int apps_in_folder(app_folder_t folder, const app_t **out, int max);

// Whether a file name has one of the app's rom_ext extensions ("nes" against
// "nes,zip"), ignoring case. True for any name when the app lists none.
bool apps_rom_matches(const app_t *app, const char *file_name);

#endif /* APPS_H */
