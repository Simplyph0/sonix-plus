#ifndef SUBSONIC_H
#define SUBSONIC_H

#include <stdbool.h>
#include <stddef.h>

// Subsonic, and the servers that speak its API: Navidrome, Airsonic, Gonic,
// Ampache and the rest.
//
// Unlike Qobuz and Tidal there is no company behind it and no application key:
// the user's own server is the service, so the page asks for its address, a
// user name and a password.
//
// The password is used once, to work out the token the API asks for with every
// request, md5(password + salt), and is not kept. What is stored is the server
// address, the user name, the salt and the token. The token is as good as the
// password for that one server and nothing else, which is the same trade the
// Qobuz session token makes.
//
// Every function that talks to the network blocks. Call them from a worker
// thread, never from the GUI thread -- the same rule as http.h.

#define SUBSONIC_ID_MAX 64
#define SUBSONIC_TITLE_MAX 200
#define SUBSONIC_NAME_MAX 128
#define SUBSONIC_URL_MAX 768

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char title[SUBSONIC_TITLE_MAX];
	char artist[SUBSONIC_NAME_MAX];
	char album[SUBSONIC_TITLE_MAX];
	char album_id[SUBSONIC_ID_MAX]; // to reopen the album the track comes from
	char cover_id[SUBSONIC_ID_MAX];
	char suffix[8]; // "flac", "mp3"...: names the file, and picks its decoder
	int duration;	// seconds
	int track_number;
	int bitrate;	 // kbit/s, 0 when the server does not say
	int bit_depth;	 // 0 when the server does not say
	int sample_rate; // Hz, 0 when the server does not say
} subsonic_track_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char title[SUBSONIC_TITLE_MAX];
	char artist[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	char year[8];
	int track_count;
} subsonic_album_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char name[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	int album_count;
} subsonic_artist_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char name[SUBSONIC_TITLE_MAX];
	char owner[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	int track_count;
} subsonic_playlist_t;

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

// Reads the stored server and token. Once at startup, after config_init().
void subsonic_init(void);

bool subsonic_logged_in(void);
const char *subsonic_server(void);	 // "http://host:4533", empty when not signed in
const char *subsonic_user(void);	 // empty when not signed in

// Blocks. `server` may be typed as "host:4533" or "https://host/music": a
// missing scheme means http://. Pings the server with the new token and, only
// if it answers, stores the session.
bool subsonic_login(const char *server, const char *user, const char *password);

// Forgets the session, here and in the configuration.
void subsonic_logout(void);

// Why the last call failed, already in a form fit to display. Empty on
// success. Per thread, like http_last_error().
const char *subsonic_last_error(void);

// ---------------------------------------------------------------------------
// Requests. All return how many items they wrote, or -1 on error
// (`subsonic_last_error()` says what happened). All block.
//
// `offset` and `max` page the result. Subsonic itself pages only the album
// lists and the search; for the rest the whole answer is read and the page cut
// out of it, which costs one request per page but keeps the caller uniform.
// ---------------------------------------------------------------------------

int subsonic_artists(int offset, subsonic_artist_t *out, int max);
int subsonic_artist_albums(const char *artist_id, int offset, subsonic_album_t *out, int max);

// `type` is the server's own: "newest", "recent", "frequent", "random",
// "alphabeticalByName", "alphabeticalByArtist", "starred".
int subsonic_album_list(const char *type, int offset, subsonic_album_t *out, int max);

int subsonic_album_tracks(const char *album_id, int offset, subsonic_track_t *out, int max);

int subsonic_playlists(int offset, subsonic_playlist_t *out, int max);
int subsonic_playlist_tracks(const char *playlist_id, int offset, subsonic_track_t *out, int max);

int subsonic_starred_tracks(int offset, subsonic_track_t *out, int max);

int subsonic_search_tracks(const char *query, int offset, subsonic_track_t *out, int max);
int subsonic_search_albums(const char *query, int offset, subsonic_album_t *out, int max);
int subsonic_search_artists(const char *query, int offset, subsonic_artist_t *out, int max);

// ---------------------------------------------------------------------------
// Addresses
//
// Built here and not by a request: they carry the credentials in their query,
// so nothing that holds one may log or store it. The audio is asked for as it
// is on the server (format=raw): the server transcodes only when told to, and
// a DAP wants the original file.
// ---------------------------------------------------------------------------

bool subsonic_stream_url(const char *track_id, char *out, size_t size);
bool subsonic_cover_url(const char *cover_id, int pixels, char *out, size_t size);

#endif /* SUBSONIC_H */
