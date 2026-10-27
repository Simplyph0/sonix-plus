#include "apps.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/device/system.h"
#include "src/system/image/stb_image_decl.h"

static app_t apps[APPS_MAX];
static int app_count;

static const char README[] =
	"Apps folder\n"
	"===========\n"
	"\n"
	"Each app is a folder in here:\n"
	"\n"
	"  Apps/<name>/manifest.ini   its name, where it goes, what it opens\n"
	"  Apps/<name>/icon.png       the tile, 112x112\n"
	"  Apps/<name>/<program>      a program built for this player\n"
	"\n"
	"The player lists them in More > Games, More > Emulators and More. To install\n"
	"an app copy its folder here; to remove it delete the folder.\n"
	"\n"
	"The format, and how to write an app, are in APPS.md in the Sonix Plus repository.\n"
	"An app is a program that runs as root with the whole player at its disposal,\n"
	"so only install ones you trust.\n";

bool apps_prepare(const char *sd_root) {
	if (!sd_root || !sd_root[0] || !storage_card_attached() || !storage_sd_writable()) {
		return false;
	}
	char dir[600];
	if (snprintf(dir, sizeof(dir), "%s/" APPS_FOLDER, sd_root) >= (int)sizeof(dir)) {
		return false;
	}
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		return false;
	}
	char readme[640];
	snprintf(readme, sizeof(readme), "%s/README.txt", dir);
	FILE *f = fopen(readme, "wx"); // written once: an edit of it is kept
	if (f) {
		fputs(README, f);
		fclose(f);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Reading a manifest
// ---------------------------------------------------------------------------

static void trim(char *s) {
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) {
		s[--n] = '\0';
	}
	char *p = s;
	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	if (p != s) {
		memmove(s, p, strlen(p) + 1);
	}
}

static void copy_value(char *dst, size_t size, const char *value) { snprintf(dst, size, "%s", value); }

// A path inside the app's folder or the card, never above or outside it.
static bool safe_relative(const char *path) { return path[0] && path[0] != '/' && !strstr(path, ".."); }

typedef struct {
	char icon_name[128];
	char exec_name[128];
} manifest_names_t;

static bool parse_manifest(const char *path, app_t *app, manifest_names_t *names) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	app->folder = APP_FOLDER_MORE;
	bool in_app = false;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		if (!line[0] || line[0] == ';' || line[0] == '#') {
			continue;
		}
		if (line[0] == '[') {
			in_app = strncasecmp(line, "[app]", 5) == 0;
			continue;
		}
		if (!in_app) {
			continue;
		}
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *key = line;
		char *value = eq + 1;
		trim(key);
		trim(value);

		if (!strcasecmp(key, "name")) {
			copy_value(app->name, sizeof(app->name), value);
		} else if (!strcasecmp(key, "exec")) {
			copy_value(names->exec_name, sizeof(names->exec_name), value);
		} else if (!strcasecmp(key, "icon")) {
			copy_value(names->icon_name, sizeof(names->icon_name), value);
		} else if (!strcasecmp(key, "folder")) {
			if (!strcasecmp(value, "games")) {
				app->folder = APP_FOLDER_GAMES;
			} else if (!strcasecmp(value, "emulators")) {
				app->folder = APP_FOLDER_EMULATORS;
			} else {
				app->folder = APP_FOLDER_MORE;
			}
		} else if (!strcasecmp(key, "version")) {
			copy_value(app->version, sizeof(app->version), value);
		} else if (!strcasecmp(key, "author")) {
			copy_value(app->author, sizeof(app->author), value);
		} else if (!strcasecmp(key, "description")) {
			copy_value(app->description, sizeof(app->description), value);
		} else if (!strcasecmp(key, "rom_dir")) {
			copy_value(app->rom_dir, sizeof(app->rom_dir), value);
		} else if (!strcasecmp(key, "rom_ext")) {
			copy_value(app->rom_exts, sizeof(app->rom_exts), value);
		}
	}
	fclose(f);
	return true;
}

// ---------------------------------------------------------------------------
// The icon
// ---------------------------------------------------------------------------

static uint8_t *read_file(const char *path, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 4 * 1024 * 1024) {
		fclose(f);
		return NULL;
	}
	uint8_t *data = malloc((size_t)n);
	if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
		free(data);
		data = NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return data;
}

// A PNG as the tile wants it: APPS_ICON_SIZE square, blue-green-red-alpha, which
// is LVGL's ARGB8888 on this little-endian CPU. Another size is scaled to fit,
// keeping its proportions and centred on transparent.
static uint8_t *load_icon(const char *path) {
	size_t size;
	uint8_t *file = read_file(path, &size);
	if (!file) {
		return NULL;
	}
	int w;
	int h;
	int channels;
	uint8_t *rgba = stbi_load_from_memory(file, (int)size, &w, &h, &channels, 4);
	free(file);
	if (!rgba || w <= 0 || h <= 0) {
		if (rgba) {
			stbi_image_free(rgba);
		}
		return NULL;
	}

	uint8_t *out = calloc((size_t)APPS_ICON_SIZE * APPS_ICON_SIZE, 4);
	if (!out) {
		stbi_image_free(rgba);
		return NULL;
	}
	int fit = w > h ? w : h;
	int dw = w * APPS_ICON_SIZE / fit;
	int dh = h * APPS_ICON_SIZE / fit;
	int ox = (APPS_ICON_SIZE - dw) / 2;
	int oy = (APPS_ICON_SIZE - dh) / 2;
	for (int y = 0; y < dh; y++) {
		int sy = y * h / dh;
		for (int x = 0; x < dw; x++) {
			int sx = x * w / dw;
			const uint8_t *s = rgba + ((size_t)sy * w + sx) * 4;
			uint8_t *d = out + ((size_t)(oy + y) * APPS_ICON_SIZE + ox + x) * 4;
			d[0] = s[2];
			d[1] = s[1];
			d[2] = s[0];
			d[3] = s[3];
		}
	}
	stbi_image_free(rgba);
	return out;
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

static void free_all(void) {
	for (int i = 0; i < app_count; i++) {
		free(apps[i].icon);
	}
	memset(apps, 0, sizeof(apps));
	app_count = 0;
}

static int by_name(const void *a, const void *b) { return strcasecmp(((const app_t *)a)->name, ((const app_t *)b)->name); }

int apps_scan(const char *sd_root) {
	free_all();
	if (!sd_root || !sd_root[0] || !storage_card_attached()) {
		return 0;
	}
	char root[600];
	snprintf(root, sizeof(root), "%s/" APPS_FOLDER, sd_root);
	DIR *dir = opendir(root);
	if (!dir) {
		return 0;
	}
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL && app_count < APPS_MAX) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		char app_dir[640];
		if (snprintf(app_dir, sizeof(app_dir), "%s/%s", root, entry->d_name) >= (int)sizeof(app_dir)) {
			continue;
		}
		struct stat st;
		if (stat(app_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}
		char manifest[700];
		snprintf(manifest, sizeof(manifest), "%s/manifest.ini", app_dir);

		app_t *app = &apps[app_count];
		memset(app, 0, sizeof(*app));
		manifest_names_t names;
		memset(&names, 0, sizeof(names));
		snprintf(app->id, sizeof(app->id), "%s", entry->d_name);
		snprintf(app->dir, sizeof(app->dir), "%s", app_dir);
		if (!parse_manifest(manifest, app, &names)) {
			continue;
		}
		if (!app->name[0] || !safe_relative(names.exec_name)) {
			continue;
		}
		if (snprintf(app->exec, sizeof(app->exec), "%s/%s", app_dir, names.exec_name) >= (int)sizeof(app->exec) ||
			stat(app->exec, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue; // no program, nothing to start
		}
		if (app->rom_dir[0] && !safe_relative(app->rom_dir)) {
			app->rom_dir[0] = '\0';
		}
		if (names.icon_name[0] && safe_relative(names.icon_name)) {
			char icon_path[780];
			snprintf(icon_path, sizeof(icon_path), "%s/%s", app_dir, names.icon_name);
			app->icon = load_icon(icon_path);
			if (app->icon) {
				app->icon_w = APPS_ICON_SIZE;
				app->icon_h = APPS_ICON_SIZE;
			}
		}
		app_count++;
	}
	closedir(dir);
	qsort(apps, (size_t)app_count, sizeof(apps[0]), by_name);
	return app_count;
}

int apps_count(void) { return app_count; }

const app_t *apps_at(int index) { return index >= 0 && index < app_count ? &apps[index] : NULL; }

int apps_in_folder(app_folder_t folder, const app_t **out, int max) {
	int n = 0;
	for (int i = 0; i < app_count && n < max; i++) {
		if (apps[i].folder == folder) {
			out[n++] = &apps[i];
		}
	}
	return n;
}

bool apps_rom_matches(const app_t *app, const char *file_name) {
	if (!app || !app->rom_exts[0]) {
		return true;
	}
	const char *dot = strrchr(file_name, '.');
	if (!dot || !dot[1]) {
		return false;
	}
	dot++;
	char list[sizeof(app->rom_exts)];
	snprintf(list, sizeof(list), "%s", app->rom_exts);
	for (char *tok = strtok(list, ", "); tok; tok = strtok(NULL, ", ")) {
		if (!strcasecmp(tok, dot)) {
			return true;
		}
	}
	return false;
}
