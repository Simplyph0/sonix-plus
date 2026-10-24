#include "subsoniccache.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamturn.h"

#define FETCH_TIMEOUT_SECS 20
#define FETCH_CHUNK 32768

static char cache_dir[512];

// Every error exit records what happened, so that no card, no network and a
// full card do not all reach the page as one "download failed".
static __thread char last_error[192];

const char *subsoniccache_last_error(void) { return last_error; }

static void set_error(const char *what) { snprintf(last_error, sizeof(last_error), "%s", what ? what : ""); }

void subsoniccache_abandon_all(void);

// Registers with the shared turn once, at the first opportunity: from then on a
// track requested from another service knows how to make this download let go.
static void register_abandon_once(void) {
	static bool done;
	if (!done) {
		done = true;
		streamturn_register_abandon(subsoniccache_abandon_all);
	}
}

void subsoniccache_set_root(const char *sd_root) {
	register_abandon_once();
	if (!sd_root || !*sd_root) {
		cache_dir[0] = '\0';
		return;
	}
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, SUBSONICCACHE_DIR);
}

bool subsoniccache_ready(void) { return cache_dir[0] != '\0'; }

void subsoniccache_key(const char *track_id, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	size_t n = 0;
	for (const char *p = track_id ? track_id : ""; *p && n + 1 < size && n + 1 < SUBSONICCACHE_KEY_MAX; p++, n++) {
		char c = *p;
		bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
		out[n] = plain ? c : '_';
	}
	out[n] = '\0';
}

void subsoniccache_path(const char *track_id, const char *suffix, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (!subsoniccache_ready()) {
		out[0] = '\0';
		return;
	}
	char key[SUBSONICCACHE_KEY_MAX];
	subsoniccache_key(track_id, key, sizeof(key));
	// The directory part is bounded so the file name always fits: a path
	// truncated halfway would silently open (or delete) something elsewhere.
	snprintf(out, size, "%.380s/%s.%s", cache_dir, key, suffix && *suffix ? suffix : "mp3");
}

// A marker next to the file says the download is incomplete. The track is
// written straight to its final name (the decoder opens it while it grows), so
// completeness is recorded elsewhere.
static void marker_path(const char *path, char *out, size_t size) { snprintf(out, size, "%.500s.incompleto", path); }

bool subsoniccache_has(const char *track_id, const char *suffix, char *out, size_t size) {
	char path[512];
	subsoniccache_path(track_id, suffix, path, sizeof(path));
	if (!path[0]) {
		return false;
	}

	char marker[544];
	marker_path(path, marker, sizeof(marker));
	struct stat st;
	if (stat(marker, &st) == 0) {
		return false; // interrupted: download it again
	}
	if (stat(path, &st) != 0 || st.st_size == 0) {
		return false;
	}
	if (out && size) {
		snprintf(out, size, "%s", path);
	}
	return true;
}

bool subsoniccache_owns(const char *path) {
	return path && cache_dir[0] && strncmp(path, cache_dir, strlen(cache_dir)) == 0;
}

const char *subsoniccache_dir(void) { return cache_dir[0] ? cache_dir : NULL; }

bool subsoniccache_key_from_path(const char *path, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!subsoniccache_owns(path)) {
		return false;
	}
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	const char *dot = strchr(name, '.');
	size_t len = dot ? (size_t)(dot - name) : strlen(name);
	if (len == 0 || len >= size) {
		return false;
	}
	memcpy(out, name, len);
	out[len] = '\0';
	return true;
}

static bool ensure_dir(void) {
	if (!subsoniccache_ready()) {
		return false;
	}
	char path[512];
	snprintf(path, sizeof(path), "%s", cache_dir);
	for (char *p = path + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(path, 0777);
		*p = '/';
	}
	if (mkdir(path, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "subsoniccache: cannot create %s: %s\n", path, strerror(errno));
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// measuring and pruning
// ---------------------------------------------------------------------------

typedef struct {
	char name[96];
	long size;
	long atime; // last access: what tells which tracks are actually listened to
} entry_t;

// The name of a file without its extensions: "<key>.flac.tags" -> "<key>".
static void stem_of(const char *name, char *out, size_t size) {
	snprintf(out, size, "%s/%.80s", cache_dir, name);
	char *dot = strchr(out + strlen(cache_dir) + 1, '.');
	if (dot) {
		*dot = '\0';
	}
}

static int scan(entry_t *out, int max, long *total_out) {
	long total = 0;
	int count = 0;

	DIR *d = opendir(cache_dir);
	if (!d) {
		if (total_out) {
			*total_out = 0;
		}
		return 0;
	}

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		total += st.st_size;

		// What is coming down right now is not prunable, nor are its sidecar
		// files.
		char stem[640];
		stem_of(e->d_name, stem, sizeof(stem));
		if (growfile_prefix_is_growing(stem)) {
			continue;
		}
		if (out && count < max && strlen(e->d_name) < sizeof(out[0].name)) {
			snprintf(out[count].name, sizeof(out[count].name), "%s", e->d_name);
			out[count].size = (long)st.st_size;
			out[count].atime = (long)st.st_atime;
			count++;
		}
	}
	closedir(d);

	if (total_out) {
		*total_out = total;
	}
	return count;
}

long subsoniccache_bytes(void) {
	long total = 0;
	if (subsoniccache_ready()) {
		scan(NULL, 0, &total);
	}
	return total;
}

static bool is_protected_stem(const char *stem);
static bool is_recent_stem(const char *stem);

#define PRUNE_MAX_ENTRIES 512

// Drops the least recently listened to until the cache fits again.
static void prune(long keep_room_for) {
	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		return;
	}

	long total = 0;
	int count = scan(entries, PRUNE_MAX_ENTRIES, &total);
	if (total + keep_room_for <= SUBSONICCACHE_MAX_BYTES) {
		free(entries);
		return;
	}

	for (int i = 0; i < count; i++) {
		int oldest = i;
		for (int k = i + 1; k < count; k++) {
			if (entries[k].atime < entries[oldest].atime) {
				oldest = k;
			}
		}
		entry_t tmp = entries[i];
		entries[i] = entries[oldest];
		entries[oldest] = tmp;
	}

	for (int i = 0; i < count && total + keep_room_for > SUBSONICCACHE_MAX_BYTES; i++) {
		char stem[640];
		stem_of(entries[i].name, stem, sizeof(stem));
		if (is_protected_stem(stem) || is_recent_stem(stem)) {
			continue; // still to be played
		}
		char path[640];
		snprintf(path, sizeof(path), "%s/%s", cache_dir, entries[i].name);
		if (remove(path) == 0) {
			total -= entries[i].size;
			printf("subsoniccache: removed %s (%ld KB)\n", entries[i].name, entries[i].size / 1024);
		}
	}
	free(entries);
}

// The tracks the queue still has ahead, which a sweep must not touch.
static pthread_mutex_t protected_lock = PTHREAD_MUTEX_INITIALIZER;
static char protected_paths[SUBSONICCACHE_PROTECTED_MAX][512];
static int protected_count;

void subsoniccache_set_protected(const char *const *paths, int count) {
	pthread_mutex_lock(&protected_lock);
	protected_count = 0;
	for (int i = 0; i < count && protected_count < SUBSONICCACHE_PROTECTED_MAX; i++) {
		if (paths[i] && paths[i][0]) {
			snprintf(protected_paths[protected_count++], sizeof(protected_paths[0]), "%s", paths[i]);
		}
	}
	pthread_mutex_unlock(&protected_lock);
}

// True when `stem` is the name, without extension, of a protected track.
static bool is_protected_stem(const char *stem) {
	size_t len = strlen(stem);
	pthread_mutex_lock(&protected_lock);
	bool match = false;
	for (int i = 0; i < protected_count && !match; i++) {
		match = strncmp(protected_paths[i], stem, len) == 0 && protected_paths[i][len] == '.';
	}
	pthread_mutex_unlock(&protected_lock);
	return match;
}

// A small ring of the tracks downloaded last: a safety net for the instant
// between "the file exists" and "the queue knows about it".
#define RECENT_MAX 8
static pthread_mutex_t recent_lock = PTHREAD_MUTEX_INITIALIZER;
static char recent_paths[RECENT_MAX][512];
static int recent_next;

static bool is_recent_stem(const char *stem) {
	size_t len = strlen(stem);
	pthread_mutex_lock(&recent_lock);
	bool match = false;
	for (int i = 0; i < RECENT_MAX && !match; i++) {
		match = recent_paths[i][0] && strncmp(recent_paths[i], stem, len) == 0 && recent_paths[i][len] == '.';
	}
	pthread_mutex_unlock(&recent_lock);
	return match;
}

static void clear_files(bool keep_playing) {
	if (!subsoniccache_ready()) {
		return;
	}
	DIR *d = opendir(cache_dir);
	if (!d) {
		return;
	}
	int removed = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		if (keep_playing) {
			char stem[640];
			stem_of(e->d_name, stem, sizeof(stem));
			if (growfile_prefix_is_growing(stem) || is_protected_stem(stem) || is_recent_stem(stem)) {
				continue;
			}
		}
		if (remove(path) == 0) {
			removed++;
		}
	}
	closedir(d);
	printf("subsoniccache: cleared (%d files)\n", removed);
}

void subsoniccache_clear(void) { clear_files(false); }

void subsoniccache_clear_on_exit(void) {
	if (!subsoniccache_ready()) {
		return;
	}
	printf("subsoniccache: shutting down, clearing the cache\n");
	clear_files(false);
}

static int played_since_clear;

void subsoniccache_note_played(const char *path) {
	if (!subsoniccache_ready()) {
		return;
	}
	if (path && path[0]) {
		pthread_mutex_lock(&recent_lock);
		snprintf(recent_paths[recent_next], sizeof(recent_paths[0]), "%s", path);
		recent_next = (recent_next + 1) % RECENT_MAX;
		pthread_mutex_unlock(&recent_lock);
	}
	if (++played_since_clear < SUBSONICCACHE_CLEAR_EVERY) {
		return;
	}
	played_since_clear = 0;
	clear_files(true);
}

// ---------------------------------------------------------------------------
// downloading
// ---------------------------------------------------------------------------

typedef struct {
	char key[SUBSONICCACHE_KEY_MAX];
	char path[512];
	char marker[544];
	http_stream_t stream;
	FILE *f;
	long done;
	long total;
	unsigned generation;
} download_t;

// Changing track bumps this counter; threads downloading something else notice
// at the next chunk and stop, so the bandwidth goes to what is playing now.
static unsigned generation = 1;
static pthread_mutex_t gen_lock = PTHREAD_MUTEX_INITIALIZER;

static void (*slow_start_cb)(int seconds);

void subsoniccache_set_slow_start_cb(void (*cb)(int seconds)) { slow_start_cb = cb; }

void subsoniccache_wait_idle(void) { streamturn_wait_idle(); }

// The in-progress download's stream, so another thread can jolt it awake. Only
// one runs at a time, so a single pointer suffices.
static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *active_stream;
static char active_key[SUBSONICCACHE_KEY_MAX];

bool subsoniccache_downloading(const char *track_id) {
	char key[SUBSONICCACHE_KEY_MAX];
	subsoniccache_key(track_id, key, sizeof(key));
	pthread_mutex_lock(&active_lock);
	bool yes = active_key[0] && strcmp(active_key, key) == 0;
	pthread_mutex_unlock(&active_lock);
	return yes;
}

bool subsoniccache_any_downloading(void) {
	pthread_mutex_lock(&active_lock);
	bool yes = active_key[0] != '\0';
	pthread_mutex_unlock(&active_lock);
	return yes;
}

static volatile bool network_wanted;

void subsoniccache_set_network_wanted(bool wanted) { network_wanted = wanted; }
bool subsoniccache_network_wanted(void) { return network_wanted; }

void subsoniccache_abandon_all(void) {
	pthread_mutex_lock(&gen_lock);
	generation++;
	pthread_mutex_unlock(&gen_lock);

	// The counter alone is noticed only at the next chunk; shutting the socket
	// down under the blocked recv() makes the download let go at once.
	pthread_mutex_lock(&active_lock);
	if (active_stream) {
		http_stream_wake(active_stream);
	}
	pthread_mutex_unlock(&active_lock);
}

static unsigned current_generation(void) {
	pthread_mutex_lock(&gen_lock);
	unsigned value = generation;
	pthread_mutex_unlock(&gen_lock);
	return value;
}

static void clear_active(const char *key) {
	pthread_mutex_lock(&active_lock);
	if (strcmp(active_key, key) == 0) {
		active_key[0] = '\0';
	}
	pthread_mutex_unlock(&active_lock);
}

// Closes everything and records how it ended for readers of the file.
static void download_end(download_t *d, bool ok) {
	// Out of view of whoever might jolt the stream first, closed second.
	pthread_mutex_lock(&active_lock);
	if (active_stream == &d->stream) {
		active_stream = NULL;
	}
	if (strcmp(active_key, d->key) == 0) {
		active_key[0] = '\0';
	}
	pthread_mutex_unlock(&active_lock);

	if (d->f) {
		if (fclose(d->f) != 0) {
			ok = false;
		}
		d->f = NULL;
	}
	http_stream_close(&d->stream);

	if (ok && d->total > 0 && d->done != d->total) {
		fprintf(stderr, "subsoniccache: %s cut short at %ld of %ld bytes\n", d->key, d->done, d->total);
		ok = false;
	}
	if (ok && d->done == 0) {
		ok = false;
	}

	growfile_finish(d->path, ok);

	if (ok) {
		remove(d->marker);
		printf("subsoniccache: %s complete (%ld KB)\n", d->key, d->done / 1024);
	} else {
		// The marker stays: next time subsoniccache_has() says no and the track
		// is downloaded again instead of playing half of it.
		printf("subsoniccache: %s not completed\n", d->key);
	}

	// Released last: while the marker is there the track counts as incomplete,
	// so anyone waiting for the turn would start re-downloading it.
	streamturn_release();
	free(d);
}

// Writes until `until` bytes are on disk (or, with until == 0, to the end of
// the stream). Returns false when it went wrong.
static bool download_pump(download_t *d, long until, bool *finished) {
	char buf[FETCH_CHUNK];
	*finished = false;

	while (d->done < until || until == 0) {
		if (current_generation() != d->generation) {
			return false; // something else is playing now
		}
		int n = http_stream_read(&d->stream, buf, (int)sizeof(buf));
		if (n < 0) {
			return false;
		}
		if (n == 0) {
			*finished = true;
			return true;
		}
		if (fwrite(buf, 1, (size_t)n, d->f) != (size_t)n) {
			fprintf(stderr, "subsoniccache: write failed (card full?)\n");
			return false;
		}
		// Flushed before the progress is announced: a reader would otherwise go
		// looking on disk for bytes still in the stdio buffer.
		fflush(d->f);
		d->done += n;
		growfile_progress(d->path, d->done);
	}
	return true;
}

static void *download_thread(void *arg) {
	// Background priority, like every other worker: there is one core.
	thread_be_background("subsonic download");

	download_t *d = arg;
	bool finished = false;
	bool ok = download_pump(d, 0, &finished);
	download_end(d, ok && finished);
	return NULL;
}

bool subsoniccache_start(const char *track_id, const char *url, const char *suffix, int duration_secs, char *out,
						 size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	set_error("");

	if (!subsoniccache_ready()) {
		set_error(tr("download_no_card"));
		return false;
	}
	if (!url || !*url) {
		set_error(tr("subsonic_no_track_address"));
		return false;
	}

	// Already fully downloaded: no network, no wait.
	if (subsoniccache_has(track_id, suffix, out, size)) {
		return true;
	}
	if (!ensure_dir()) {
		set_error(tr("cache_folder_failed"));
		return false;
	}

	download_t *d = calloc(1, sizeof(*d));
	if (!d) {
		set_error(tr("out_of_memory"));
		return false;
	}
	subsoniccache_key(track_id, d->key, sizeof(d->key));
	subsoniccache_path(track_id, suffix, d->path, sizeof(d->path));
	marker_path(d->path, d->marker, sizeof(d->marker));

	// This track is already coming down, and it is the one playing. Starting
	// again would truncate the file under the decoder reading it.
	if (growfile_is_growing(d->path)) {
		snprintf(out, size, "%.500s", d->path);
		free(d);
		return true;
	}

	// One at a time. Abandoning the running one is the caller's business, not
	// this function's: prefetch calls it too.
	streamturn_acquire();

	// Taken AFTER the wait: a value read before would be stale, and a track
	// change during the wait would cancel this download as it starts.
	d->generation = current_generation();

	// Published at once, not when the connection is up: callers act on "is it
	// downloading?" and the connection takes seconds.
	pthread_mutex_lock(&active_lock);
	snprintf(active_key, sizeof(active_key), "%s", d->key);
	pthread_mutex_unlock(&active_lock);

	if (!http_stream_open(&d->stream, url, FETCH_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		fprintf(stderr, "subsoniccache: %s does not open%s%s\n", d->key, why && why[0] ? ": " : "",
				why && why[0] ? why : "");
		set_error(why && why[0] ? why : tr("subsonic_cannot_reach"));
		clear_active(d->key);
		streamturn_release();
		free(d);
		return false;
	}

	d->total = d->stream.content_length;
	prune(d->total > 0 ? d->total : 0);

	// The marker before the file: if power is lost between the two, an orphan
	// marker beats an incomplete file that looks good.
	FILE *marker = fopen(d->marker, "wb");
	if (marker) {
		fclose(marker);
	}

	d->f = fopen(d->path, "wb");
	if (!d->f) {
		fprintf(stderr, "subsoniccache: %s does not open for writing: %s\n", d->path, strerror(errno));
		set_error(errno == ENOSPC ? tr("card_full") : tr("card_write_failed"));
		http_stream_close(&d->stream);
		clear_active(d->key);
		streamturn_release();
		free(d);
		return false;
	}

	pthread_mutex_lock(&active_lock);
	active_stream = &d->stream;
	pthread_mutex_unlock(&active_lock);

	growfile_announce(d->path, d->total);

	// --- prebuffer, measured rather than guessed -----------------------------
	// The reasoning is in qobuzcache.c: time a first chunk, and start once
	// enough is in hand for the download to stay ahead of playback.
	long want = STREAM_PREBUFFER;
	if (d->total > 0 && d->total < want) {
		want = d->total;
	}
	long probe = want < STREAM_PROBE ? want : STREAM_PROBE;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	bool finished = false;
	if (!download_pump(d, probe, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		return false;
	}

	if (!finished && d->total > 0 && duration_secs > 0) {
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		double measured = elapsed > 0.3 ? d->done / elapsed : 0.0;
		double required = (double)d->total / duration_secs;

		if (required > 0) {
			long floor_bytes = (long)(required * STREAM_HEAD_SECS);
			if (floor_bytes > want) {
				want = floor_bytes;
			}
			if (want > d->total) {
				want = d->total;
			}
		}
		if (measured > 0 && required > 0) {
			double head = (double)d->total * (1.0 - measured / (required * 1.2));
			double cap = required * STREAM_HEAD_MAX_SECS;
			if (head > cap) {
				head = cap;
			}
			if (head > want) {
				want = (long)head;
			}
			if (want > d->total) {
				want = d->total;
			}
			int wait_secs = (int)((want - d->done) / measured);
			if (wait_secs >= 2 && slow_start_cb) {
				slow_start_cb(wait_secs);
			}
		}
	}

	if (!finished && d->done < want && !download_pump(d, want, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		return false;
	}

	snprintf(out, size, "%.500s", d->path);
	if (finished) {
		download_end(d, true); // a short track: all of it is already here
		return true;
	}

	// From the thread's start `d` belongs to it.
	pthread_t thread;
	if (pthread_create(&thread, NULL, download_thread, d) != 0) {
		set_error(tr("cannot_start_the_download"));
		download_end(d, false); // also releases the turn
		out[0] = '\0';
		return false;
	}
	pthread_detach(thread);
	return true;
}

// ---------------------------------------------------------------------------
// sidecar files: what the API knows and the audio file does not
// ---------------------------------------------------------------------------

void subsoniccache_write_sidecars(const char *track_id, const char *suffix, const char *title, const char *artist,
								  const char *album, const char *album_id, int track_number, const char *cover_url) {
	if (!subsoniccache_ready() || !ensure_dir()) {
		return;
	}

	char path[512];
	subsoniccache_path(track_id, suffix, path, sizeof(path));
	if (!path[0]) {
		return;
	}

	char tags[544];
	snprintf(tags, sizeof(tags), "%.500s.tags", path);

	// Atomic: a temporary file, then rename(). Anyone reading the metadata
	// meanwhile sees the old file or the new one, never half of one.
	char tags_tmp[560];
	snprintf(tags_tmp, sizeof(tags_tmp), "%s.tmp", tags);
	FILE *f = fopen(tags_tmp, "w");
	if (f) {
		fprintf(f, "title=%s\n", title ? title : "");
		fprintf(f, "artist=%s\n", artist ? artist : "");
		fprintf(f, "album=%s\n", album ? album : "");
		fprintf(f, "album_id=%s\n", album_id ? album_id : "");
		fprintf(f, "track=%d\n", track_number);
		fclose(f);
		if (rename(tags_tmp, tags) != 0) {
			remove(tags_tmp);
		}
	}

	if (!cover_url || !*cover_url) {
		return;
	}

	// The cover takes the track's name: the cover loader already looks for
	// "<track name>.jpg" next to the file.
	char cover[544];
	const char *dot = strrchr(path, '.');
	snprintf(cover, sizeof(cover), "%.*s.jpg", dot ? (int)(dot - path) : (int)strlen(path), path);

	struct stat st;
	if (stat(cover, &st) == 0 && st.st_size > 0) {
		return;
	}

	char *body = NULL;
	size_t len = 0;
	if (!http_get(cover_url, &body, &len, 4 * 1024 * 1024, FETCH_TIMEOUT_SECS) || !body) {
		return;
	}
	FILE *img = fopen(cover, "wb");
	if (img) {
		fwrite(body, 1, len, img);
		fclose(img);
	}
	free(body);
}

bool subsoniccache_album_id(const char *path, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!subsoniccache_owns(path)) {
		return false;
	}
	char tags[544];
	snprintf(tags, sizeof(tags), "%.500s.tags", path);
	FILE *f = fopen(tags, "r");
	if (!f) {
		return false;
	}
	char line[512];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		if (strncmp(line, "album_id=", 9) != 0) {
			continue;
		}
		char *value = line + 9;
		size_t len = strlen(value);
		while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r')) {
			value[--len] = '\0';
		}
		snprintf(out, size, "%s", value);
		found = out[0] != '\0';
	}
	fclose(f);
	return found;
}
