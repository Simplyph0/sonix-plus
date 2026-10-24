#include "subsonicpage.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
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

// ---------------------------------------------------------------------------
// Subsonic front end.
//
// Built the way the Qobuz page is, because the constraints are the same:
// everything that talks to the network BLOCKS (see subsonic.h), so none of it
// runs on the GUI thread. There is a single worker with a single slot for the
// job to do, and results come back through gui_post(). Touching a second entry
// while the first is still loading means the second is what matters: the first
// is dropped, not queued.
//
// Smaller than Qobuz in two ways. There is no account to write to (no
// favourites, no playlists edited from here), and the lists carry no covers: the
// cover of the track that plays arrives with the track, as a sidecar.
// ---------------------------------------------------------------------------

#define PAGE_LIMIT 50
// Four pages, as for Qobuz: every entry is static memory on a device with 64 MB.
#define MAX_HELD (4 * PAGE_LIMIT)

// How many tracks to prefetch while one plays.
#define PREFETCH_AHEAD 2

lv_obj_t *subsonic_screen;
static lv_obj_t *login_screen;
static lv_obj_t *list_screen;
static lv_obj_t *search_screen;
static lv_obj_t *settings_screen;

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_LOGIN,
	JOB_ARTISTS,
	JOB_ARTIST_ALBUMS,
	JOB_ALBUMS_NEWEST,
	JOB_ALBUMS_AZ,
	JOB_ALBUMS_FREQUENT,
	JOB_ALBUMS_RANDOM,
	JOB_ALBUM_TRACKS,
	JOB_PLAYLISTS,
	JOB_PLAYLIST_TRACKS,
	JOB_STARRED,
	JOB_SEARCH_TRACKS,
	JOB_SEARCH_ALBUMS,
	JOB_SEARCH_ARTISTS,
	JOB_PLAY,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char text[256];	  // search terms, or the title of what was opened
	char server[256]; // login: the server address
	char user[96];	  // login: the user name
	char password[128];
	char id[SUBSONIC_ID_MAX];
	int index;	// which track of the list playback starts from
	int offset; // first entry of this page: 0 = new list
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

// What came back. Only the worker writes here, and the GUI reads inside the
// async that runs after the worker is done.
static subsonic_track_t result_tracks[MAX_HELD];
static subsonic_album_t result_albums[MAX_HELD];
static subsonic_artist_t result_artists[MAX_HELD];
static subsonic_playlist_t result_playlists[MAX_HELD];
static int result_count;
static int result_from; // where the last page received starts (0 = new list)
static job_kind_t result_kind;
static char result_error[256];
static char result_title[160];

// What the list is showing right now, so a row tap knows what to do.
static job_kind_t list_kind;

// Pagination: the last page came back full, so there may be another -- and one
// request at a time.
static bool page_more;
static bool page_loading;

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

// ---------------------------------------------------------------------------
// the result list
// ---------------------------------------------------------------------------

static lv_obj_t *list_container;
static lv_obj_t *list_title;
static lv_obj_t *list_empty;

static void rebuild_home(void);
static void row_clicked_cb(lv_event_t *e);
static void run(const job_t *job);

static void duration_text(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

// One list row: name on top, detail below. The two labels form a flex column
// with a declared gap, each exactly one line high, so too-long text can only
// end in an ellipsis instead of wrapping over the subtitle.
static lv_obj_t *make_row(const char *name, const char *detail, int index) {
	lv_obj_t *row = lv_btn_create(list_container);
	lv_obj_set_size(row, lv_pct(100), 88);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// Gestures (back, and pulling in the player sheet) often start on a row, and
	// a button keeps the press to itself.
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *texts = lv_obj_create(row);
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

	lv_obj_t *label = lv_label_create(texts);
	lv_label_set_text(label, name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);

	if (detail && detail[0]) {
		lv_obj_t *sub = lv_label_create(texts);
		lv_label_set_text(sub, detail);
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_obj_set_width(sub, lv_pct(100));
		lv_obj_set_height(sub, lv_font_get_line_height(&font_ui_18));
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_18, 0);
	}
	return row;
}

// "FLAC 24/96 · 5:12", or "MP3 320 kbps · 3:42" when the server says what it
// has, and the length alone when it does not.
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

// `from` is the first row to create: 0 rebuilds everything, a higher value
// APPENDS the new rows without touching the others, which keeps the scroll
// position steady when the next page arrives.
static void fill_list(int from) {
	if (from == 0) {
		lv_obj_clean(list_container);
	}
	list_kind = result_kind;

	for (int i = from; i < result_count; i++) {
		char detail[400];
		switch (result_kind) {
		case JOB_ALBUM_TRACKS:
		case JOB_PLAYLIST_TRACKS:
		case JOB_STARRED:
		case JOB_SEARCH_TRACKS: {
			track_detail(&result_tracks[i], detail, sizeof(detail));
			make_row(result_tracks[i].title, detail, i);
			break;
		}
		case JOB_ARTIST_ALBUMS:
		case JOB_ALBUMS_NEWEST:
		case JOB_ALBUMS_AZ:
		case JOB_ALBUMS_FREQUENT:
		case JOB_ALBUMS_RANDOM:
		case JOB_SEARCH_ALBUMS: {
			const subsonic_album_t *a = &result_albums[i];
			snprintf(detail, sizeof(detail), "%s%s%s", a->artist, a->artist[0] && a->year[0] ? " \xC2\xB7 " : "",
					 a->year);
			make_row(a->title, detail, i);
			break;
		}
		case JOB_ARTISTS:
		case JOB_SEARCH_ARTISTS: {
			const subsonic_artist_t *a = &result_artists[i];
			detail[0] = '\0';
			if (a->album_count > 0) {
				snprintf(detail, sizeof(detail), "%d %s", a->album_count, tr("albums"));
			}
			make_row(a->name, detail, i);
			break;
		}
		case JOB_PLAYLISTS: {
			const subsonic_playlist_t *p = &result_playlists[i];
			snprintf(detail, sizeof(detail), "%s%s%d", p->owner, p->owner[0] ? " \xC2\xB7 " : "", p->track_count);
			make_row(p->name, detail, i);
			break;
		}
		default:
			break;
		}
	}

	lv_obj_set_hidden(list_empty, result_count != 0);
	lv_label_set_text(list_title, result_title);
}

// ---------------------------------------------------------------------------
// what comes back from the worker, on the GUI thread
// ---------------------------------------------------------------------------

static void job_done_cb(void *user) {
	(void)user;
	busy_hide();

	if (result_error[0]) {
		gui_notify_popup(result_error);
		page_loading = false;
		return;
	}

	switch (result_kind) {
	case JOB_LOGIN:
		rebuild_home();
		switch_screen(subsonic_screen);
		toast_success("signed_in");
		break;

	case JOB_PLAY:
		// Nothing: the worker opened the player the instant the music started.
		break;

	default:
		fill_list(result_from);
		page_loading = false;
		// A full last page probably means there is another. The random list is
		// not one list but a fresh draw each time: asking for more would repeat.
		page_more = result_kind != JOB_ALBUMS_RANDOM && (result_count - result_from == PAGE_LIMIT) &&
					result_count < MAX_HELD;
		if (lv_screen_active() != list_screen && result_from == 0) {
			switch_screen(list_screen);
		}
		break;
	}
}

// ---------------------------------------------------------------------------
// playback
//
// The tapped track downloads and starts by itself. The ones after it, in the
// same list, download while it plays and are queued as they arrive: an album
// plays straight through without waiting for the whole thing to land first.
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

// The whole queued list, for the prefetch thread and for on-demand requests.
static subsonic_track_t queued_tracks[MAX_HELD];
static int queued_count;

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
}

static void write_track_sidecars(const subsonic_track_t *t, bool with_cover) {
	char cover[SUBSONIC_URL_MAX] = "";
	if (with_cover && t->cover_id[0]) {
		subsonic_cover_url(t->cover_id, 600, cover, sizeof(cover));
	}
	subsoniccache_write_sidecars(t->id, t->suffix, t->title, t->artist, t->album, t->album_id, t->track_number, cover);
}

// Downloads one track of the list while another plays: address, download, and
// the sidecar files (tags and cover) so everything is ready by its turn.
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

static void do_play(int index) {
	if (index < 0 || index >= result_count) {
		return;
	}
	const subsonic_track_t *track = &result_tracks[index];

	// Whatever was downloading for another track is no longer needed.
	streamturn_abandon_all();

	char url[SUBSONIC_URL_MAX];
	if (!subsonic_stream_url(track->id, url, sizeof(url))) {
		snprintf(result_error, sizeof(result_error), "%s", tr("subsonic_no_track_address"));
		return;
	}

	char path[512];
	if (!subsoniccache_start(track->id, url, track->suffix, track->duration, path, sizeof(path))) {
		const char *why = subsoniccache_last_error();
		snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr("download_failed"));
		return;
	}
	subsoniccache_note_played(path);

	// Tags BEFORE starting playback: that is when device_state reads the metadata.
	write_track_sidecars(track, false);

	// The queue is the WHOLE list, not only what has downloaded: every path is
	// known before the track is, since the file is named after the id and the
	// suffix the server reported.
	static char paths[MAX_HELD][512];
	static const char *path_ptr[MAX_HELD];

	queued_count = 0;
	int start = 0;
	for (int i = 0; i < result_count && queued_count < MAX_HELD; i++) {
		if (i == index) {
			start = queued_count;
			snprintf(paths[queued_count], sizeof(paths[0]), "%s", path);
		} else {
			subsoniccache_path(result_tracks[i].id, result_tracks[i].suffix, paths[queued_count], sizeof(paths[0]));
		}
		path_ptr[queued_count] = paths[queued_count];
		queued_tracks[queued_count] = result_tracks[i];
		queued_count++;
	}

	// Tags for the whole queue at once: a few dozen bytes each, and without them
	// the queue page would show file names for everything not yet downloaded.
	for (int i = 0; i < queued_count; i++) {
		if (i != start) {
			write_track_sidecars(&queued_tracks[i], false);
		}
	}

	device_state_play_list(path_ptr, queued_count, start);

	// The cover last, with the music already playing: it is another download.
	write_track_sidecars(track, true);

	gui_post(play_started_async, NULL);
	prefetch_submit();
}

// The queue watchdog, once a second on the GUI thread. It tells the cache which
// files it must not evict (those still to be played) and the prefetch thread
// WHICH track to download now: the first missing one between the current track
// and the next few. When playback has reached a track that is not there yet,
// that is exactly the one being waited for.
static void queue_watch_cb(lv_timer_t *timer) {
	(void)timer;
	// A queue that is a library list holds no cache files. The bookkeeping below
	// still has to run: the flag keeping Wi-Fi awake is only lowered here.
	bool library_queue = playlist_is_library_backed();
	if (library_queue) {
		subsoniccache_set_protected(NULL, 0);
		waiting_for_track = false;
	}

	// Nothing more is asked of the network while the Wi-Fi transfer is on: the
	// track would land on a card another process is writing.
	bool transfer_on = wifitransfer_get_enabled();
	if (transfer_on) {
		waiting_for_track = false;
	}

	int count = playlist_count();
	int current = playlist_current_index();

	static char queued[SUBSONICCACHE_PROTECTED_MAX][512];
	static const char *queued_ptr[SUBSONICCACHE_PROTECTED_MAX];
	int protect = 0;
	for (int i = current; !library_queue && i >= 0 && i < count && protect < SUBSONICCACHE_PROTECTED_MAX; i++) {
		if (playlist_path_at(i, queued[protect], sizeof(queued[0]))) {
			queued_ptr[protect] = queued[protect];
			protect++;
		}
	}
	if (!library_queue) {
		subsoniccache_set_protected(queued_ptr, protect);
	}

	char want[SUBSONICCACHE_KEY_MAX] = "";
	if (!library_queue && !transfer_on && current >= 0 && queued_count > 0) {
		for (int i = current; i <= current + PREFETCH_AHEAD && i < count && !want[0]; i++) {
			char path[512];
			char key[SUBSONICCACHE_KEY_MAX];
			if (!playlist_path_at(i, path, sizeof(path)) || !subsoniccache_key_from_path(path, key, sizeof(key))) {
				continue;
			}
			if (!subsoniccache_has(key, path_suffix(path), NULL, 0)) {
				snprintf(want, sizeof(want), "%s", key);
			}
		}
	}

	pthread_mutex_lock(&prefetch_lock);
	if (strcmp(want, prefetch_want_key) != 0) {
		snprintf(prefetch_want_key, sizeof(prefetch_want_key), "%s", want);
		pthread_cond_broadcast(&prefetch_ready);
	}
	pthread_mutex_unlock(&prefetch_lock);

	// The wait only holds while the queue really points at a Subsonic track.
	if (waiting_for_track) {
		int index = playlist_current_index();
		char wanted[512];
		if (index >= 0 && playlist_path_at(index, wanted, sizeof(wanted)) && !subsoniccache_owns(wanted)) {
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

		// From key to track: the data (title, duration, suffix) is taken from the
		// queued list by SEARCHING, not by trusting a position.
		int index = -1;
		for (int i = 0; i < queued_count && index < 0; i++) {
			char other[SUBSONICCACHE_KEY_MAX];
			subsoniccache_key(queued_tracks[i].id, other, sizeof(other));
			if (strcmp(other, key) == 0) {
				index = i;
			}
		}

		if (index < 0) {
			// Not in the queue being played: wait for the watchdog to recompute.
			snprintf(prefetch_failed_key, sizeof(prefetch_failed_key), "%s", key);
			prefetch_failed_ms = lv_tick_get();
			pthread_mutex_lock(&prefetch_lock);
			if (strcmp(prefetch_want_key, key) == 0) {
				prefetch_want_key[0] = '\0';
			}
			pthread_mutex_unlock(&prefetch_lock);
			continue;
		}

		subsonic_track_t track = queued_tracks[index];
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
// the worker's main loop
// ---------------------------------------------------------------------------

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

		// Where this page starts. 0 is a new list; anything higher continues the
		// one already held, and only the tail of the array is written.
		int off = job.offset;
		if (off < 0 || off >= MAX_HELD) {
			off = 0;
		}
		int want = MAX_HELD - off;
		if (want > PAGE_LIMIT) {
			want = PAGE_LIMIT;
		}

		// Playback does NOT clear the list: what has to play is precisely the
		// track held in it, plus the ones after it to prefetch.
		if (job.kind != JOB_PLAY && job.kind != JOB_LOGIN && off == 0) {
			result_count = 0;
			result_title[0] = '\0';
		}

		int n = 0;
		switch (job.kind) {
		case JOB_LOGIN:
			if (!subsonic_login(job.server, job.user, job.password)) {
				snprintf(result_error, sizeof(result_error), "%s", subsonic_last_error());
			}
			memset(job.password, 0, sizeof(job.password));
			break;

		case JOB_ARTISTS:
			n = subsonic_artists(off, result_artists + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("artists"));
			break;
		case JOB_ARTIST_ALBUMS:
			n = subsonic_artist_albums(job.id, off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_ALBUMS_NEWEST:
			n = subsonic_album_list("newest", off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("subsonic_recently_added"));
			break;
		case JOB_ALBUMS_AZ:
			n = subsonic_album_list("alphabeticalByName", off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("albums"));
			break;
		case JOB_ALBUMS_FREQUENT:
			n = subsonic_album_list("frequent", off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("subsonic_most_played"));
			break;
		case JOB_ALBUMS_RANDOM:
			n = subsonic_album_list("random", off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("subsonic_random_albums"));
			break;

		case JOB_ALBUM_TRACKS:
			n = subsonic_album_tracks(job.id, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_PLAYLISTS:
			n = subsonic_playlists(off, result_playlists + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("playlists"));
			break;
		case JOB_PLAYLIST_TRACKS:
			n = subsonic_playlist_tracks(job.id, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_STARRED:
			n = subsonic_starred_tracks(off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%s", tr("subsonic_starred_tracks"));
			break;

		case JOB_SEARCH_TRACKS:
			n = subsonic_search_tracks(job.text, off, result_tracks + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_SEARCH_ALBUMS:
			n = subsonic_search_albums(job.text, off, result_albums + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;
		case JOB_SEARCH_ARTISTS:
			n = subsonic_search_artists(job.text, off, result_artists + off, want);
			snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			break;

		case JOB_PLAY:
			do_play(job.index);
			break;

		default:
			break;
		}

		if (n < 0) {
			snprintf(result_error, sizeof(result_error), "%s", subsonic_last_error());
			n = 0;
		}
		if (job.kind != JOB_PLAY && job.kind != JOB_LOGIN) {
			result_from = off;
			result_count = off + n;
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

// ---------------------------------------------------------------------------
// going back
//
// Every Subsonic list lives on ONE reused page, so the screen manager has
// nothing to stack: opening an album from the results and then going back
// would skip the results. The page keeps its own history -- a stack of jobs --
// and the chevron (and the gesture, through the same function) consults it
// first; only when it is empty does back really leave the page.
// ---------------------------------------------------------------------------

#define LIST_HISTORY_DEPTH 8
static job_t list_history[LIST_HISTORY_DEPTH];
static int list_history_len;
static job_t current_list_job;
static bool have_current_list;

static bool job_is_list(job_kind_t kind) { return kind != JOB_PLAY && kind != JOB_LOGIN && kind != JOB_NONE; }

static bool list_back_has_step(void) { return list_history_len > 0; }

static bool list_back_guard(void) {
	if (list_history_len == 0) {
		have_current_list = false;
		return false; // nothing underneath: leave the page
	}
	job_t previous = list_history[--list_history_len];
	have_current_list = true;
	current_list_job = previous;
	page_more = false;
	page_loading = false;
	start_worker();
	busy_show();
	submit(&previous);
	return true;
}

static void run(const job_t *job) {
	if (job_is_list(job->kind)) {
		// Re-reading the SAME list is not a step forward: the whole job is
		// compared, not just its kind.
		bool same = have_current_list && current_list_job.kind == job->kind &&
					strcmp(current_list_job.id, job->id) == 0 && strcmp(current_list_job.text, job->text) == 0;
		if (have_current_list && !same && lv_screen_active() == list_screen && list_history_len < LIST_HISTORY_DEPTH) {
			list_history[list_history_len++] = current_list_job;
		} else if (lv_screen_active() != list_screen) {
			list_history_len = 0; // entering from outside: the history restarts
		}
		current_list_job = *job;
		have_current_list = true;
		page_more = false;
		page_loading = false;
	}

	start_worker();
	if (job->kind != JOB_PLAY) {
		busy_show();
	}
	submit(job);
}

// Another page when scrolling nears the bottom. No veil and no history entry:
// it is the SAME list continuing, and the rows already there stay usable.
static void maybe_load_more(void) {
	if (page_loading || !page_more) {
		return;
	}
	if (lv_screen_active() != list_screen || !have_current_list || !job_is_list(current_list_job.kind)) {
		return;
	}
	// A sideways drag (back, or the player sheet) also moves this list.
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	if (lv_obj_get_scroll_bottom(list_container) > lv_obj_get_height(list_container)) {
		return;
	}
	page_loading = true;
	job_t job = current_list_job;
	job.offset = result_count;
	start_worker();
	submit(&job);
}

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	maybe_load_more();
}

// ---------------------------------------------------------------------------
// taps
// ---------------------------------------------------------------------------

static void row_clicked_cb(lv_event_t *e) {
	// A gesture that ends over a row is not a tap on the row: events bubble, so
	// the click that closes a drag arrives here too.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= result_count) {
		return;
	}

	job_t job = {0};
	switch (list_kind) {
	case JOB_ALBUM_TRACKS:
	case JOB_PLAYLIST_TRACKS:
	case JOB_STARRED:
	case JOB_SEARCH_TRACKS:
		job.kind = JOB_PLAY;
		job.index = index;
		run(&job);
		return;

	case JOB_ARTIST_ALBUMS:
	case JOB_ALBUMS_NEWEST:
	case JOB_ALBUMS_AZ:
	case JOB_ALBUMS_FREQUENT:
	case JOB_ALBUMS_RANDOM:
	case JOB_SEARCH_ALBUMS:
		job.kind = JOB_ALBUM_TRACKS;
		snprintf(job.id, sizeof(job.id), "%s", result_albums[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_albums[index].title);
		run(&job);
		return;

	case JOB_ARTISTS:
	case JOB_SEARCH_ARTISTS:
		job.kind = JOB_ARTIST_ALBUMS;
		snprintf(job.id, sizeof(job.id), "%s", result_artists[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_artists[index].name);
		run(&job);
		return;

	case JOB_PLAYLISTS:
		job.kind = JOB_PLAYLIST_TRACKS;
		snprintf(job.id, sizeof(job.id), "%s", result_playlists[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_playlists[index].name);
		run(&job);
		return;

	default:
		return;
	}
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

static lv_obj_t *search_field;
static lv_obj_t *search_clear_btn;
static keyboard_t *search_keyboard;
static int search_kind_index; // 0 tracks, 1 albums, 2 artists
static lv_obj_t *search_pills[3];

static void paint_search_pills(void) {
	for (int i = 0; i < 3; i++) {
		bool on = i == search_kind_index;
		lv_obj_set_style_bg_color(search_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(search_pills[i], 0), on ? lv_color_white() : theme()->text_primary,
									0);
	}
}

static void search_pill_cb(lv_event_t *e) {
	search_kind_index = (int)(intptr_t)lv_event_get_user_data(e);
	paint_search_pills();
}

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
	job_t job = {0};
	job.kind = search_kind_index == 1 ? JOB_SEARCH_ALBUMS
									  : (search_kind_index == 2 ? JOB_SEARCH_ARTISTS : JOB_SEARCH_TRACKS);
	snprintf(job.text, sizeof(job.text), "%s", text);
	run(&job);
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

	static const char *const KINDS[] = {"tracks_2", "albums", "artists"};
	for (int i = 0; i < 3; i++) {
		lv_obj_t *pill = lv_btn_create(search_screen);
		lv_obj_set_size(pill, 140, 52);
		lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(pill, 0, 0);
		lv_obj_set_style_border_width(pill, 0, 0);
		lv_obj_align(pill, LV_ALIGN_TOP_LEFT, cfg->padding + i * 150, top + 78);
		lv_obj_add_event_cb(pill, search_pill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *label = lv_label_create(pill);
		lv_label_set_text(label, tr(KINDS[i]));
		lv_obj_set_style_text_font(label, &font_ui_20, 0);
		lv_obj_center(label);
		search_pills[i] = pill;
	}
	paint_search_pills();

	search_keyboard = keyboard_create(search_screen, cfg->screen_width, 316, search_field, NULL, "ok",
									  search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
	theme_register_refresh(paint_search_pills);
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
	// The tracks downloaded from that server are of no use to anyone.
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

static lv_obj_t *home_container;
static lv_obj_t *settings_btn;
static lv_obj_t *search_btn;

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

static void simple_job_cb(lv_event_t *e) {
	job_t job = {0};
	job.kind = (job_kind_t)(intptr_t)lv_event_get_user_data(e);
	run(&job);
}

static void open_wifi_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// Whether the network is actually up. With no IP address every request ends in
// a generic error that does not tell the user the one thing they need to know.
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

static void rebuild_home(void) {
	lv_obj_clean(home_container);
	update_corner_buttons();

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

	if (!subsonic_logged_in()) {
		settingsrow_add(home_container, "sign_in", NULL, open_login_cb, NULL);
		return;
	}

	settingsrow_add(home_container, "artists", NULL, simple_job_cb, (void *)(intptr_t)JOB_ARTISTS);
	settingsrow_add(home_container, "albums", NULL, simple_job_cb, (void *)(intptr_t)JOB_ALBUMS_AZ);
	settingsrow_add(home_container, "subsonic_recently_added", NULL, simple_job_cb, (void *)(intptr_t)JOB_ALBUMS_NEWEST);
	settingsrow_add(home_container, "subsonic_most_played", NULL, simple_job_cb, (void *)(intptr_t)JOB_ALBUMS_FREQUENT);
	settingsrow_add(home_container, "subsonic_random_albums", NULL, simple_job_cb, (void *)(intptr_t)JOB_ALBUMS_RANDOM);
	settingsrow_add(home_container, "playlists", NULL, simple_job_cb, (void *)(intptr_t)JOB_PLAYLISTS);
	settingsrow_add(home_container, "subsonic_starred_tracks", NULL, simple_job_cb, (void *)(intptr_t)JOB_STARRED);
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	// The card may have been put in or taken out since the page was built, and
	// without one there is nowhere to download to.
	subsoniccache_set_root(storage_sd_root());

	// Rebuilt on every open: the network and the sign-in can change while the
	// user is elsewhere.
	rebuild_home();
}

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	list_container = settingsrow_page(list_screen, cfg, "subsonic");
	list_title = settingsrow_page_title(list_screen);

	list_empty = lv_label_create(list_screen);
	lv_label_set_text(list_empty, tr("no_results"));
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_24, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 100);
	lv_obj_set_hidden(list_empty, true);

	lv_obj_add_event_cb(list_container, list_scrolled_cb, LV_EVENT_SCROLL, NULL);
	switcher_attach_back_gesture(list_screen);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	// The leftward drag that pulls in the player sheet, as on every browsable page.
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_container, true);
}

void subsonicpage_init(gui_config_t *cfg) {
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

	subsoniccache_set_root(storage_sd_root());
	rebuild_home();

	lv_obj_add_event_cb(subsonic_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(subsonic_screen);
	player_sheet_attach_drag(subsonic_screen, true);
	player_sheet_attach_drag(home_container, true);
}
