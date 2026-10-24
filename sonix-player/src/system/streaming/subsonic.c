#include "subsonic.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"

#define SUBSONIC_API_VERSION "1.16.1"
#define SUBSONIC_CLIENT "SonixPlayer"

// How long a reply is waited for, and how much of one is held in memory. An
// artist index of a large library is the biggest answer and stays well under
// this; the cap keeps a runaway response from becoming a memory problem.
#define SUBSONIC_TIMEOUT_SECS 15
#define SUBSONIC_BODY_LIMIT (4 * 1024 * 1024)

static char server_url[256];
static char user_name[96];
static char salt[32];
static char token[MD5_HEX_LEN];

static __thread char last_error[256];

const char *subsonic_last_error(void) { return last_error; }

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, args);
	va_end(args);
}

static void clear_error(void) { last_error[0] = '\0'; }

// ---------------------------------------------------------------------------
// session
// ---------------------------------------------------------------------------

void subsonic_init(void) {
	snprintf(server_url, sizeof(server_url), "%s", config_get("subsonic", "server", ""));
	snprintf(user_name, sizeof(user_name), "%s", config_get("subsonic", "user", ""));
	snprintf(salt, sizeof(salt), "%s", config_get("subsonic", "salt", ""));
	snprintf(token, sizeof(token), "%s", config_get("subsonic", "token", ""));
	printf("subsonic: %s\n", subsonic_logged_in() ? "saved session" : "no session");
}

bool subsonic_logged_in(void) { return server_url[0] && user_name[0] && salt[0] && token[0]; }
const char *subsonic_server(void) { return server_url; }
const char *subsonic_user(void) { return user_name; }

void subsonic_logout(void) {
	server_url[0] = user_name[0] = salt[0] = token[0] = '\0';
	config_set("subsonic", "server", "");
	config_set("subsonic", "user", "");
	config_set("subsonic", "salt", "");
	config_set("subsonic", "token", "");
	config_save();
	printf("subsonic: session closed\n");
}

// "host:4533" becomes "http://host:4533"; a trailing slash goes, so paths can
// be appended with one.
static void normalise_server(const char *typed, char *out, size_t size) {
	while (*typed == ' ') {
		typed++;
	}
	const char *prefix = strstr(typed, "://") ? "" : "http://";
	snprintf(out, size, "%s%s", prefix, typed);
	size_t len = strlen(out);
	while (len > 0 && (out[len - 1] == '/' || out[len - 1] == ' ')) {
		out[--len] = '\0';
	}
}

static void make_salt(char *out, size_t size) {
	unsigned char raw[6] = {0};
	FILE *f = fopen("/dev/urandom", "rb");
	if (!f || fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
		srand((unsigned)time(NULL) ^ (unsigned)getpid());
		for (size_t i = 0; i < sizeof(raw); i++) {
			raw[i] = (unsigned char)rand();
		}
	}
	if (f) {
		fclose(f);
	}
	snprintf(out, size, "%02x%02x%02x%02x%02x%02x", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
}

// ---------------------------------------------------------------------------
// making a request
// ---------------------------------------------------------------------------

// The query every request starts with. The user name goes through the URL
// encoder: a name with an "@" or a space in it is ordinary.
static int auth_query(char *out, size_t size, const char *user, const char *salt_in, const char *token_in) {
	char user_enc[3 * 96 + 1];
	http_url_encode(user, user_enc, sizeof(user_enc));
	return snprintf(out, size, "u=%s&t=%s&s=%s&v=%s&c=%s&f=json", user_enc, token_in, salt_in, SUBSONIC_API_VERSION,
					SUBSONIC_CLIENT);
}

// A request URL: `base`/rest/`method`.view?<auth>&<args>. `args` is already
// encoded ("id=..."), or NULL.
static bool build_url(const char *base, const char *user, const char *salt_in, const char *token_in,
					  const char *method, const char *args, char *out, size_t size) {
	char auth[512];
	auth_query(auth, sizeof(auth), user, salt_in, token_in);
	int n = snprintf(out, size, "%s/rest/%s.view?%s%s%s", base, method, auth, args && args[0] ? "&" : "",
					 args ? args : "");
	return n > 0 && (size_t)n < size;
}

// An integer argument.
static void arg_int(char *out, size_t size, const char *name, int value) { snprintf(out, size, "%s=%d", name, value); }

// An id argument: "id=<encoded>".
static void arg_text(char *out, size_t size, const char *name, const char *value) {
	char enc[3 * SUBSONIC_TITLE_MAX + 1];
	http_url_encode(value ? value : "", enc, sizeof(enc));
	snprintf(out, size, "%s=%s", name, enc);
}

// The answer is {"subsonic-response":{"status":"ok"|"failed", ...}}. On failure
// "error":{"code":40,"message":"Wrong username or password"} says why, in words
// written for a person.
static bool note_failure(const json_doc_t *doc, int response) {
	int error = json_get(doc, response, "error");
	char message[200] = "";
	if (error >= 0 && json_obj_str(doc, error, "message", message, sizeof(message)) && message[0]) {
		set_error("%s", message);
	} else {
		set_error("%s", tr("subsonic_unreadable_reply"));
	}
	return false;
}

// Makes the request, parses the answer, and fills last_error on failure. On
// success the caller frees *body only after it is done with *doc: the document
// points into the text instead of copying it. `*response` is the token of the
// "subsonic-response" object.
static bool api_call_as(const char *base, const char *user, const char *salt_in, const char *token_in,
						const char *method, const char *args, char **body, json_doc_t *doc, int *response) {
	clear_error();
	*body = NULL;

	char url[SUBSONIC_URL_MAX + 512];
	if (!base[0] || !build_url(base, user, salt_in, token_in, method, args, url, sizeof(url))) {
		set_error("%s", tr("subsonic_not_signed_in"));
		return false;
	}

	int status = 0;
	size_t len = 0;
	if (!http_get_ex(url, NULL, true, body, &len, SUBSONIC_BODY_LIMIT, SUBSONIC_TIMEOUT_SECS, &status) || !*body) {
		const char *why = http_last_error();
		if (why && why[0]) {
			set_error("%s", why);
		} else if (status > 0) {
			set_error(tr("subsonic_answered_with"), status);
		} else {
			set_error("%s", tr("subsonic_cannot_reach"));
		}
		free(*body);
		*body = NULL;
		return false;
	}

	if (!json_parse(*body, doc)) {
		set_error("%s", tr("subsonic_unreadable_reply"));
		free(*body);
		*body = NULL;
		return false;
	}

	*response = json_get(doc, json_root(doc), "subsonic-response");
	if (*response < 0) {
		set_error("%s", tr("subsonic_unreadable_reply"));
		json_free(doc);
		free(*body);
		*body = NULL;
		return false;
	}

	char state[16] = "";
	json_obj_str(doc, *response, "status", state, sizeof(state));
	if (strcmp(state, "ok") != 0) {
		note_failure(doc, *response);
		json_free(doc);
		free(*body);
		*body = NULL;
		return false;
	}
	return true;
}

static bool api_call(const char *method, const char *args, char **body, json_doc_t *doc, int *response) {
	if (!subsonic_logged_in()) {
		clear_error();
		set_error("%s", tr("subsonic_not_signed_in"));
		return false;
	}
	return api_call_as(server_url, user_name, salt, token, method, args, body, doc, response);
}

bool subsonic_login(const char *server, const char *user, const char *password) {
	clear_error();
	if (!server || !*server || !user || !*user || !password) {
		set_error("%s", tr("credentials_required"));
		return false;
	}

	char base[256];
	normalise_server(server, base, sizeof(base));

	char new_salt[32];
	make_salt(new_salt, sizeof(new_salt));

	char joined[256];
	snprintf(joined, sizeof(joined), "%s%s", password, new_salt);
	char new_token[MD5_HEX_LEN];
	md5_hex(joined, strlen(joined), new_token);
	memset(joined, 0, sizeof(joined));

	char *body = NULL;
	json_doc_t doc;
	int response = -1;
	if (!api_call_as(base, user, new_salt, new_token, "ping", NULL, &body, &doc, &response)) {
		return false;
	}
	json_free(&doc);
	free(body);

	snprintf(server_url, sizeof(server_url), "%s", base);
	snprintf(user_name, sizeof(user_name), "%s", user);
	snprintf(salt, sizeof(salt), "%s", new_salt);
	snprintf(token, sizeof(token), "%s", new_token);
	config_set("subsonic", "server", server_url);
	config_set("subsonic", "user", user_name);
	config_set("subsonic", "salt", salt);
	config_set("subsonic", "token", token);
	config_save();
	printf("subsonic: signed in to %s\n", server_url);
	return true;
}

// ---------------------------------------------------------------------------
// reading the answers
// ---------------------------------------------------------------------------

// Servers disagree about ids: Navidrome sends strings, older Subsonic numbers.
// Both come out as text.
static void read_id(const json_doc_t *doc, int obj, const char *key, char *out, size_t size) {
	out[0] = '\0';
	int tok = json_get(doc, obj, key);
	if (tok < 0) {
		return;
	}
	if (json_str(doc, tok, out, size)) {
		return;
	}
	const json_tok_t *t = &doc->toks[tok];
	if (t->type == JSON_NUMBER) {
		size_t len = (size_t)(t->end - t->start);
		if (len >= size) {
			len = size - 1;
		}
		memcpy(out, doc->text + t->start, len);
		out[len] = '\0';
	}
}

static void read_text(const json_doc_t *doc, int obj, const char *key, char *out, size_t size) {
	out[0] = '\0';
	json_obj_str(doc, obj, key, out, size);
}

// A list that is an array, or -- from a server that unwraps one-element lists --
// a single object.
static int list_count(const json_doc_t *doc, int list) {
	if (list < 0) {
		return 0;
	}
	if (doc->toks[list].type == JSON_ARRAY) {
		return json_len(doc, list);
	}
	return doc->toks[list].type == JSON_OBJECT ? 1 : 0;
}

static int list_item(const json_doc_t *doc, int list, int index) {
	return doc->toks[list].type == JSON_ARRAY ? json_at(doc, list, index) : list;
}

static void fill_track(const json_doc_t *doc, int obj, subsonic_track_t *t) {
	memset(t, 0, sizeof(*t));
	read_id(doc, obj, "id", t->id, sizeof(t->id));
	read_text(doc, obj, "title", t->title, sizeof(t->title));
	if (!t->title[0]) {
		read_text(doc, obj, "name", t->title, sizeof(t->title));
	}
	read_text(doc, obj, "artist", t->artist, sizeof(t->artist));
	read_text(doc, obj, "album", t->album, sizeof(t->album));
	read_id(doc, obj, "albumId", t->album_id, sizeof(t->album_id));
	read_id(doc, obj, "coverArt", t->cover_id, sizeof(t->cover_id));
	read_text(doc, obj, "suffix", t->suffix, sizeof(t->suffix));
	t->duration = (int)json_obj_long(doc, obj, "duration", 0);
	t->track_number = (int)json_obj_long(doc, obj, "track", 0);
	t->bitrate = (int)json_obj_long(doc, obj, "bitRate", 0);
	t->bit_depth = (int)json_obj_long(doc, obj, "bitDepth", 0);
	t->sample_rate = (int)json_obj_long(doc, obj, "samplingRate", 0);

	t->starred = json_get(doc, obj, "starred") >= 0;
	read_text(doc, obj, "genre", t->genre, sizeof(t->genre));
	read_text(doc, obj, "albumArtist", t->album_artist, sizeof(t->album_artist));
	t->year = (int)json_obj_long(doc, obj, "year", 0);
	t->disc = (int)json_obj_long(doc, obj, "discNumber", 0);
	int gain = json_get(doc, obj, "replayGain");
	if (gain >= 0) {
		int track_gain = json_get(doc, gain, "trackGain");
		int album_gain = json_get(doc, gain, "albumGain");
		t->has_gain = track_gain >= 0 || album_gain >= 0;
		t->track_gain = (float)json_double(doc, track_gain, 0.0);
		t->album_gain = (float)json_double(doc, album_gain, 0.0);
		t->track_peak = (float)json_double(doc, json_get(doc, gain, "trackPeak"), 0.0);
		t->album_peak = (float)json_double(doc, json_get(doc, gain, "albumPeak"), 0.0);
	}

	// The decoder is picked from the file's extension, so one has to be there
	// even when the server omits the suffix.
	if (!t->suffix[0]) {
		char type[64] = "";
		read_text(doc, obj, "contentType", type, sizeof(type));
		const char *suffix = "mp3";
		if (strstr(type, "flac")) {
			suffix = "flac";
		} else if (strstr(type, "ogg") || strstr(type, "vorbis")) {
			suffix = "ogg";
		} else if (strstr(type, "opus")) {
			suffix = "opus";
		} else if (strstr(type, "mp4") || strstr(type, "aac") || strstr(type, "m4a")) {
			suffix = "m4a";
		} else if (strstr(type, "wav")) {
			suffix = "wav";
		}
		snprintf(t->suffix, sizeof(t->suffix), "%s", suffix);
	}
	// A suffix is a file extension and ends up in a path: nothing odd in it.
	for (char *p = t->suffix; *p; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) {
			if (*p >= 'A' && *p <= 'Z') {
				*p = (char)(*p - 'A' + 'a');
			} else {
				snprintf(t->suffix, sizeof(t->suffix), "mp3");
				break;
			}
		}
	}
}

static void fill_album(const json_doc_t *doc, int obj, subsonic_album_t *a) {
	memset(a, 0, sizeof(*a));
	read_id(doc, obj, "id", a->id, sizeof(a->id));
	read_text(doc, obj, "name", a->title, sizeof(a->title));
	if (!a->title[0]) {
		read_text(doc, obj, "title", a->title, sizeof(a->title));
	}
	read_text(doc, obj, "artist", a->artist, sizeof(a->artist));
	read_id(doc, obj, "coverArt", a->cover_id, sizeof(a->cover_id));
	long year = json_obj_long(doc, obj, "year", 0);
	if (year > 0) {
		snprintf(a->year, sizeof(a->year), "%ld", year);
	}
	a->track_count = (int)json_obj_long(doc, obj, "songCount", 0);
}

static void fill_artist(const json_doc_t *doc, int obj, subsonic_artist_t *a) {
	memset(a, 0, sizeof(*a));
	read_id(doc, obj, "id", a->id, sizeof(a->id));
	read_text(doc, obj, "name", a->name, sizeof(a->name));
	read_id(doc, obj, "coverArt", a->cover_id, sizeof(a->cover_id));
	a->album_count = (int)json_obj_long(doc, obj, "albumCount", 0);
}

static void fill_playlist(const json_doc_t *doc, int obj, subsonic_playlist_t *p) {
	memset(p, 0, sizeof(*p));
	read_id(doc, obj, "id", p->id, sizeof(p->id));
	read_text(doc, obj, "name", p->name, sizeof(p->name));
	read_text(doc, obj, "owner", p->owner, sizeof(p->owner));
	read_id(doc, obj, "coverArt", p->cover_id, sizeof(p->cover_id));
	p->track_count = (int)json_obj_long(doc, obj, "songCount", 0);
}

// Reads the entries of `list`, from `offset`, at most `max`, with `fill`.
// Entries without an id are skipped: nothing can be done with them.
typedef void (*fill_fn)(const json_doc_t *doc, int obj, void *out);
static int read_list(const json_doc_t *doc, int list, int offset, void *out, size_t item_size, int max, fill_fn fill) {
	int total = list_count(doc, list);
	int written = 0;
	for (int i = offset; i < total && written < max; i++) {
		int obj = list_item(doc, list, i);
		if (obj < 0) {
			continue;
		}
		void *slot = (char *)out + (size_t)written * item_size;
		fill(doc, obj, slot);
		if (((char *)slot)[0] == '\0') {
			continue; // every record starts with its id
		}
		written++;
	}
	return written;
}

static void fill_track_v(const json_doc_t *d, int o, void *out) { fill_track(d, o, out); }
static void fill_album_v(const json_doc_t *d, int o, void *out) { fill_album(d, o, out); }
static void fill_artist_v(const json_doc_t *d, int o, void *out) { fill_artist(d, o, out); }
static void fill_playlist_v(const json_doc_t *d, int o, void *out) { fill_playlist(d, o, out); }

// ---------------------------------------------------------------------------
// requests
// ---------------------------------------------------------------------------

int subsonic_artists(int offset, subsonic_artist_t *out, int max) {
	char *body;
	json_doc_t doc;
	int response;
	if (!api_call("getArtists", NULL, &body, &doc, &response)) {
		return -1;
	}

	// {"artists":{"index":[{"name":"A","artist":[...]}, ...]}}: the letters are
	// groups, and the page runs across them.
	int groups = json_path(&doc, response, "artists.index");
	int written = 0;
	int seen = 0; // artists passed so far, in every group
	for (int g = 0; g < list_count(&doc, groups) && written < max; g++) {
		int group = list_item(&doc, groups, g);
		int list = group >= 0 ? json_get(&doc, group, "artist") : -1;
		int n = list_count(&doc, list);
		if (seen + n > offset) {
			int start = offset > seen ? offset - seen : 0;
			written += read_list(&doc, list, start, out + written, sizeof(*out), max - written, fill_artist_v);
		}
		seen += n;
	}
	json_free(&doc);
	free(body);
	return written;
}

int subsonic_artist_albums(const char *artist_id, int offset, subsonic_album_t *out, int max) {
	char args[3 * SUBSONIC_ID_MAX + 8];
	arg_text(args, sizeof(args), "id", artist_id);
	char *body;
	json_doc_t doc;
	int response;
	if (!api_call("getArtist", args, &body, &doc, &response)) {
		return -1;
	}
	int n = read_list(&doc, json_path(&doc, response, "artist.album"), offset, out, sizeof(*out), max, fill_album_v);
	json_free(&doc);
	free(body);
	return n;
}

int subsonic_album_list(const char *type, int offset, subsonic_album_t *out, int max) {
	char args[128];
	snprintf(args, sizeof(args), "type=%s&size=%d&offset=%d", type, max, offset);
	char *body;
	json_doc_t doc;
	int response;
	if (!api_call("getAlbumList2", args, &body, &doc, &response)) {
		return -1;
	}
	// The server already cut the page.
	int n = read_list(&doc, json_path(&doc, response, "albumList2.album"), 0, out, sizeof(*out), max, fill_album_v);
	json_free(&doc);
	free(body);
	return n;
}

int subsonic_album_tracks(const char *album_id, int offset, subsonic_track_t *out, int max) {
	char args[3 * SUBSONIC_ID_MAX + 8];
	arg_text(args, sizeof(args), "id", album_id);
	char *body;
	json_doc_t doc;
	int response;
	if (!api_call("getAlbum", args, &body, &doc, &response)) {
		return -1;
	}
	int n = read_list(&doc, json_path(&doc, response, "album.song"), offset, out, sizeof(*out), max, fill_track_v);
	json_free(&doc);
	free(body);
	return n;
}

int subsonic_playlists(int offset, subsonic_playlist_t *out, int max) {
	char *body;
	json_doc_t doc;
	int response;
	if (!api_call("getPlaylists", NULL, &body, &doc, &response)) {
		return -1;
	}
	int n = read_list(&doc, json_path(&doc, response, "playlists.playlist"), offset, out, sizeof(*out), max,
					  fill_playlist_v);
	json_free(&doc);
	free(body);
	return n;
}

int subsonic_playlist_tracks(const char *playlist_id, int offset, subsonic_track_t *out, int max) {
	char args[3 * SUBSONIC_ID_MAX + 8];
	arg_text(args, sizeof(args), "id", playlist_id);
	char *body;
	json_doc_t doc;
	int response;
	if (!api_call("getPlaylist", args, &body, &doc, &response)) {
		return -1;
	}
	int n = read_list(&doc, json_path(&doc, response, "playlist.entry"), offset, out, sizeof(*out), max,
					  fill_track_v);
	json_free(&doc);
	free(body);
	return n;
}

int subsonic_starred_tracks(int offset, subsonic_track_t *out, int max) {
	char *body;
	json_doc_t doc;
	int response;
	if (!api_call("getStarred2", NULL, &body, &doc, &response)) {
		return -1;
	}
	int n = read_list(&doc, json_path(&doc, response, "starred2.song"), offset, out, sizeof(*out), max, fill_track_v);
	json_free(&doc);
	free(body);
	return n;
}

// search3 pages each of its three kinds on its own; only one is asked for.
static int search(const char *query, int offset, int max, const char *kind, void *out, size_t item_size,
				  fill_fn fill) {
	char q[3 * SUBSONIC_TITLE_MAX + 8];
	arg_text(q, sizeof(q), "query", query);
	const char *kinds[3] = {"artist", "album", "song"};
	char args[3 * SUBSONIC_TITLE_MAX + 160];
	int used = snprintf(args, sizeof(args), "%s", q);
	for (int i = 0; i < 3; i++) {
		int count = strcmp(kinds[i], kind) == 0 ? max : 0;
		int off = strcmp(kinds[i], kind) == 0 ? offset : 0;
		char name_count[24], name_offset[24];
		snprintf(name_count, sizeof(name_count), "%sCount", kinds[i]);
		snprintf(name_offset, sizeof(name_offset), "%sOffset", kinds[i]);
		used += snprintf(args + used, sizeof(args) - (size_t)used, "&%s=%d&%s=%d", name_count, count, name_offset, off);
	}

	char *body;
	json_doc_t doc;
	int response;
	if (!api_call("search3", args, &body, &doc, &response)) {
		return -1;
	}
	char path[40];
	snprintf(path, sizeof(path), "searchResult3.%s", kind);
	int n = read_list(&doc, json_path(&doc, response, path), 0, out, item_size, max, fill);
	json_free(&doc);
	free(body);
	return n;
}

int subsonic_search_tracks(const char *query, int offset, subsonic_track_t *out, int max) {
	return search(query, offset, max, "song", out, sizeof(*out), fill_track_v);
}

int subsonic_search_albums(const char *query, int offset, subsonic_album_t *out, int max) {
	return search(query, offset, max, "album", out, sizeof(*out), fill_album_v);
}

int subsonic_search_artists(const char *query, int offset, subsonic_artist_t *out, int max) {
	return search(query, offset, max, "artist", out, sizeof(*out), fill_artist_v);
}

// ---------------------------------------------------------------------------
// writing back
// ---------------------------------------------------------------------------

static bool simple_call(const char *method, const char *args) {
	char *body;
	json_doc_t doc;
	int response;
	if (!api_call(method, args, &body, &doc, &response)) {
		return false;
	}
	json_free(&doc);
	free(body);
	return true;
}

bool subsonic_star(const char *track_id, bool on) {
	char args[3 * SUBSONIC_ID_MAX + 8];
	arg_text(args, sizeof(args), "id", track_id);
	return simple_call(on ? "star" : "unstar", args);
}

bool subsonic_scrobble(const char *track_id) {
	char id[3 * SUBSONIC_ID_MAX + 8];
	arg_text(id, sizeof(id), "id", track_id);
	char args[3 * SUBSONIC_ID_MAX + 40];
	snprintf(args, sizeof(args), "%s&submission=true", id);
	return simple_call("scrobble", args);
}

// ---------------------------------------------------------------------------
// addresses
// ---------------------------------------------------------------------------

bool subsonic_stream_url(const char *track_id, char *out, size_t size) {
	if (!subsonic_logged_in() || !track_id || !*track_id) {
		return false;
	}
	char args[3 * SUBSONIC_ID_MAX + 32];
	char id[3 * SUBSONIC_ID_MAX + 8];
	arg_text(id, sizeof(id), "id", track_id);
	snprintf(args, sizeof(args), "%s&format=raw", id);
	return build_url(server_url, user_name, salt, token, "stream", args, out, size);
}

bool subsonic_cover_url(const char *cover_id, int pixels, char *out, size_t size) {
	if (!subsonic_logged_in() || !cover_id || !*cover_id) {
		return false;
	}
	char args[3 * SUBSONIC_ID_MAX + 32];
	char id[3 * SUBSONIC_ID_MAX + 8];
	arg_text(id, sizeof(id), "id", cover_id);
	int n = snprintf(args, sizeof(args), "%s", id);
	if (pixels > 0) {
		char sz[24];
		arg_int(sz, sizeof(sz), "size", pixels);
		snprintf(args + n, sizeof(args) - (size_t)n, "&%s", sz);
	}
	return build_url(server_url, user_name, salt, token, "getCoverArt", args, out, size);
}
