#include "subsonicdb.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "src/system/core/utils.h"
#include "src/system/db/sqlite3.h"
#include "src/system/streaming/subsoniccache.h"

#define SYNC_PAGE 500

static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *db;
static char db_path[512];

static const char *const SCHEMA[] = {
	"CREATE TABLE IF NOT EXISTS META(k TEXT PRIMARY KEY, v TEXT)",
	"CREATE TABLE IF NOT EXISTS SONGS("
	"key TEXT PRIMARY KEY, id TEXT NOT NULL, title TEXT, artist TEXT, album TEXT, album_artist TEXT,"
	"album_id TEXT, cover_id TEXT, genre TEXT, suffix TEXT, duration INT, track INT, disc INT, year INT,"
	"bitrate INT, bit_depth INT, sample_rate INT, tg REAL, ag REAL, tp REAL, ap REAL, has_gain INT,"
	"starred INT, added INT, seen INT, sortkey TEXT, artist_key TEXT, album_sort TEXT)",
	"CREATE INDEX IF NOT EXISTS songs_sort_idx ON SONGS(sortkey, key)",
	"CREATE INDEX IF NOT EXISTS songs_artist_idx ON SONGS(artist, album_sort, disc, track)",
	"CREATE INDEX IF NOT EXISTS songs_album_idx ON SONGS(album_id, disc, track)",
	"CREATE INDEX IF NOT EXISTS songs_genre_idx ON SONGS(genre, sortkey)",
	"CREATE INDEX IF NOT EXISTS songs_added_idx ON SONGS(added)",
	"CREATE INDEX IF NOT EXISTS songs_artist_key_idx ON SONGS(artist_key)",
	"CREATE INDEX IF NOT EXISTS songs_album_sort_idx ON SONGS(album_sort)",
};

static bool exec(const char *sql) {
	char *err = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "subsonicdb: %s: %s\n", sql, err ? err : "?");
		sqlite3_free(err);
		return false;
	}
	return true;
}

static void meta_set(const char *key, const char *value) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO META(k,v) VALUES(?,?)", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
}

static bool meta_get(const char *key, char *out, size_t size) {
	out[0] = '\0';
	sqlite3_stmt *stmt = NULL;
	bool found = false;
	if (sqlite3_prepare_v2(db, "SELECT v FROM META WHERE k=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *v = sqlite3_column_text(stmt, 0);
			snprintf(out, size, "%s", v ? (const char *)v : "");
			found = true;
		}
		sqlite3_finalize(stmt);
	}
	return found;
}

// Who the songs belong to. Another server or another user means they are not
// the songs of this one.
static void check_owner(void) {
	char identity[400];
	snprintf(identity, sizeof(identity), "%s|%s", subsonic_server(), subsonic_user());
	char stored[400];
	if (meta_get("owner", stored, sizeof(stored)) && strcmp(stored, identity) != 0) {
		printf("subsonicdb: another server or user, starting over\n");
		exec("DELETE FROM SONGS");
		exec("DELETE FROM META");
	}
	if (subsonic_logged_in()) {
		meta_set("owner", identity);
	}
}

void sdb_open(const char *sd_root) {
	pthread_mutex_lock(&db_lock);
	if (db) {
		sqlite3_close(db);
		db = NULL;
	}
	db_path[0] = '\0';
	if (!sd_root || !*sd_root) {
		pthread_mutex_unlock(&db_lock);
		return;
	}

	char dir[512];
	snprintf(dir, sizeof(dir), "%.490s/.local", sd_root);
	mkdir(dir, 0777);
	snprintf(db_path, sizeof(db_path), "%.490s/subsonic.db", dir);

	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "subsonicdb: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
		sqlite3_close(db);
		db = NULL;
		pthread_mutex_unlock(&db_lock);
		return;
	}
	// A card is slow to write to and the data is a copy of the server's.
	sqlite3_busy_timeout(db, 2000);
	exec("PRAGMA journal_mode=WAL");
	exec("PRAGMA synchronous=OFF");
	for (size_t i = 0; i < sizeof(SCHEMA) / sizeof(SCHEMA[0]); i++) {
		exec(SCHEMA[i]);
	}
	check_owner();
	pthread_mutex_unlock(&db_lock);
}

bool sdb_ready(void) {
	pthread_mutex_lock(&db_lock);
	bool ready = db != NULL;
	pthread_mutex_unlock(&db_lock);
	return ready;
}

// ---------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------

#define SONG_COLUMNS                                                                                               \
	"key,id,title,artist,album,album_artist,album_id,cover_id,genre,suffix,duration,track,disc,year,bitrate,"      \
	"bit_depth,sample_rate,tg,ag,tp,ap,has_gain,starred"

static void copy_text(char *out, size_t size, sqlite3_stmt *stmt, int col) {
	const unsigned char *v = sqlite3_column_text(stmt, col);
	snprintf(out, size, "%s", v ? (const char *)v : "");
}

// The key is not in subsonic_track_t -- it is worked out from the id -- so the
// first column is skipped.
static void read_song(sqlite3_stmt *stmt, subsonic_track_t *t) {
	memset(t, 0, sizeof(*t));
	copy_text(t->id, sizeof(t->id), stmt, 1);
	copy_text(t->title, sizeof(t->title), stmt, 2);
	copy_text(t->artist, sizeof(t->artist), stmt, 3);
	copy_text(t->album, sizeof(t->album), stmt, 4);
	copy_text(t->album_artist, sizeof(t->album_artist), stmt, 5);
	copy_text(t->album_id, sizeof(t->album_id), stmt, 6);
	copy_text(t->cover_id, sizeof(t->cover_id), stmt, 7);
	copy_text(t->genre, sizeof(t->genre), stmt, 8);
	copy_text(t->suffix, sizeof(t->suffix), stmt, 9);
	t->duration = sqlite3_column_int(stmt, 10);
	t->track_number = sqlite3_column_int(stmt, 11);
	t->disc = sqlite3_column_int(stmt, 12);
	t->year = sqlite3_column_int(stmt, 13);
	t->bitrate = sqlite3_column_int(stmt, 14);
	t->bit_depth = sqlite3_column_int(stmt, 15);
	t->sample_rate = sqlite3_column_int(stmt, 16);
	t->track_gain = (float)sqlite3_column_double(stmt, 17);
	t->album_gain = (float)sqlite3_column_double(stmt, 18);
	t->track_peak = (float)sqlite3_column_double(stmt, 19);
	t->album_peak = (float)sqlite3_column_double(stmt, 20);
	t->has_gain = sqlite3_column_int(stmt, 21) != 0;
	t->starred = sqlite3_column_int(stmt, 22) != 0;
}

// The WHERE and ORDER BY of a song list. `value` is bound by the caller as ?1
// (and ?2 for the search, which matches three columns).
static void song_query_parts(sdb_filter_t filter, const char **where, const char **order, bool *needs_value) {
	*needs_value = true;
	switch (filter) {
	case SDB_SONGS_ARTIST:
		*where = "artist = ?1";
		*order = "album_sort, album_id, disc, track, sortkey";
		break;
	case SDB_SONGS_ALBUM:
		*where = "album_id = ?1";
		*order = "disc, track, sortkey";
		break;
	case SDB_SONGS_GENRE:
		*where = "genre = ?1";
		*order = "sortkey, key";
		break;
	case SDB_SONGS_SEARCH:
		*where = "(title LIKE ?1 ESCAPE '\\' OR artist LIKE ?1 ESCAPE '\\' OR album LIKE ?1 ESCAPE '\\')";
		*order = "sortkey, key";
		break;
	case SDB_SONGS_RECENT:
		*where = "1";
		*order = "added DESC, rowid DESC";
		*needs_value = false;
		break;
	case SDB_SONGS_STARRED:
		*where = "starred = 1";
		*order = "sortkey, key";
		*needs_value = false;
		break;
	case SDB_SONGS_ALL:
	default:
		*where = "1";
		*order = "sortkey, key";
		*needs_value = false;
		break;
	}
}

// "%value%" with LIKE's own characters escaped.
static void like_pattern(const char *value, char *out, size_t size) {
	size_t n = 0;
	if (n + 1 < size) {
		out[n++] = '%';
	}
	for (const char *p = value; *p && n + 3 < size; p++) {
		if (*p == '%' || *p == '_' || *p == '\\') {
			out[n++] = '\\';
		}
		out[n++] = *p;
	}
	if (n + 2 < size) {
		out[n++] = '%';
	}
	out[n] = '\0';
}

static void bind_value(sqlite3_stmt *stmt, sdb_filter_t filter, const char *value) {
	if (filter == SDB_SONGS_SEARCH) {
		char pattern[300];
		like_pattern(value ? value : "", pattern, sizeof(pattern));
		sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
	} else if (value) {
		sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
	}
}

int sdb_total(void) {
	pthread_mutex_lock(&db_lock);
	int n = -1;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM SONGS", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			n = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return n;
}

int sdb_song_count(sdb_filter_t filter, const char *value) {
	const char *where, *order;
	bool needs_value;
	song_query_parts(filter, &where, &order, &needs_value);

	char sql[400];
	snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM SONGS WHERE %s", where);

	pthread_mutex_lock(&db_lock);
	int n = -1;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		bind_value(stmt, filter, value);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			n = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return n;
}

int sdb_songs(sdb_filter_t filter, const char *value, int offset, int count, subsonic_track_t *out) {
	const char *where, *order;
	bool needs_value;
	song_query_parts(filter, &where, &order, &needs_value);

	char sql[600];
	snprintf(sql, sizeof(sql), "SELECT " SONG_COLUMNS " FROM SONGS WHERE %s ORDER BY %s LIMIT %d OFFSET %d", where,
			 order, count, offset);

	pthread_mutex_lock(&db_lock);
	int n = -1;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		bind_value(stmt, filter, value);
		n = 0;
		while (n < count && sqlite3_step(stmt) == SQLITE_ROW) {
			read_song(stmt, &out[n++]);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return n;
}

bool sdb_find(const char *key, subsonic_track_t *out) {
	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT " SONG_COLUMNS " FROM SONGS WHERE key=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			read_song(stmt, out);
			found = true;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

bool sdb_starred(const char *key) {
	pthread_mutex_lock(&db_lock);
	bool on = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT starred FROM SONGS WHERE key=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		on = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return on;
}

void sdb_set_starred(const char *key, bool on) {
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "UPDATE SONGS SET starred=? WHERE key=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, on ? 1 : 0);
		sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

// ---------------------------------------------------------------------------
// names
// ---------------------------------------------------------------------------

// Group by, and what to show, for each kind of name.
static const char *names_group(sdb_names_t kind) {
	switch (kind) {
	case SDB_NAMES_ARTISTS:
		return "artist";
	case SDB_NAMES_GENRES:
		return "genre";
	case SDB_NAMES_ALBUMS:
	default:
		// Two albums of one name by different artists are two albums; and a
		// server that sends no album id still has the name.
		return "COALESCE(NULLIF(album_id,''), album)";
	}
}

static const char *names_where(sdb_names_t kind) {
	switch (kind) {
	case SDB_NAMES_ARTISTS:
		return "artist <> ''";
	case SDB_NAMES_GENRES:
		return "genre <> ''";
	case SDB_NAMES_ALBUMS:
	default:
		return "album <> ''";
	}
}

static const char *names_order(sdb_names_t kind) {
	switch (kind) {
	case SDB_NAMES_ARTISTS:
		return "MIN(artist_key)";
	case SDB_NAMES_GENRES:
		return "MIN(genre)";
	case SDB_NAMES_ALBUMS:
	default:
		return "MIN(album_sort), MIN(artist_key)";
	}
}

int sdb_name_count(sdb_names_t kind) {
	char sql[400];
	snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM (SELECT 1 FROM SONGS WHERE %s GROUP BY %s)", names_where(kind),
			 names_group(kind));
	pthread_mutex_lock(&db_lock);
	int n = -1;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			n = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return n;
}

int sdb_names(sdb_names_t kind, int offset, int count, sdb_name_t *out) {
	char sql[700];
	// name, sub, value, cover, count
	const char *columns;
	switch (kind) {
	case SDB_NAMES_ARTISTS:
		columns = "artist, '', artist, MIN(cover_id), COUNT(*)";
		break;
	case SDB_NAMES_GENRES:
		columns = "genre, '', genre, MIN(cover_id), COUNT(*)";
		break;
	case SDB_NAMES_ALBUMS:
	default:
		columns = "album, COALESCE(NULLIF(MIN(album_artist),''), MIN(artist)), COALESCE(NULLIF(album_id,''), album), "
				  "MIN(cover_id), COUNT(*)";
		break;
	}
	snprintf(sql, sizeof(sql), "SELECT %s FROM SONGS WHERE %s GROUP BY %s ORDER BY %s LIMIT %d OFFSET %d", columns,
			 names_where(kind), names_group(kind), names_order(kind), count, offset);

	pthread_mutex_lock(&db_lock);
	int n = -1;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		n = 0;
		while (n < count && sqlite3_step(stmt) == SQLITE_ROW) {
			sdb_name_t *row = &out[n++];
			memset(row, 0, sizeof(*row));
			copy_text(row->name, sizeof(row->name), stmt, 0);
			copy_text(row->sub, sizeof(row->sub), stmt, 1);
			copy_text(row->value, sizeof(row->value), stmt, 2);
			copy_text(row->cover_id, sizeof(row->cover_id), stmt, 3);
			row->count = sqlite3_column_int(stmt, 4);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return n;
}

// ---------------------------------------------------------------------------
// the A-Z strip
// ---------------------------------------------------------------------------

static void buckets_from(const char *sql, const char *value, int first[LIBRARY_INDEX_BUCKETS]) {
	for (int i = 0; i < LIBRARY_INDEX_BUCKETS; i++) {
		first[i] = -1;
	}
	sqlite3_stmt *stmt = NULL;
	if (!db || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		return;
	}
	if (value) {
		sqlite3_bind_text(stmt, 1, value, -1, SQLITE_STATIC);
	}
	int row = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *key = sqlite3_column_text(stmt, 0);
		int slot = library_index_slot(library_index_letter_of_key(key ? (const char *)key : ""));
		if (slot >= 0 && slot < LIBRARY_INDEX_BUCKETS && first[slot] < 0) {
			first[slot] = row;
		}
		row++;
	}
	sqlite3_finalize(stmt);
}

bool sdb_song_buckets(sdb_filter_t filter, const char *value, int first[LIBRARY_INDEX_BUCKETS]) {
	pthread_mutex_lock(&db_lock);
	bool ok = db != NULL;
	if (ok) {
		if (filter == SDB_SONGS_GENRE) {
			buckets_from("SELECT sortkey FROM SONGS WHERE genre = ?1 ORDER BY sortkey, key", value, first);
		} else if (filter == SDB_SONGS_STARRED) {
			buckets_from("SELECT sortkey FROM SONGS WHERE starred = 1 ORDER BY sortkey, key", NULL, first);
		} else {
			buckets_from("SELECT sortkey FROM SONGS ORDER BY sortkey, key", NULL, first);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool sdb_name_buckets(sdb_names_t kind, int first[LIBRARY_INDEX_BUCKETS]) {
	char sql[400];
	const char *key = kind == SDB_NAMES_ALBUMS ? "MIN(album_sort)" : (kind == SDB_NAMES_ARTISTS ? "MIN(artist_key)" : "MIN(genre)");
	snprintf(sql, sizeof(sql), "SELECT %s FROM SONGS WHERE %s GROUP BY %s ORDER BY %s", key, names_where(kind),
			 names_group(kind), names_order(kind));
	pthread_mutex_lock(&db_lock);
	bool ok = db != NULL;
	if (ok) {
		buckets_from(sql, NULL, first);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

// ---------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------

typedef struct {
	sqlite3_stmt *replace;
	sqlite3_stmt *ignore;
} writer_t;

static bool writer_open(writer_t *w) {
	static const char *const COLUMNS =
		"(key,id,title,artist,album,album_artist,album_id,cover_id,genre,suffix,duration,track,disc,year,bitrate,"
		"bit_depth,sample_rate,tg,ag,tp,ap,has_gain,starred,added,seen,sortkey,artist_key,album_sort)"
		" VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
	char sql[800];
	snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO SONGS%s", COLUMNS);
	if (sqlite3_prepare_v2(db, sql, -1, &w->replace, NULL) != SQLITE_OK) {
		return false;
	}
	snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO SONGS%s", COLUMNS);
	if (sqlite3_prepare_v2(db, sql, -1, &w->ignore, NULL) != SQLITE_OK) {
		sqlite3_finalize(w->replace);
		return false;
	}
	return true;
}

static void writer_close(writer_t *w) {
	sqlite3_finalize(w->replace);
	sqlite3_finalize(w->ignore);
}

static void write_song(sqlite3_stmt *stmt, const subsonic_track_t *t, long added, int seen) {
	char key[SUBSONICCACHE_KEY_MAX];
	subsoniccache_key(t->id, key, sizeof(key));

	// The title read as the library reads it, so the order is the same: case
	// and accents ignored, a leading "The" skipped.
	char sortkey[LIBRARY_SORT_KEY_MAX], artist_key[LIBRARY_SORT_KEY_MAX], album_sort[LIBRARY_SORT_KEY_MAX];
	library_sort_key(t->title, sortkey, sizeof(sortkey));
	library_sort_key(t->artist, artist_key, sizeof(artist_key));
	library_sort_key(t->album, album_sort, sizeof(album_sort));

	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	int c = 1;
	sqlite3_bind_text(stmt, c++, key, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, c++, t->id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, c++, t->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, c++, t->artist, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, c++, t->album, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, c++, t->album_artist, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, c++, t->album_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, c++, t->cover_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, c++, t->genre, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, c++, t->suffix, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, c++, t->duration);
	sqlite3_bind_int(stmt, c++, t->track_number);
	sqlite3_bind_int(stmt, c++, t->disc);
	sqlite3_bind_int(stmt, c++, t->year);
	sqlite3_bind_int(stmt, c++, t->bitrate);
	sqlite3_bind_int(stmt, c++, t->bit_depth);
	sqlite3_bind_int(stmt, c++, t->sample_rate);
	sqlite3_bind_double(stmt, c++, (double)t->track_gain);
	sqlite3_bind_double(stmt, c++, (double)t->album_gain);
	sqlite3_bind_double(stmt, c++, (double)t->track_peak);
	sqlite3_bind_double(stmt, c++, (double)t->album_peak);
	sqlite3_bind_int(stmt, c++, t->has_gain ? 1 : 0);
	sqlite3_bind_int(stmt, c++, t->starred ? 1 : 0);
	sqlite3_bind_int64(stmt, c++, added);
	sqlite3_bind_int(stmt, c++, seen);
	sqlite3_bind_text(stmt, c++, sortkey, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, c++, artist_key, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, c++, album_sort, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_reset(stmt);
}

void sdb_remember(const subsonic_track_t *tracks, int count) {
	pthread_mutex_lock(&db_lock);
	writer_t w;
	if (db && count > 0 && writer_open(&w)) {
		exec("BEGIN");
		for (int i = 0; i < count; i++) {
			if (tracks[i].id[0]) {
				// seen = 0 belongs to no sync, which prunes such songs only if
				// the server no longer has them -- and it does, they came from it.
				write_song(w.ignore, &tracks[i], (long)time(NULL), -1);
			}
		}
		exec("COMMIT");
		writer_close(&w);
	}
	pthread_mutex_unlock(&db_lock);
}

// ---------------------------------------------------------------------------
// by path
// ---------------------------------------------------------------------------

bool sdb_path_starred(const char *path) {
	char key[SUBSONICCACHE_KEY_MAX];
	return subsoniccache_key_from_path(path, key, sizeof(key)) && sdb_starred(key);
}

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char key[SUBSONICCACHE_KEY_MAX];
	bool on;
	void (*done)(bool ok, const char *error, void *user);
	void *user;
} star_job_t;

static void *star_main(void *arg) {
	star_job_t *job = arg;
	bool ok = subsonic_star(job->id, job->on);
	if (!ok) {
		sdb_set_starred(job->key, !job->on); // the server said no: take it back
	}
	if (job->done) {
		job->done(ok, ok ? "" : subsonic_last_error(), job->user);
	}
	free(job);
	return NULL;
}

void sdb_path_toggle_star(const char *path, void (*done)(bool ok, const char *error, void *user), void *user) {
	star_job_t *job = calloc(1, sizeof(*job));
	subsonic_track_t track;
	if (!job || !subsoniccache_key_from_path(path, job->key, sizeof(job->key)) || !sdb_find(job->key, &track)) {
		free(job);
		if (done) {
			done(false, "", user);
		}
		return;
	}
	snprintf(job->id, sizeof(job->id), "%s", track.id);
	job->on = !track.starred;
	job->done = done;
	job->user = user;
	sdb_set_starred(job->key, job->on);

	pthread_t thread;
	if (pthread_create(&thread, NULL, star_main, job) == 0) {
		pthread_detach(thread);
	} else {
		sdb_set_starred(job->key, !job->on);
		free(job);
		if (done) {
			done(false, "", user);
		}
	}
}

static void *scrobble_main(void *arg) {
	char *id = arg;
	subsonic_scrobble(id);
	free(id);
	return NULL;
}

void sdb_path_scrobble(const char *path) {
	char key[SUBSONICCACHE_KEY_MAX];
	subsonic_track_t track;
	if (!subsonic_logged_in() || !subsoniccache_key_from_path(path, key, sizeof(key)) || !sdb_find(key, &track)) {
		return;
	}
	char *id = strdup(track.id);
	pthread_t thread;
	if (id && pthread_create(&thread, NULL, scrobble_main, id) == 0) {
		pthread_detach(thread);
	} else {
		free(id);
	}
}

// ---------------------------------------------------------------------------
// sync
// ---------------------------------------------------------------------------

static pthread_t sync_thread;
static volatile bool sync_running;
static volatile int sync_found;
static char sync_error[256];
static long last_sync;
static void (*sync_listener)(bool ok);

const char *sdb_sync_error(void) { return sync_error; }
bool sdb_sync_running(void) { return sync_running; }
int sdb_sync_found(void) { return sync_found; }
void sdb_set_sync_listener(void (*cb)(bool ok)) { sync_listener = cb; }

long sdb_last_sync(void) {
	pthread_mutex_lock(&db_lock);
	long when = last_sync;
	if (when == 0 && db) {
		char v[32];
		if (meta_get("synced", v, sizeof(v))) {
			when = atol(v);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return when;
}

static int next_generation(void) {
	char v[32];
	int gen = 1;
	if (meta_get("generation", v, sizeof(v))) {
		gen = atoi(v) + 1;
	}
	snprintf(v, sizeof(v), "%d", gen);
	meta_set("generation", v);
	return gen;
}

// Stores one page of songs. Returns false when the database has gone.
static bool store_page(const subsonic_track_t *page, int n, int gen) {
	pthread_mutex_lock(&db_lock);
	writer_t w;
	bool ok = db && writer_open(&w);
	if (ok) {
		exec("BEGIN");
		long now = (long)time(NULL);
		for (int i = 0; i < n; i++) {
			// A song already held keeps the day it first arrived: that is what
			// "recently added" means. INSERT OR REPLACE would reset it, so the
			// date is read first.
			long added = now;
			sqlite3_stmt *stmt = NULL;
			char key[SUBSONICCACHE_KEY_MAX];
			subsoniccache_key(page[i].id, key, sizeof(key));
			if (sqlite3_prepare_v2(db, "SELECT added FROM SONGS WHERE key=?", -1, &stmt, NULL) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					added = (long)sqlite3_column_int64(stmt, 0);
				}
				sqlite3_finalize(stmt);
			}
			write_song(w.replace, &page[i], added, gen);
		}
		exec("COMMIT");
		writer_close(&w);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

// Albums are the way into a server that will not list its songs without a
// query: every album, then its songs.
static int crawl_albums(int gen, subsonic_track_t *page, int page_size) {
	subsonic_album_t *albums = malloc(sizeof(*albums) * 100);
	if (!albums) {
		return -1;
	}
	int total = 0;
	for (int offset = 0;; offset += 100) {
		int n = subsonic_album_list("alphabeticalByName", offset, albums, 100);
		if (n < 0) {
			free(albums);
			return -1;
		}
		for (int i = 0; i < n; i++) {
			int songs = subsonic_album_tracks(albums[i].id, 0, page, page_size);
			if (songs > 0) {
				store_page(page, songs, gen);
				total += songs;
				sync_found = total;
			}
		}
		if (n < 100) {
			break;
		}
	}
	free(albums);
	return total;
}

static void *sync_main(void *arg) {
	(void)arg;
	thread_be_background("subsonic sync");

	bool ok = false;
	sync_error[0] = '\0';
	sync_found = 0;

	subsonic_track_t *page = malloc(sizeof(*page) * SYNC_PAGE);
	if (!page) {
		snprintf(sync_error, sizeof(sync_error), "out of memory");
	} else {
		pthread_mutex_lock(&db_lock);
		int gen = db ? next_generation() : 0;
		pthread_mutex_unlock(&db_lock);

		int total = 0;
		if (gen > 0) {
			// An empty search lists everything on Navidrome and most others.
			for (int offset = 0;; offset += SYNC_PAGE) {
				int n = subsonic_search_tracks("", offset, page, SYNC_PAGE);
				if (n < 0) {
					total = -1;
					break;
				}
				if (n == 0) {
					break;
				}
				if (!store_page(page, n, gen)) {
					total = -1;
					break;
				}
				total += n;
				sync_found = total;
				if (n < SYNC_PAGE) {
					break;
				}
			}
			// A server that answers an empty search with nothing is walked
			// album by album instead.
			if (total == 0) {
				total = crawl_albums(gen, page, SYNC_PAGE);
			}
		}

		if (total < 0) {
			snprintf(sync_error, sizeof(sync_error), "%s", subsonic_last_error());
		} else {
			// What the server no longer has goes. Songs added by hand
			// (sdb_remember) carry no generation and are left alone.
			pthread_mutex_lock(&db_lock);
			if (db) {
				char sql[160];
				snprintf(sql, sizeof(sql), "DELETE FROM SONGS WHERE seen >= 0 AND seen <> %d", gen);
				exec(sql);
				char v[32];
				last_sync = (long)time(NULL);
				snprintf(v, sizeof(v), "%ld", last_sync);
				meta_set("synced", v);
			}
			pthread_mutex_unlock(&db_lock);
			ok = true;
			printf("subsonicdb: %d songs\n", total);
		}
		free(page);
	}

	sync_running = false;
	if (sync_listener) {
		sync_listener(ok);
	}
	return NULL;
}

bool sdb_sync_start(void) {
	if (sync_running || !subsonic_logged_in() || !sdb_ready()) {
		return false;
	}
	sync_running = true;
	if (pthread_create(&sync_thread, NULL, sync_main, NULL) != 0) {
		sync_running = false;
		return false;
	}
	pthread_detach(sync_thread);
	return true;
}
