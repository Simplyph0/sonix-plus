#include "highscore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/system/core/config.h"
#include "src/system/device/system.h"

#define MAX_ENTRIES 64
#define NAME_MAX_LEN 40

typedef struct {
	char name[NAME_MAX_LEN];
	long value;
} entry_t;

static const char *const *root_ptr;
static entry_t entries[MAX_ENTRIES];
static int entry_count;
static bool loaded;
static bool loaded_from_card;

static const char *root_of(void) { return root_ptr && *root_ptr ? *root_ptr : ""; }

static void path_of(char *out, size_t size, const char *file) {
	snprintf(out, size, "%s/Saves/Games/%s", root_of(), file);
}

static bool card_usable(void) { return root_of()[0] && storage_card_attached() && storage_sd_writable(); }

static entry_t *find(const char *name) {
	for (int i = 0; i < entry_count; i++) {
		if (strcmp(entries[i].name, name) == 0) {
			return &entries[i];
		}
	}
	return NULL;
}

static void load(void) {
	entry_count = 0;
	loaded = true;
	loaded_from_card = false;
	if (!root_of()[0] || !storage_card_attached()) {
		return;
	}
	char path[640];
	path_of(path, sizeof(path), "scores.ini");
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	loaded_from_card = true;
	char line[128];
	while (fgets(line, sizeof(line), f) && entry_count < MAX_ENTRIES) {
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#' || line[0] == ';') {
			continue;
		}
		*eq = '\0';
		size_t len = strlen(line);
		if (len >= NAME_MAX_LEN) {
			continue;
		}
		memcpy(entries[entry_count].name, line, len + 1);
		entries[entry_count].value = atol(eq + 1);
		entry_count++;
	}
	fclose(f);
}

static void save(void) {
	if (!card_usable()) {
		return;
	}
	char dir[640];
	snprintf(dir, sizeof(dir), "%s/Saves", root_of());
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/Saves/Games", root_of());
	mkdir(dir, 0755);

	char path[640];
	char tmp[660];
	path_of(path, sizeof(path), "scores.ini");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	fputs("# Best scores of the games in More > Games. Delete a line to reset it.\n", f);
	for (int i = 0; i < entry_count; i++) {
		fprintf(f, "%s=%ld\n", entries[i].name, entries[i].value);
	}
	bool ok = fflush(f) == 0 && ferror(f) == 0;
	fclose(f);
	if (ok) {
		rename(tmp, path);
	} else {
		remove(tmp);
	}
}

void highscore_init(const char *const *sd_root) {
	root_ptr = sd_root;
	loaded = false;
}

long highscore_get(const char *name) {
	if (!loaded || (!loaded_from_card && card_usable())) {
		load(); // a card that came in after the player started is picked up here
	}
	entry_t *e = find(name);
	if (e) {
		return e->value;
	}
	// Before the scores moved to the card the three first games kept theirs in the
	// player's settings, [name] best.
	char section[NAME_MAX_LEN];
	snprintf(section, sizeof(section), "%s", name);
	return config_get_int(section, "best", 0) + config_get_int("games", name, 0);
}

void highscore_set(const char *name, long value) {
	if (!loaded) {
		load();
	}
	if (card_usable()) {
		entry_t *e = find(name);
		if (!e && entry_count < MAX_ENTRIES) {
			e = &entries[entry_count++];
			snprintf(e->name, NAME_MAX_LEN, "%s", name);
		}
		if (e) {
			e->value = value;
			save();
			return;
		}
	}
	config_set_int("games", name, value);
	config_save();
}

bool highscore_submit(const char *name, long value, bool lower_is_better) {
	long old = highscore_get(name);
	bool better = lower_is_better ? (old == 0 || value < old) : value > old;
	if (better && value > 0) {
		highscore_set(name, value);
		return true;
	}
	return false;
}
