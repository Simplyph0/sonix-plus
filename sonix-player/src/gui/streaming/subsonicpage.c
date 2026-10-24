#include "subsonicpage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/azindex.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/subsoniccache.h"
#include "src/system/streaming/subsonicdb.h"

// ---------------------------------------------------------------------------
// Subsonic front end.
//
// The lists are made from the local copy of the server's songs (subsonicdb.h),
// not from requests: All songs, Artists, Albums, Genres, Starred and Recently
// added open at once, sorted as the library sorts, with the A-Z strip, and
// search is a query on the same copy. The network is used for the three things
// it is needed for: bringing the copy up to date, the server's playlists, and
// the music itself.
//
// Everything that talks to the network BLOCKS (see subsonic.h), so none of it
// runs on the GUI thread: a single worker with a single slot for the job to do,
// and results come back through gui_post(). Touching a second entry while the
// first is still loading means the second is what matters.
//
// A list of thousands of rows is windowed, as the file browser is: a fixed pool
// of row widgets is pointed at whichever rows are on screen, and the rows
// themselves are read from the database a window at a time.
// ---------------------------------------------------------------------------

// Server playlists are held whole: they are short, and the server does not page them.
#define MAX_HELD 300

// How many tracks to prefetch while one plays.
#define PREFETCH_AHEAD 2

// Tracks handed to the player when a row is tapped: the list from a little
// before the tapped row onwards. The whole of a twenty-thousand-song list is
// not a queue anyone plays through, and the player holds every entry.
#define QUEUE_MAX 400
#define QUEUE_BEFORE 40

// Of those, how many get their tag files at once. The rest get them when they
// are downloaded; writing four hundred files is a wait on a memory card.
#define QUEUE_TAGS_NOW 24

// Row geometry. The pitch is what the windowing maths is built on.
#define ROW_HEIGHT 88
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_POOL 10
#define ROW_PAD 16
#define PLAYMARK_WIDTH 6
#define PLAYMARK_HEIGHT 52

// The rows read from the database at a time.
#define WINDOW 48

lv_obj_t *subsonic_screen;
static gui_config_t *g_cfg;

// Where the card is: the real one, or on the simulator the folder standing in
// for it (the same choice main.c makes for the other caches).
static const char *card_root(void) {
	const char *root = storage_sd_root();
#ifdef HOST_BUILD
	// The simulator names /mnt/sd_0 whether or not it exists on this machine.
	if (!root || !root[0] || access(root, W_OK) != 0) {
		return g_cfg->sd_root_path;
	}
#endif
	return root && root[0] ? root : g_cfg->sd_root_path;
}
static lv_obj_t *login_screen;
lv_obj_t *subsonic_list_screen;
#define list_screen subsonic_list_screen
static lv_obj_t *search_screen;
static lv_obj_t *settings_screen;

// ---------------------------------------------------------------------------
// what the list is showing
// ---------------------------------------------------------------------------

typedef enum {
	VIEW_NONE = 0,
	VIEW_SONGS,		  // songs from the database
	VIEW_NAMES,		  // artists, albums or genres from the database
	VIEW_PLAYLISTS,	  // the server's playlists
	VIEW_PLAYLIST_SONGS, // one of them
} view_kind_t;

typedef struct {
	view_kind_t kind;
	sdb_filter_t filter;  // VIEW_SONGS
	sdb_names_t names;	  // VIEW_NAMES
	char value[SUBSONIC_ID_MAX]; // the filter's argument, or the playlist id
	char title[160];
	int scroll;			  // where the list was left, for the way back
} view_t;

static view_t view;		   // what is on screen
static int view_count;	   // rows in it

// A window of rows read ahead of the scroll position.
static subsonic_track_t win_tracks[WINDOW];
static sdb_name_t win_names[WINDOW];
static int win_first = -1;
static int win_n;

// The server's playlists, and the songs of one, held whole.
static subsonic_playlist_t held_playlists[MAX_HELD];
static subsonic_track_t held_tracks[MAX_HELD];
static int held_playlist_count;
static int held_track_count;

static bool view_has_songs(void) { return view.kind == VIEW_SONGS || view.kind == VIEW_PLAYLIST_SONGS; }

static void win_drop(void) {
	win_first = -1;
	win_n = 0;
}

// Reads the rows around `index` if they are not in the window already.
static bool win_ensure(int index) {
	if (index < 0 || index >= view_count) {
		return false;
	}
	if (win_first >= 0 && index >= win_first && index < win_first + win_n) {
		return true;
	}
	int first = index - ROW_POOL;
	if (first < 0) {
		first = 0;
	}
	int n = 0;
	switch (view.kind) {
	case VIEW_SONGS:
		n = sdb_songs(view.filter, view.value[0] ? view.value : NULL, first, WINDOW, win_tracks);
		break;
	case VIEW_NAMES:
		n = sdb_names(view.names, first, WINDOW, win_names);
		break;
	case VIEW_PLAYLIST_SONGS:
		for (; n < WINDOW && first + n < held_track_count; n++) {
			win_tracks[n] = held_tracks[first + n];
		}
		break;
	case VIEW_PLAYLISTS:
		n = 0; // read straight from held_playlists
		win_first = first;
		win_n = held_playlist_count - first < WINDOW ? held_playlist_count - first : WINDOW;
		return win_n > 0;
	default:
		break;
	}
	if (n <= 0) {
		win_drop();
		return false;
	}
	win_first = first;
	win_n = n;
	return index < first + n;
}

static const subsonic_track_t *track_row(int index) {
	return win_ensure(index) ? &win_tracks[index - win_first] : NULL;
}

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_LOGIN,
	JOB_PLAYLISTS,
	JOB_PLAYLIST_SONGS,
	JOB_PLAY,
	JOB_STAR,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char text[256];	  // a title
	char server[256]; // login: the server address
	char user[96];	  // login: the user name
	char password[128];
	char id[SUBSONIC_ID_MAX];
	int index; // JOB_PLAY: the row tapped
	bool on;   // JOB_STAR
	bool push; // a list job: open the result as a step forward
	view_t from; // JOB_PLAY: the list it was tapped in, as it was
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

// What the worker has to say about the last job.
static job_kind_t result_kind;
static char result_error[256];
static char result_title[160];

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	pending = *job;
	have_pending = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
}

// ---------------------------------------------------------------------------
// the busy veil: a spinner over a dimmed screen, for requests that lead to a
// list. Playback does not raise it: the track starts while it is still coming
// down, so there is no wait to show.
// ---------------------------------------------------------------------------

static lv_obj_t *busy_layer;

static void busy_hide(void) {
	if (busy_layer) {
		lv_obj_set_hidden(busy_layer, true);
	}
}

static void busy_show(void) {
	if (!busy_layer) {
		return;
	}
	lv_obj_set_hidden(busy_layer, false);
	lv_obj_move_foreground(busy_layer);
}

static void build_busy_layer(void) {
	busy_layer = lv_obj_create(lv_layer_sys());
	lv_obj_set_size(busy_layer, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(busy_layer, 0, 0);
	lv_obj_set_style_bg_color(busy_layer, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(busy_layer, LV_OPA_50, 0);
	lv_obj_set_style_border_width(busy_layer, 0, 0);
	lv_obj_set_style_radius(busy_layer, 0, 0);
	lv_obj_set_scrollable(busy_layer, false);
	lv_obj_set_hidden(busy_layer, true);
	lv_obj_set_flex_flow(busy_layer, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(busy_layer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *spin = spinner_create(busy_layer, &icon_loader_big);
	lv_obj_set_style_image_recolor(spin, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(spin, LV_OPA_COVER, 0);
}

static void run(const job_t *job);

// ---------------------------------------------------------------------------
// playback
//
// The tapped track downloads and starts by itself. The ones after it, in the
// same list, download while it plays: a list plays straight through without
// waiting for any of it to land first.
//
// Any file in the cache is looked up in the database by the key its name
// carries, so the same machinery serves a queue made here, a favourite, a saved
// playlist and the track menu's "Add to queue": whatever puts one of these paths
// in the queue, the player comes back to this to have it downloaded.
// ---------------------------------------------------------------------------

static void slow_start_async(void *user) {
	int seconds = (int)(intptr_t)user;
	char message[128];
	snprintf(message, sizeof(message), tr("stream_slow_connection"), seconds);
	gui_notify_popup(message);
}

static void on_slow_start(int seconds) { gui_post(slow_start_async, (void *)(intptr_t)seconds); }

// Set when playback stopped on a track that had not arrived yet: when the file
// lands, it resumes.
static bool waiting_for_track;

// The extension of a cache path, without the dot.
static const char *path_suffix(const char *path) {
	const char *dot = strrchr(path, '.');
	return dot ? dot + 1 : "";
}

// True when the file can already be played: fully downloaded, or growing right
// now (the decoder reads a growing file, which is how the first note plays
// without waiting for the end).
static bool track_is_playable(const char *path) {
	char key[SUBSONICCACHE_KEY_MAX];
	if (!subsoniccache_key_from_path(path, key, sizeof(key))) {
		return true; // not ours
	}
	return subsoniccache_has(key, path_suffix(path), NULL, 0) || growfile_is_growing(path) ||
		   subsoniccache_downloading(key);
}

static void prefetch_urgent(const char *wanted_path);

static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup("downloading");
}

// Called by device_state when a file is about to start, from whatever thread it
// happens to be on. The notice goes through gui_post: LVGL is not reentrant.
static bool subsonic_prepare_track(const char *path) {
	if (!subsoniccache_owns(path)) {
		waiting_for_track = false;
		return true; // not a Subsonic file
	}
	if (track_is_playable(path)) {
		waiting_for_track = false;
		return true;
	}
	// Not there. Whatever was playing is stopped FIRST: it is reading a file
	// that is still downloading, and one of those prefetch_urgent() is about to
	// abandon. Pulling the file from under the audio thread would make it report
	// "track finished" and the queue would advance onto another missing track.
	fprintf(stderr, "subsonic: '%s' not ready: stopping and requesting it\n", path);
	subsoniccache_set_network_wanted(true);
	waiting_for_track = true;
	device_state_stop();
	prefetch_urgent(path);
	gui_post(downloading_note_async, NULL);
	return false;
}

static void resume_if_waiting_async(void *user) {
	(void)user;
	if (!waiting_for_track || wifitransfer_get_enabled()) {
		return;
	}
	int index = playlist_current_index();
	char path[512];
	if (index < 0 || !playlist_path_at(index, path, sizeof(path))) {
		return;
	}
	if (subsoniccache_owns(path) && !track_is_playable(path)) {
		return; // a different track arrived, not this one
	}
	waiting_for_track = false;
	device_state_play_queue_index(index);
	player_refresh_now_playing();
}

static void cover_ready_async(void *user) {
	char *path = user;
	device_state_t state;
	device_state_get(&state);
	if (path && strcmp(path, state.current_file) == 0) {
		player_refresh_now_playing();
	}
	free(path);
}

static void play_started_async(void *user) {
	(void)user;
	player_refresh_now_playing();
	switch_screen(player_screen);
}

// ---------------------------------------------------------------------------
// prefetching, on a thread of its own
//
// Not on the worker that serves taps: prefetching sits waiting for the download
// slot for the whole of a track, and for all that time tapping another entry
// would do nothing. This thread checks the wanted key before each step, so as
// soon as another track is started it notices it is no longer needed.
// ---------------------------------------------------------------------------

static pthread_mutex_t prefetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_ready = PTHREAD_COND_INITIALIZER;

// Which queued track must be downloaded now, by key. Empty = nothing to do.
// Written by the queue watchdog on the GUI thread, the only one that knows where
// playback is. A key and not a position: with shuffle the play queue has an
// order of its own.
static char prefetch_want_key[SUBSONICCACHE_KEY_MAX];
static bool prefetch_started;

// The last track that did NOT download, and when it was tried, so a server that
// says no is not asked sixty times a minute.
static char prefetch_failed_key[SUBSONICCACHE_KEY_MAX];
static uint32_t prefetch_failed_ms;
#define PREFETCH_RETRY_MS 4000

static void *prefetch_main(void *arg);

static void prefetch_submit(void) {
	pthread_mutex_lock(&prefetch_lock);
	if (!prefetch_started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, prefetch_main, NULL) == 0) {
			pthread_detach(thread);
			prefetch_started = true;
		}
	}
	pthread_cond_broadcast(&prefetch_ready);
	pthread_mutex_unlock(&prefetch_lock);
}

// The track needed NOW -- prev was pressed, or the queue reached one that was
// not prefetched. The running download is abandoned (only one runs at a time),
// unless it is already the wanted one.
static void prefetch_urgent(const char *wanted_path) {
	char key[SUBSONICCACHE_KEY_MAX];
	if (!subsoniccache_key_from_path(wanted_path ? wanted_path : "", key, sizeof(key))) {
		return;
	}
	if (!subsoniccache_downloading(key)) {
		streamturn_abandon_all();
	}
	subsoniccache_set_network_wanted(true);

	pthread_mutex_lock(&prefetch_lock);
	snprintf(prefetch_want_key, sizeof(prefetch_want_key), "%s", key);
	pthread_cond_broadcast(&prefetch_ready);
	pthread_mutex_unlock(&prefetch_lock);
	prefetch_submit();
}

static void write_track_sidecars(const subsonic_track_t *t, bool with_cover) {
	char cover[SUBSONIC_URL_MAX] = "";
	if (with_cover && t->cover_id[0]) {
		subsonic_cover_url(t->cover_id, 600, cover, sizeof(cover));
	}
	subsoniccache_write_sidecars(t, cover);
}

// Downloads one track while another plays: address, download, and the sidecar
// files (tags and cover) so everything is ready by its turn.
static bool prepare_track(const subsonic_track_t *track, char *path, size_t size) {
	char url[SUBSONIC_URL_MAX];
	if (!subsonic_stream_url(track->id, url, sizeof(url))) {
		return false;
	}
	if (!subsoniccache_start(track->id, url, track->suffix, track->duration, path, size)) {
		return false;
	}
	write_track_sidecars(track, true);
	subsoniccache_note_played(path);

	char *owned = strdup(path);
	if (owned) {
		gui_post(cover_ready_async, owned);
	}
	return true;
}

// The tracks of a list as the queue: from a little before `index`.
static int queue_tracks(const view_t *from, int index, subsonic_track_t *out, int *start_out) {
	int first = index - QUEUE_BEFORE;
	if (first < 0) {
		first = 0;
	}
	int n = 0;
	if (from->kind == VIEW_SONGS) {
		n = sdb_songs(from->filter, from->value[0] ? from->value : NULL, first, QUEUE_MAX, out);
	} else if (from->kind == VIEW_PLAYLIST_SONGS) {
		for (; n < QUEUE_MAX && first + n < held_track_count; n++) {
			out[n] = held_tracks[first + n];
		}
	}
	*start_out = index - first;
	return n;
}

static void do_play(const job_t *job) {
	subsonic_track_t *tracks = malloc(sizeof(*tracks) * QUEUE_MAX);
	char (*paths)[512] = malloc(sizeof(*paths) * QUEUE_MAX);
	const char **path_ptr = malloc(sizeof(*path_ptr) * QUEUE_MAX);
	if (!tracks || !paths || !path_ptr) {
		snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
		goto done;
	}

	int start = 0;
	int n = queue_tracks(&job->from, job->index, tracks, &start);
	if (n <= 0 || start >= n) {
		goto done;
	}
	// A list the database did not give (a server playlist) is put in it, so
	// that every path in the queue can be looked up again.
	if (job->from.kind == VIEW_PLAYLIST_SONGS) {
		sdb_remember(tracks, n);
	}
	const subsonic_track_t *track = &tracks[start];

	// Whatever was downloading for another track is no longer needed.
	streamturn_abandon_all();

	char url[SUBSONIC_URL_MAX];
	if (!subsonic_stream_url(track->id, url, sizeof(url))) {
		snprintf(result_error, sizeof(result_error), "%s", tr("subsonic_no_track_address"));
		goto done;
	}

	char path[512];
	if (!subsoniccache_start(track->id, url, track->suffix, track->duration, path, sizeof(path))) {
		const char *why = subsoniccache_last_error();
		snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr("download_failed"));
		goto done;
	}
	subsoniccache_note_played(path);

	// Tags BEFORE starting playback: that is when device_state reads the metadata.
	write_track_sidecars(track, false);

	// The queue is the whole list, not only what has downloaded: every path is
	// known before the track is, since the file is named after the id and the
	// suffix the server reported.
	for (int i = 0; i < n; i++) {
		if (i == start) {
			snprintf(paths[i], sizeof(paths[0]), "%s", path);
		} else {
			subsoniccache_path(tracks[i].id, tracks[i].suffix, paths[i], sizeof(paths[0]));
		}
		path_ptr[i] = paths[i];
	}

	// Tags for the first of the rest at once, so the queue page names them.
	for (int i = start + 1; i < n && i < start + 1 + QUEUE_TAGS_NOW; i++) {
		write_track_sidecars(&tracks[i], false);
	}

	device_state_play_list(path_ptr, n, start);

	// The cover last, with the music already playing: it is another download.
	write_track_sidecars(track, true);

	gui_post(play_started_async, NULL);
	prefetch_submit();

done:
	free(tracks);
	free(paths);
	free(path_ptr);
}

// The queue watchdog, once a second on the GUI thread. It tells the cache which
// files it must not evict (those still to be played) and the prefetch thread
// WHICH track to download now: the first missing one between the current track
// and the next few. When playback has reached a track that is not there yet,
// that is exactly the one being waited for.
static void queue_watch_cb(lv_timer_t *timer) {
	(void)timer;
	int count = playlist_count();
	int current = playlist_current_index();

	// The queue of a library list is read from the database one entry at a time,
	// so only a short window is looked at -- and only when it has any of ours.
	static char window[SUBSONICCACHE_PROTECTED_MAX][512];
	static const char *window_ptr[SUBSONICCACHE_PROTECTED_MAX];
	int protect = 0;
	bool ours = false;
	char want[SUBSONICCACHE_KEY_MAX] = "";
	bool transfer_on = wifitransfer_get_enabled();

	for (int i = current; i >= 0 && i < count && i <= current + PREFETCH_AHEAD; i++) {
		if (!playlist_path_at(i, window[protect], sizeof(window[0]))) {
			continue;
		}
		if (!subsoniccache_owns(window[protect])) {
			continue;
		}
		ours = true;
		window_ptr[protect] = window[protect];
		char key[SUBSONICCACHE_KEY_MAX];
		if (!want[0] && !transfer_on && subsoniccache_key_from_path(window[protect], key, sizeof(key)) &&
			!subsoniccache_has(key, path_suffix(window[protect]), NULL, 0)) {
			snprintf(want, sizeof(want), "%s", key);
		}
		protect++;
	}
	// And the rest of what is lined up, kept from the sweep: only walked when
	// the track playing is one of ours.
	if (ours) {
		for (int i = current + PREFETCH_AHEAD + 1; i < count && protect < SUBSONICCACHE_PROTECTED_MAX; i++) {
			if (!playlist_path_at(i, window[protect], sizeof(window[0]))) {
				continue;
			}
			if (subsoniccache_owns(window[protect])) {
				window_ptr[protect] = window[protect];
				protect++;
			}
		}
	}
	subsoniccache_set_protected(window_ptr, protect);

	if (transfer_on) {
		waiting_for_track = false;
	}

	pthread_mutex_lock(&prefetch_lock);
	if (strcmp(want, prefetch_want_key) != 0) {
		snprintf(prefetch_want_key, sizeof(prefetch_want_key), "%s", want);
		pthread_cond_broadcast(&prefetch_ready);
	}
	pthread_mutex_unlock(&prefetch_lock);
	if (want[0]) {
		prefetch_submit();
	}

	// The wait only holds while the queue really points at a Subsonic track.
	if (waiting_for_track) {
		char wanted[512];
		if (current >= 0 && playlist_path_at(current, wanted, sizeof(wanted)) && !subsoniccache_owns(wanted)) {
			waiting_for_track = false;
		}
	}

	subsoniccache_set_network_wanted(want[0] != '\0' || waiting_for_track);
}

static void *prefetch_main(void *arg) {
	(void)arg;
	thread_be_background("subsonic prefetch");

	for (;;) {
		pthread_mutex_lock(&prefetch_lock);
		while (prefetch_want_key[0] == '\0') {
			pthread_cond_wait(&prefetch_ready, &prefetch_lock);
		}
		char key[SUBSONICCACHE_KEY_MAX];
		snprintf(key, sizeof(key), "%s", prefetch_want_key);
		pthread_mutex_unlock(&prefetch_lock);

		if (strcmp(key, prefetch_failed_key) == 0 && lv_tick_get() - prefetch_failed_ms < PREFETCH_RETRY_MS) {
			usleep(300 * 1000);
			continue;
		}

		subsonic_track_t track;
		if (!sdb_find(key, &track)) {
			// A song the database does not know: nothing can be asked of the
			// server for it. Waited out, in case a sync is about to bring it.
			snprintf(prefetch_failed_key, sizeof(prefetch_failed_key), "%s", key);
			prefetch_failed_ms = lv_tick_get();
			pthread_mutex_lock(&prefetch_lock);
			if (strcmp(prefetch_want_key, key) == 0) {
				prefetch_want_key[0] = '\0';
			}
			pthread_mutex_unlock(&prefetch_lock);
			continue;
		}

		subsoniccache_wait_idle();

		// While waiting for the slot the request may have changed.
		pthread_mutex_lock(&prefetch_lock);
		bool moved_on = strcmp(prefetch_want_key, key) != 0;
		pthread_mutex_unlock(&prefetch_lock);
		if (moved_on) {
			continue;
		}

		char next_path[512];
		bool ok = prepare_track(&track, next_path, sizeof(next_path));
		if (!ok) {
			fprintf(stderr, "subsonic: %s does not download: %s\n", key, subsoniccache_last_error());
			snprintf(prefetch_failed_key, sizeof(prefetch_failed_key), "%s", key);
			prefetch_failed_ms = lv_tick_get();
		} else if (strcmp(prefetch_failed_key, key) == 0) {
			prefetch_failed_key[0] = '\0';
		}

		// However it went, this request has been served: the watchdog recomputes
		// on its next pass.
		pthread_mutex_lock(&prefetch_lock);
		if (strcmp(prefetch_want_key, key) == 0) {
			prefetch_want_key[0] = '\0';
		}
		bool superseded = prefetch_want_key[0] != '\0';
		pthread_mutex_unlock(&prefetch_lock);

		// Belt as well as braces: if the request changed while the download was
		// already starting, what was just started is a zombie nobody else will stop.
		if (ok && superseded && subsoniccache_downloading(key)) {
			subsoniccache_abandon_all();
		}
		if (ok && !superseded) {
			gui_post(resume_if_waiting_async, NULL);
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the list
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *button;
	lv_obj_t *title;
	lv_obj_t *sub;
	lv_obj_t *star;
	lv_obj_t *playmark;
	int index; // the row of the list this widget shows, -1 for none
} row_t;

static row_t rows[ROW_POOL];
static lv_obj_t *list_viewport;
static lv_obj_t *list_body;
static lv_obj_t *list_title;
static lv_obj_t *list_empty;
static azindex_t *az;

// The key of the file playing, for the mark on its row. Kept by the timer.
static char np_key[SUBSONICCACHE_KEY_MAX];

static void rebuild_home(void);
static void open_view(const view_t *next, bool push);

static void duration_text(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

// "FLAC 24/96 · 5:12", or "MP3 320 kbps · 3:42" when the server says what it
// has, and the length alone when it does not; the artist first.
static void track_detail(const subsonic_track_t *t, char *out, size_t size) {
	char length[16];
	duration_text(t->duration, length, sizeof(length));

	char format[48] = "";
	char suffix[8];
	snprintf(suffix, sizeof(suffix), "%s", t->suffix);
	for (char *p = suffix; *p; p++) {
		if (*p >= 'a' && *p <= 'z') {
			*p = (char)(*p - 'a' + 'A');
		}
	}
	if (t->bit_depth > 0 && t->sample_rate > 0) {
		if (t->sample_rate % 1000 == 0) {
			snprintf(format, sizeof(format), "%s %d/%d", suffix, t->bit_depth, t->sample_rate / 1000);
		} else {
			snprintf(format, sizeof(format), "%s %d/%d.%d", suffix, t->bit_depth, t->sample_rate / 1000,
					 (t->sample_rate % 1000) / 100);
		}
	} else if (t->bitrate > 0) {
		snprintf(format, sizeof(format), "%s %d kbps", suffix, t->bitrate);
	}

	snprintf(out, size, "%s%s%s%s%s", t->artist, t->artist[0] && format[0] ? " \xC2\xB7 " : "", format,
			 (t->artist[0] || format[0]) && length[0] ? " \xC2\xB7 " : "", length);
}

static bool row_is_playing(int index) {
	if (!np_key[0] || !view_has_songs()) {
		return false;
	}
	const subsonic_track_t *t = track_row(index);
	if (!t) {
		return false;
	}
	char key[SUBSONICCACHE_KEY_MAX];
	subsoniccache_key(t->id, key, sizeof(key));
	return strcmp(key, np_key) == 0;
}

static void row_bind(row_t *row, int index) {
	if (index < 0 || index >= view_count || !win_ensure(index)) {
		row->index = -1;
		lv_obj_set_hidden(row->button, true);
		return;
	}

	char detail[400] = "";
	const char *title = "";
	bool starred = false;
	switch (view.kind) {
	case VIEW_SONGS:
	case VIEW_PLAYLIST_SONGS: {
		const subsonic_track_t *t = &win_tracks[index - win_first];
		title = t->title;
		track_detail(t, detail, sizeof(detail));
		starred = t->starred;
		break;
	}
	case VIEW_NAMES: {
		const sdb_name_t *n = &win_names[index - win_first];
		title = n->name;
		snprintf(detail, sizeof(detail), "%s%s%d %s", n->sub, n->sub[0] ? " \xC2\xB7 " : "", n->count,
				 n->count == 1 ? tr("subsonic_song") : tr("subsonic_songs"));
		break;
	}
	case VIEW_PLAYLISTS: {
		const subsonic_playlist_t *p = &held_playlists[index];
		title = p->name;
		snprintf(detail, sizeof(detail), "%s%s%d", p->owner, p->owner[0] ? " \xC2\xB7 " : "", p->track_count);
		break;
	}
	default:
		break;
	}

	row->index = index;
	lv_obj_set_y(row->button, index * ROW_PITCH);
	lv_obj_set_hidden(row->button, false);
	lv_label_set_text(row->title, title);
	lv_label_set_text(row->sub, detail);
	lv_obj_set_hidden(row->sub, detail[0] == '\0');
	lv_obj_set_hidden(row->star, !starred);
	lv_obj_set_hidden(row->playmark, !row_is_playing(index));
}

// Assigns the row widgets to the band of entries around the scroll position:
// quantise the scroll offset to the row pitch, start one row early, and cover
// the viewport from there. Entry `index` always lives in widget `index %
// ROW_POOL`, so when the band slides down by one the other nine rows are left
// alone.
static void window_update(void) {
	if (view_count == 0) {
		for (int i = 0; i < ROW_POOL; i++) {
			row_bind(&rows[i], -1);
		}
		return;
	}
	int scroll = lv_obj_get_scroll_y(list_viewport);
	if (scroll < 0) {
		scroll = 0;
	}
	int first = (scroll / ROW_PITCH) - 1;
	if (first + ROW_POOL > view_count) {
		first = view_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&rows[index % ROW_POOL], index < view_count ? index : -1);
	}
}

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	window_update();
	if (az) {
		azindex_flash(az);
	}
}

// The strip's jump moved the list: the rows have to be bound again.
static void az_jumped(void) { window_update(); }

// ---------------------------------------------------------------------------
// going from list to list
//
// Every list lives on ONE reused page, so the screen manager has nothing to
// stack: opening an album from the artists and then going back would skip the
// artists. The page keeps its own history -- a stack of views, with where each
// was scrolled to.
// ---------------------------------------------------------------------------

#define HISTORY_DEPTH 8
static view_t history[HISTORY_DEPTH];
static int history_len;

static bool list_back_has_step(void) { return history_len > 0; }

static void count_view(void) {
	switch (view.kind) {
	case VIEW_SONGS:
		view_count = sdb_song_count(view.filter, view.value[0] ? view.value : NULL);
		break;
	case VIEW_NAMES:
		view_count = sdb_name_count(view.names);
		break;
	case VIEW_PLAYLISTS:
		view_count = held_playlist_count;
		break;
	case VIEW_PLAYLIST_SONGS:
		view_count = held_track_count;
		break;
	default:
		view_count = 0;
		break;
	}
	if (view_count < 0) {
		view_count = 0;
	}
}

// The strip for the list on screen: only for those sorted by title or name.
static void set_strip(void) {
	if (!az) {
		return;
	}
	int first[LIBRARY_INDEX_BUCKETS];
	bool have = false;
	if (view.kind == VIEW_SONGS && (view.filter == SDB_SONGS_ALL || view.filter == SDB_SONGS_GENRE ||
									view.filter == SDB_SONGS_STARRED)) {
		have = sdb_song_buckets(view.filter, view.value[0] ? view.value : NULL, first);
	} else if (view.kind == VIEW_NAMES) {
		have = sdb_name_buckets(view.names, first);
	}
	azindex_set_rows(az, have ? first : NULL, view_count, false);
}

// Counts the rows again, keeping the place: after a sync, or a star.
static void view_refresh(void) {
	if (view.kind == VIEW_NONE) {
		return;
	}
	int scroll = lv_obj_get_scroll_y(list_viewport);
	win_drop();
	count_view();
	lv_obj_set_height(list_body, view_count ? view_count * ROW_PITCH : ROW_PITCH);
	lv_obj_scroll_to_y(list_viewport, scroll, LV_ANIM_OFF);
	lv_obj_set_hidden(list_empty, view_count != 0);
	set_strip();
	window_update();
}

static void open_view(const view_t *next, bool push) {
	if (push && view.kind != VIEW_NONE && lv_screen_active() == list_screen) {
		if (history_len == HISTORY_DEPTH) {
			memmove(&history[0], &history[1], sizeof(history[0]) * (HISTORY_DEPTH - 1));
			history_len--;
		}
		history[history_len] = view;
		history[history_len].scroll = lv_obj_get_scroll_y(list_viewport);
		history_len++;
	} else if (lv_screen_active() != list_screen) {
		history_len = 0; // entering from outside: the history restarts
	}

	view = *next;
	win_drop();
	count_view();

	lv_label_set_text(list_title, view.title);
	lv_obj_set_height(list_body, view_count ? view_count * ROW_PITCH : ROW_PITCH);
	int max_scroll = view_count * ROW_PITCH - lv_obj_get_height(list_viewport);
	int target = view.scroll;
	if (target > max_scroll) {
		target = max_scroll > 0 ? max_scroll : 0;
	}
	lv_obj_scroll_to_y(list_viewport, target < 0 ? 0 : target, LV_ANIM_OFF);

	lv_label_set_text(list_empty, tr(view.kind == VIEW_SONGS && view.filter == SDB_SONGS_SEARCH ? "no_results"
															: "subsonic_empty_library"));
	lv_obj_set_hidden(list_empty, view_count != 0);
	set_strip();
	window_update();

	if (lv_screen_active() != list_screen) {
		switch_screen(list_screen);
	}
}

static bool list_back_guard(void) {
	if (history_len == 0) {
		view.kind = VIEW_NONE;
		return false; // nothing underneath: leave the page
	}
	view_t previous = history[--history_len];
	if (previous.kind == VIEW_PLAYLISTS || previous.kind == VIEW_PLAYLIST_SONGS) {
		// Held only while on screen: read again.
		job_t job = {0};
		job.kind = previous.kind == VIEW_PLAYLISTS ? JOB_PLAYLISTS : JOB_PLAYLIST_SONGS;
		snprintf(job.id, sizeof(job.id), "%s", previous.value);
		snprintf(job.text, sizeof(job.text), "%s", previous.title);
		job.from = previous;
		busy_show();
			run(&job);
		return true;
	}
	open_view(&previous, false);
	return true;
}

// ---------------------------------------------------------------------------
// taps
// ---------------------------------------------------------------------------

static void open_songs(sdb_filter_t filter, const char *value, const char *title) {
	view_t next = {0};
	next.kind = VIEW_SONGS;
	next.filter = filter;
	snprintf(next.value, sizeof(next.value), "%s", value ? value : "");
	snprintf(next.title, sizeof(next.title), "%s", title);
	open_view(&next, true);
}

bool subsonicpage_open_album(const char *album_id, const char *title) {
	if (!album_id || !album_id[0] || !subsonic_logged_in()) {
		return false;
	}
	history_len = 0;
	view.kind = VIEW_NONE;
	open_songs(SDB_SONGS_ALBUM, album_id, title && title[0] ? title : tr("subsonic"));
	return true;
}

static void open_names(sdb_names_t kind, const char *title) {
	view_t next = {0};
	next.kind = VIEW_NAMES;
	next.names = kind;
	snprintf(next.title, sizeof(next.title), "%s", title);
	open_view(&next, true);
}

static row_t *row_of_button(lv_obj_t *button) {
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button == button) {
			return &rows[i];
		}
	}
	return NULL;
}

static void row_clicked_cb(lv_event_t *e) {
	// A gesture that ends over a row is not a tap on the row: events bubble, so
	// the click that closes a drag arrives here too.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	row_t *row = row_of_button(lv_event_get_current_target(e));
	if (!row || row->index < 0 || row->index >= view_count || !win_ensure(row->index)) {
		return;
	}
	int index = row->index;

	switch (view.kind) {
	case VIEW_SONGS:
	case VIEW_PLAYLIST_SONGS: {
		job_t job = {0};
		job.kind = JOB_PLAY;
		job.index = index;
		job.from = view;
		run(&job);
		return;
	}
	case VIEW_NAMES: {
		const sdb_name_t *n = &win_names[index - win_first];
		sdb_filter_t filter = view.names == SDB_NAMES_ARTISTS ? SDB_SONGS_ARTIST
							  : view.names == SDB_NAMES_ALBUMS ? SDB_SONGS_ALBUM
															   : SDB_SONGS_GENRE;
		char value[SUBSONIC_ID_MAX], title[SUBSONIC_TITLE_MAX];
		snprintf(value, sizeof(value), "%s", n->value);
		snprintf(title, sizeof(title), "%s", n->name);
		open_songs(filter, value, title);
		return;
	}
	case VIEW_PLAYLISTS: {
		job_t job = {0};
		job.kind = JOB_PLAYLIST_SONGS;
		snprintf(job.id, sizeof(job.id), "%s", held_playlists[index].id);
		snprintf(job.text, sizeof(job.text), "%s", held_playlists[index].name);
		run(&job);
		return;
	}
	default:
		return;
	}
}

// ---------------------------------------------------------------------------
// the row menu: what a long press on a song offers
// ---------------------------------------------------------------------------

static subsonic_track_t menu_track;
static popover_item_t menu_items[6];

static void menu_path(char *out, size_t size) { subsoniccache_path(menu_track.id, menu_track.suffix, out, size); }

// A path that goes to the queue or a playlist is read for its name by the
// player at once, so the tags are written first.
static bool menu_prepare(char *path, size_t size) {
	if (!subsoniccache_ready()) {
		gui_notify_popup("download_no_card");
		return false;
	}
	write_track_sidecars(&menu_track, false);
	menu_path(path, size);
	return path[0] != '\0';
}

static void menu_enqueue(void *user) {
	(void)user;
	char path[512];
	if (menu_prepare(path, sizeof(path)) && playlist_enqueue(path)) {
		toast_success("added_to_the_queue");
	}
}

static void menu_next(void *user) {
	(void)user;
	char path[512];
	if (menu_prepare(path, sizeof(path)) && playlist_insert_next(path)) {
		toast_success("added_to_the_queue");
	}
}

static void menu_playlist(void *user) {
	(void)user;
	char path[512];
	if (menu_prepare(path, sizeof(path))) {
		playlistpage_add_track(path);
	}
}

static void menu_star(void *user) {
	(void)user;
	job_t job = {0};
	job.kind = JOB_STAR;
	snprintf(job.id, sizeof(job.id), "%s", menu_track.id);
	job.on = !menu_track.starred;
	run(&job);
}

static void menu_album(void *user) {
	(void)user;
	if (menu_track.album_id[0]) {
		open_songs(SDB_SONGS_ALBUM, menu_track.album_id, menu_track.album);
	}
}

static void menu_artist(void *user) {
	(void)user;
	if (menu_track.artist[0]) {
		open_songs(SDB_SONGS_ARTIST, menu_track.artist, menu_track.artist);
	}
}

static void row_long_pressed_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active() || !view_has_songs()) {
		return;
	}
	row_t *row = row_of_button(lv_event_get_current_target(e));
	if (!row || row->index < 0 || !win_ensure(row->index)) {
		return;
	}
	// The lift that ends this press would otherwise arrive as a click and play
	// the row.
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}

	menu_track = win_tracks[row->index - win_first];
	int n = 0;
	menu_items[n++] = (popover_item_t){tr("add_to_queue"), menu_enqueue, NULL, false};
	menu_items[n++] = (popover_item_t){tr("subsonic_play_next"), menu_next, NULL, false};
	menu_items[n++] = (popover_item_t){tr(menu_track.starred ? "remove_from_favourites" : "medialist_add_to_favourites"),
									   menu_star, NULL, false};
	menu_items[n++] = (popover_item_t){tr("add_to_playlist"), menu_playlist, NULL, false};
	if (menu_track.album_id[0] && !(view.kind == VIEW_SONGS && view.filter == SDB_SONGS_ALBUM)) {
		menu_items[n++] = (popover_item_t){tr("show_album"), menu_album, NULL, false};
	}
	if (menu_track.artist[0] && !(view.kind == VIEW_SONGS && view.filter == SDB_SONGS_ARTIST)) {
		menu_items[n++] = (popover_item_t){tr("subsonic_show_artist"), menu_artist, NULL, false};
	}
	popover_show(row->button, menu_items, n);
}

// ---------------------------------------------------------------------------
// building the rows
// ---------------------------------------------------------------------------

static lv_style_t style_row;

static void build_rows(int width) {
	lv_style_init(&style_row);
	lv_style_set_border_width(&style_row, 0);
	lv_style_set_shadow_width(&style_row, 0);
	lv_style_set_radius(&style_row, 12);
	lv_style_set_pad_hor(&style_row, ROW_PAD);
	lv_style_set_pad_ver(&style_row, 0);

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];
		row->index = -1;

		row->button = lv_btn_create(list_body);
		lv_obj_set_size(row->button, width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &style_row, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_scrollable(row->button, false);
		lv_obj_set_event_bubble(row->button, true); // so a swipe can start on a row
		lv_obj_add_event_cb(row->button, row_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_add_event_cb(row->button, row_long_pressed_cb, LV_EVENT_LONG_PRESSED, NULL);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(row->button, 10, 0);

		// The name over the detail: two labels, each exactly one line high, in a
		// column that takes the room the star leaves.
		lv_obj_t *texts = lv_obj_create(row->button);
		lv_obj_set_flex_grow(texts, 1);
		lv_obj_set_height(texts, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(texts, 0, 0);
		lv_obj_set_style_border_width(texts, 0, 0);
		lv_obj_set_style_pad_all(texts, 0, 0);
		lv_obj_set_style_pad_row(texts, 6, 0);
		lv_obj_set_scrollable(texts, false);
		lv_obj_set_event_bubble(texts, true);
		lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

		row->title = lv_label_create(texts);
		lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->title, lv_pct(100));
		lv_obj_set_height(row->title, lv_font_get_line_height(&font_ui_22));
		lv_obj_add_style(row->title, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->title, &font_ui_22, 0);

		row->sub = lv_label_create(texts);
		lv_label_set_long_mode(row->sub, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->sub, lv_pct(100));
		lv_obj_set_height(row->sub, lv_font_get_line_height(&font_ui_18));
		lv_obj_add_style(row->sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->sub, &font_ui_18, 0);

		row->star = lv_image_create(row->button);
		lv_image_set_src(row->star, &icon_star_filled);
		lv_obj_set_style_image_recolor(row->star, lv_color_make(246, 211, 45), 0); // as the player's
		lv_obj_set_style_image_recolor_opa(row->star, LV_OPA_COVER, 0);
		lv_obj_set_clickable(row->star, false);
		lv_obj_set_hidden(row->star, true);

		// Outside the flex layout, in the row's own left padding: marks the row
		// that is playing without moving anything on it.
		row->playmark = lv_obj_create(row->button);
		lv_obj_set_ignore_layout(row->playmark, true);
		lv_obj_set_size(row->playmark, PLAYMARK_WIDTH, PLAYMARK_HEIGHT);
		lv_obj_align(row->playmark, LV_ALIGN_LEFT_MID, 4 - ROW_PAD, 0);
		lv_obj_add_style(row->playmark, &theme_style_accent_bg, 0);
		lv_obj_set_style_radius(row->playmark, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(row->playmark, 0, 0);
		lv_obj_set_style_shadow_width(row->playmark, 0, 0);
		lv_obj_set_style_pad_all(row->playmark, 0, 0);
		lv_obj_set_scrollable(row->playmark, false);
		lv_obj_set_clickable(row->playmark, false);
		lv_obj_set_hidden(row->playmark, true);
	}
}

// What is playing, kept up to date for the marks: a second is plenty.
static lv_timer_t *mark_timer;

static void mark_tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != list_screen) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	char key[SUBSONICCACHE_KEY_MAX] = "";
	subsoniccache_key_from_path(state.current_file, key, sizeof(key));
	if (strcmp(key, np_key) != 0) {
		snprintf(np_key, sizeof(np_key), "%s", key);
		window_update();
	}
}

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	if (mark_timer) {
		lv_timer_resume(mark_timer);
	}
	// A sync, or a star, may have changed what the list holds.
	if (view_has_songs() || view.kind == VIEW_NAMES) {
		view_refresh();
	}
}

static void list_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (mark_timer) {
		lv_timer_pause(mark_timer);
	}
}

// ---------------------------------------------------------------------------
// the worker's main loop
// ---------------------------------------------------------------------------

static bool result_push;
static view_t result_view;

static void job_done_cb(void *user);

static void *worker_main(void *arg) {
	(void)arg;

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		result_error[0] = '\0';
		result_kind = job.kind;
		result_push = job.push;
		memset(&result_view, 0, sizeof(result_view));

		switch (job.kind) {
		case JOB_LOGIN:
			if (!subsonic_login(job.server, job.user, job.password)) {
				snprintf(result_error, sizeof(result_error), "%s", subsonic_last_error());
			}
			memset(job.password, 0, sizeof(job.password));
			break;

		case JOB_PLAYLISTS: {
			int n = subsonic_playlists(0, held_playlists, MAX_HELD);
			if (n < 0) {
				snprintf(result_error, sizeof(result_error), "%s", subsonic_last_error());
				break;
			}
			held_playlist_count = n;
			result_view.kind = VIEW_PLAYLISTS;
			snprintf(result_view.title, sizeof(result_view.title), "%s", tr("playlists"));
			break;
		}

		case JOB_PLAYLIST_SONGS: {
			int n = subsonic_playlist_tracks(job.id, 0, held_tracks, MAX_HELD);
			if (n < 0) {
				snprintf(result_error, sizeof(result_error), "%s", subsonic_last_error());
				break;
			}
			held_track_count = n;
			sdb_remember(held_tracks, n);
			result_view.kind = VIEW_PLAYLIST_SONGS;
			snprintf(result_view.value, sizeof(result_view.value), "%s", job.id);
			snprintf(result_view.title, sizeof(result_view.title), "%s", job.text);
			break;
		}

		case JOB_PLAY:
			do_play(&job);
			break;

		case JOB_STAR:
			if (subsonic_star(job.id, job.on)) {
				char key[SUBSONICCACHE_KEY_MAX];
				subsoniccache_key(job.id, key, sizeof(key));
				sdb_set_starred(key, job.on);
			} else {
				snprintf(result_error, sizeof(result_error), "%s", subsonic_last_error());
			}
			break;

		default:
			break;
		}

		gui_post(job_done_cb, NULL);
	}
	return NULL;
}

static void start_worker(void) {
	if (worker_started) {
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
		pthread_detach(thread);
		worker_started = true;
	}
}

static void run(const job_t *job) {
	start_worker();
	// No veil for playback or a star: the music starts right away, and a star
	// is not worth a pause.
	if (job->kind != JOB_PLAY && job->kind != JOB_STAR) {
		busy_show();
	}
	submit(job);
}

// ---------------------------------------------------------------------------
// the library on the card
// ---------------------------------------------------------------------------

#define RESYNC_AFTER_SECS (12 * 60 * 60)

static bool wifi_is_connected(void) {
#ifdef HOST_BUILD
	// The simulator runs on a machine that has a network but no wlan0.
	return true;
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

// Reads the server's songs, when they have not been read lately.
static void maybe_sync(bool force) {
	if (!subsonic_logged_in() || !sdb_ready() || sdb_sync_running() || !wifi_is_connected()) {
		return;
	}
	long last = sdb_last_sync();
	if (force || last == 0 || sdb_total() <= 0 || (long)time(NULL) - last > RESYNC_AFTER_SECS) {
		sdb_sync_start();
	}
}

static lv_obj_t *home_container;
static lv_obj_t *settings_btn;
static lv_obj_t *search_btn;
static lv_obj_t *sync_value;
static lv_timer_t *sync_timer;

static void sync_value_refresh(void) {
	if (!sync_value) {
		return;
	}
	char text[96];
	if (sdb_sync_running()) {
		snprintf(text, sizeof(text), "%d", sdb_sync_found());
	} else {
		int total = sdb_total();
		snprintf(text, sizeof(text), "%d", total < 0 ? 0 : total);
	}
	lv_label_set_text(sync_value, text);
}

static void sync_done_async(void *user) {
	bool ok = (bool)(intptr_t)user;
	if (sync_timer) {
		lv_timer_pause(sync_timer);
	}
	if (!ok && sdb_sync_error()[0]) {
		gui_notify_popup(sdb_sync_error());
	}
	if (lv_screen_active() == subsonic_screen) {
		rebuild_home();
	} else if (lv_screen_active() == list_screen) {
		view_refresh();
	}
}

static void on_sync_done(bool ok) { gui_post(sync_done_async, (void *)(intptr_t)ok); }

static void sync_tick_cb(lv_timer_t *timer) {
	(void)timer;
	sync_value_refresh();
}

static void update_library_cb(lv_event_t *e) {
	(void)e;
	if (!sdb_ready()) {
		gui_notify_popup("download_no_card");
		return;
	}
	if (!wifi_is_connected()) {
		gui_notify_popup("enable_wi_fi_first");
		return;
	}
	if (sdb_sync_start()) {
		lv_timer_resume(sync_timer);
		sync_value_refresh();
	}
}

// ---------------------------------------------------------------------------
// what comes back from the worker, on the GUI thread
// ---------------------------------------------------------------------------

static void job_done_cb(void *user) {
	(void)user;
	busy_hide();

	if (result_error[0]) {
		gui_notify_popup(result_error);
		return;
	}

	switch (result_kind) {
	case JOB_LOGIN:
		sdb_open(card_root());
		rebuild_home();
		switch_screen(subsonic_screen);
		toast_success("signed_in");
		maybe_sync(true);
		if (sdb_sync_running()) {
			lv_timer_resume(sync_timer);
		}
		break;

	case JOB_PLAYLISTS:
	case JOB_PLAYLIST_SONGS:
		open_view(&result_view, result_push);
		break;

	case JOB_STAR:
		if (lv_screen_active() == list_screen) {
			view_refresh();
		}
		break;

	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

static lv_obj_t *search_field;
static lv_obj_t *search_clear_btn;
static keyboard_t *search_keyboard;

static void refresh_search_clear(void) {
	if (!search_clear_btn) {
		return;
	}
	const char *text = lv_textarea_get_text(search_field);
	lv_obj_set_hidden(search_clear_btn, !(text && text[0]));
}

static void search_field_changed_cb(lv_event_t *e) {
	(void)e;
	refresh_search_clear();
}

static void search_clear_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	refresh_search_clear();
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !*text) {
		return;
	}
	char query[SUBSONIC_ID_MAX];
	snprintf(query, sizeof(query), "%s", text);
	history_len = 0; // a search is a start, not a step
	open_songs(SDB_SONGS_SEARCH, query, query);
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	refresh_search_clear();
	switch_screen(search_screen);
}

static void build_search_page(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);

	settingsrow_title(search_screen, cfg, "subsonic_search");
	int top = settingsrow_content_top(cfg);

	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("artist_album_or_track"));
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	lv_obj_add_event_cb(search_field, search_field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	// The x on the right edge of the field clears the query. Hidden while empty.
	search_clear_btn = lv_btn_create(search_screen);
	lv_obj_set_size(search_clear_btn, 56, 56);
	lv_obj_align(search_clear_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + (62 - 56) / 2);
	lv_obj_set_style_bg_opa(search_clear_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(search_clear_btn, 0, 0);
	lv_obj_set_style_border_width(search_clear_btn, 0, 0);
	lv_obj_set_style_pad_all(search_clear_btn, 0, 0);
	lv_obj_set_hidden(search_clear_btn, true);
	lv_obj_add_event_cb(search_clear_btn, search_clear_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *clear_icon = lv_image_create(search_clear_btn);
	lv_image_set_src(clear_icon, &icon_clear);
	lv_obj_add_style(clear_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(clear_icon, LV_OPA_70, 0);
	lv_obj_center(clear_icon);

	lv_obj_set_style_pad_right(search_field, 60, 0);

	search_keyboard = keyboard_create(search_screen, cfg->screen_width, 316, search_field, NULL, "ok",
									  search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
}

// ---------------------------------------------------------------------------
// sign in: server, user, password
// ---------------------------------------------------------------------------

static lv_obj_t *server_field;
static lv_obj_t *user_field;
static lv_obj_t *password_field;
static lv_obj_t *password_eye_icon;
static keyboard_t *login_keyboard;

static void password_eye_refresh(void) {
	bool hidden = lv_textarea_get_password_mode(password_field);
	lv_image_set_src(password_eye_icon, hidden ? &icon_eye : &icon_eye_off);
	keyboard_refresh_password(password_field);
}

static void password_eye_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_password_mode(password_field, !lv_textarea_get_password_mode(password_field));
	password_eye_refresh();
}

// The keyboard writes into one of the three fields: the last one touched. The
// caret shows only in that one -- LVGL blinks every textarea's caret from
// creation, and several carets together do not say where typing goes.
static void focus_field(lv_obj_t *field) {
	lv_obj_t *const fields[] = {server_field, user_field, password_field};
	keyboard_set_field(login_keyboard, field);
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		bool on = fields[i] == field;
		if (on) {
			lv_obj_add_state(fields[i], LV_STATE_FOCUSED);
		} else {
			lv_obj_remove_state(fields[i], LV_STATE_FOCUSED);
		}
		keyboard_show_caret(fields[i], on);
	}
}

static void field_focus_cb(lv_event_t *e) { focus_field(lv_event_get_target(e)); }

static void login_accept_cb(lv_event_t *e) {
	(void)e;
	const char *server = lv_textarea_get_text(server_field);
	const char *user = lv_textarea_get_text(user_field);
	const char *password = lv_textarea_get_text(password_field);
	if (!server || !*server || !user || !*user || !password || !*password) {
		gui_notify_popup("credentials_required");
		return;
	}

	job_t job = {0};
	job.kind = JOB_LOGIN;
	snprintf(job.server, sizeof(job.server), "%s", server);
	snprintf(job.user, sizeof(job.user), "%s", user);
	snprintf(job.password, sizeof(job.password), "%s", password);
	run(&job);

	// The password does not stay in the field a second longer than needed.
	lv_textarea_set_text(password_field, "");
}

static lv_obj_t *make_field(lv_obj_t *parent, gui_config_t *cfg, const char *placeholder, int y) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, field_focus_cb, LV_EVENT_CLICKED, NULL);
	return field;
}

static void build_login_page(gui_config_t *cfg) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);

	settingsrow_title(login_screen, cfg, "subsonic_sign_in");
	int top = settingsrow_content_top(cfg);

	server_field = make_field(login_screen, cfg, "subsonic_server_address", top);
	user_field = make_field(login_screen, cfg, "subsonic_username", top + 72);
	password_field = make_field(login_screen, cfg, "password", top + 144);

	// Dots from the first keystroke, and the eye to reveal the whole of it.
	keyboard_style_password(password_field, 0);
	lv_obj_set_style_pad_right(password_field, 60, 0);

	lv_obj_t *eye = lv_btn_create(login_screen);
	lv_obj_set_size(eye, 56, 56);
	lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + 144 + 3);
	lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(eye, 0, 0);
	lv_obj_set_style_border_width(eye, 0, 0);
	lv_obj_add_event_cb(eye, password_eye_cb, LV_EVENT_CLICKED, NULL);

	password_eye_icon = lv_image_create(eye);
	lv_obj_add_style(password_eye_icon, &theme_style_icon, 0);
	lv_obj_center(password_eye_icon);
	password_eye_refresh();

	lv_obj_t *note = lv_label_create(login_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, cfg->padding, top + 222);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("subsonic_login_note"));

	login_keyboard = keyboard_create(login_screen, cfg->screen_width, 316, server_field, NULL, "ok", login_accept_cb,
									 NULL);
	switcher_attach_back_gesture(login_screen);
}

static void open_login_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(password_field, "");
	lv_textarea_set_password_mode(password_field, true);
	password_eye_refresh();
	keyboard_reset(login_keyboard);
	focus_field(lv_textarea_get_text(server_field)[0] ? user_field : server_field);
	switch_screen(login_screen);
}

// ---------------------------------------------------------------------------
// settings: who is signed in, and signing out
// ---------------------------------------------------------------------------

static lv_obj_t *settings_info;

static void do_logout(void *user) {
	(void)user;
	subsonic_logout();
	// The tracks downloaded from that server are of no use to anyone. The list
	// of songs goes when the next sign-in finds another server or user (see
	// subsonicdb.h); for the same one it is still right.
	subsoniccache_clear();
	rebuild_home();
	switch_screen(subsonic_screen);
	toast_success("signed_out");
}

static void logout_cb(lv_event_t *e) {
	(void)e;
	char message[300];
	snprintf(message, sizeof(message), tr("sign_out_confirm_note"), subsonic_server());
	confirm_show("subsonic_sign_out", message, "leave", do_logout, NULL);
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	char info[400];
	snprintf(info, sizeof(info), tr("subsonic_signed_in_as"), subsonic_user(), subsonic_server());
	lv_label_set_text(settings_info, info);
	switch_screen(settings_screen);
}

static void build_settings_page(gui_config_t *cfg) {
	settings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "subsonic_settings");

	settings_info = lv_label_create(container);
	lv_label_set_long_mode(settings_info, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(settings_info, lv_pct(100));
	lv_obj_add_style(settings_info, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(settings_info, &font_ui_20, 0);
	lv_obj_set_style_pad_all(settings_info, 8, 0);

	settingsrow_action(container, "leave", logout_cb, NULL);
	switcher_attach_back_gesture(settings_screen);
}

// ---------------------------------------------------------------------------
// the main page
// ---------------------------------------------------------------------------

typedef enum {
	HOME_ALL,
	HOME_ARTISTS,
	HOME_ALBUMS,
	HOME_GENRES,
	HOME_STARRED,
	HOME_RECENT,
	HOME_PLAYLISTS,
} home_row_t;

static void home_row_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	home_row_t row = (home_row_t)(intptr_t)lv_event_get_user_data(e);
	history_len = 0;
	view.kind = VIEW_NONE;
	switch (row) {
	case HOME_ALL:
		open_songs(SDB_SONGS_ALL, NULL, tr("music_all_tracks"));
		break;
	case HOME_ARTISTS:
		open_names(SDB_NAMES_ARTISTS, tr("artists"));
		break;
	case HOME_ALBUMS:
		open_names(SDB_NAMES_ALBUMS, tr("albums"));
		break;
	case HOME_GENRES:
		open_names(SDB_NAMES_GENRES, tr("music_genres"));
		break;
	case HOME_STARRED:
		open_songs(SDB_SONGS_STARRED, NULL, tr("subsonic_starred_tracks"));
		break;
	case HOME_RECENT:
		open_songs(SDB_SONGS_RECENT, NULL, tr("subsonic_recently_added"));
		break;
	case HOME_PLAYLISTS: {
		job_t job = {0};
		job.kind = JOB_PLAYLISTS;
		job.push = true;
		run(&job);
		break;
	}
	}
}

static void update_corner_buttons(void) {
	bool visible = subsonic_logged_in();
	lv_obj_t *const buttons[] = {settings_btn, search_btn};
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (buttons[i]) {
			lv_obj_set_hidden(buttons[i], !visible);
		}
	}
}

// Same shape as the Music corner buttons: 56 px, transparent, aligned top right
// and stepped leftwards one slot at a time.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(subsonic_screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);

	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	return button;
}

static void open_wifi_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

static void open_login_cb(lv_event_t *e);

static void rebuild_home(void) {
	lv_obj_clean(home_container);
	sync_value = NULL;
	update_corner_buttons();

	if (!subsonic_logged_in()) {
		if (!wifi_is_connected()) {
			lv_obj_t *note = lv_label_create(home_container);
			lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
			lv_obj_set_width(note, lv_pct(100));
			lv_obj_add_style(note, &theme_style_text, 0);
			lv_obj_set_style_text_font(note, &font_ui_22, 0);
			lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_set_style_pad_hor(note, 4, 0);
			lv_obj_set_style_pad_top(note, 14, 0);
			lv_label_set_text(note, tr("enable_wi_fi_first"));
			lv_obj_t *row = settingsrow_add(home_container, "wi_fi_settings", NULL, open_wifi_settings_cb, NULL);
			lv_obj_set_style_margin_top(row, 16, 0);
			return;
		}
		settingsrow_add(home_container, "sign_in", NULL, open_login_cb, NULL);
		return;
	}

	char count[24];
	int total = sdb_total();
	snprintf(count, sizeof(count), "%d", total < 0 ? 0 : total);

	lv_obj_t *value = NULL;
	settingsrow_add(home_container, "music_all_tracks", &value, home_row_cb, (void *)(intptr_t)HOME_ALL);
	if (value) {
		lv_label_set_text(value, count);
	}
	settingsrow_add(home_container, "artists", NULL, home_row_cb, (void *)(intptr_t)HOME_ARTISTS);
	settingsrow_add(home_container, "albums", NULL, home_row_cb, (void *)(intptr_t)HOME_ALBUMS);
	settingsrow_add(home_container, "music_genres", NULL, home_row_cb, (void *)(intptr_t)HOME_GENRES);
	settingsrow_add(home_container, "subsonic_starred_tracks", NULL, home_row_cb, (void *)(intptr_t)HOME_STARRED);
	settingsrow_add(home_container, "subsonic_recently_added", NULL, home_row_cb, (void *)(intptr_t)HOME_RECENT);
	settingsrow_add(home_container, "playlists", NULL, home_row_cb, (void *)(intptr_t)HOME_PLAYLISTS);
	settingsrow_add(home_container, "subsonic_update_library", &sync_value, update_library_cb, NULL);
	sync_value_refresh();
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	// The card may have been put in or taken out since the page was built, and
	// without one there is nowhere to download to or keep the songs.
	subsoniccache_set_root(card_root());
	if (!sdb_ready()) {
		sdb_open(card_root());
	}

	// Rebuilt on every open: the network, the sign-in and the songs can change
	// while the user is elsewhere.
	rebuild_home();
	maybe_sync(false);
	if (sdb_sync_running()) {
		lv_timer_resume(sync_timer);
	}
}

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	lv_obj_add_style(list_screen, &theme_style_screen, 0);
	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(list_screen, list_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	list_title = settingsrow_title(list_screen, cfg, "subsonic");
	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *screen_container = lv_obj_create(list_screen);
	lv_obj_set_size(screen_container, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(screen_container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(screen_container, 0, 0);
	lv_obj_set_style_border_width(screen_container, 0, 0);
	lv_obj_set_style_pad_all(screen_container, 0, 0);
	lv_obj_set_style_radius(screen_container, 0, 0);
	lv_obj_set_flex_flow(screen_container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(screen_container, false);

	// The viewport scrolls; nothing inside it is laid out by LVGL, because the
	// rows are positioned by hand from the scroll offset.
	list_viewport = lv_obj_create(screen_container);
	lv_obj_set_width(list_viewport, lv_pct(100));
	lv_obj_set_style_bg_opa(list_viewport, 0, 0);
	lv_obj_set_style_border_width(list_viewport, 0, 0);
	lv_obj_set_style_radius(list_viewport, 0, 0);
	lv_obj_set_style_pad_hor(list_viewport, cfg->padding, 0);
	lv_obj_set_style_pad_ver(list_viewport, 0, 0);
	lv_obj_set_flex_grow(list_viewport, 1);
	lv_obj_set_scroll_dir(list_viewport, LV_DIR_VER);
	lv_obj_add_event_cb(list_viewport, list_scrolled_cb, LV_EVENT_SCROLL, NULL);

	// As tall as the entire list: what gives the viewport something to scroll
	// over. The rows inside it are the only widgets that exist.
	list_body = lv_obj_create(list_viewport);
	lv_obj_set_width(list_body, lv_pct(100));
	lv_obj_set_height(list_body, ROW_PITCH);
	lv_obj_set_style_bg_opa(list_body, 0, 0);
	lv_obj_set_style_border_width(list_body, 0, 0);
	lv_obj_set_style_pad_all(list_body, 0, 0);
	lv_obj_set_scrollable(list_body, false);
	lv_obj_set_event_bubble(list_body, true);

	build_rows(cfg->screen_width - 2 * cfg->padding);

	list_empty = lv_label_create(list_screen);
	lv_label_set_long_mode(list_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(list_empty, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_22, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, content_top + 100);
	lv_obj_set_hidden(list_empty, true);

	az = azindex_create(list_screen, cfg, list_viewport, ROW_PITCH, az_jumped);

	switcher_attach_back_gesture(list_screen);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	// The leftward drag that pulls in the player sheet, as on every browsable page.
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_viewport, true);

	mark_timer = lv_timer_create(mark_tick_cb, 1000, NULL);
	lv_timer_pause(mark_timer);
}

void subsonicpage_init(gui_config_t *cfg) {
	g_cfg = cfg;
	subsonic_screen = lv_obj_create(NULL);
	home_container = settingsrow_page(subsonic_screen, cfg, "subsonic");

	// Two buttons top right, like the Music gear: settings and search. The title
	// must be told the corner is taken, otherwise its text runs under them.
	settingsrow_title_corner_slots(settingsrow_page_title(subsonic_screen), cfg, 2);
	settings_btn = corner_button(cfg, 0, &icon_music_settings, open_settings_cb);
	search_btn = corner_button(cfg, 1, &icon_search, open_search_cb);

	build_list_page(cfg);
	build_search_page(cfg);
	build_settings_page(cfg);
	build_login_page(cfg);
	build_busy_layer();
	subsoniccache_set_slow_start_cb(on_slow_start);
	device_state_add_prepare_cb(subsonic_prepare_track);

	// The queue watchdog: tells the prefetch thread which track is missing next.
	lv_timer_create(queue_watch_cb, 1000, NULL);
	sync_timer = lv_timer_create(sync_tick_cb, 1000, NULL);
	lv_timer_pause(sync_timer);
	sdb_set_sync_listener(on_sync_done);

	subsoniccache_set_root(card_root());
	sdb_open(card_root());
	rebuild_home();

	lv_obj_add_event_cb(subsonic_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(subsonic_screen);
	player_sheet_attach_drag(subsonic_screen, true);
	player_sheet_attach_drag(home_container, true);
}
