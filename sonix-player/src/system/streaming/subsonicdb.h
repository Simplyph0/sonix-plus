#ifndef SUBSONICDB_H
#define SUBSONICDB_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/library/library.h"
#include "src/system/streaming/subsonic.h"

// The songs of the Subsonic server, kept on the card.
//
// A server answers a few dozen items at a time, which is no way to browse ten
// thousand songs. So the whole catalogue is read once, in pages, into a small
// database of its own and every list is made from that: instantly, sorted the
// way the local library sorts, with the A-Z strip, searchable, and still there
// with the network off. The player's library is left alone on purpose -- its
// tables are rebuilt and pruned by every scan, and rows for files that are not
// on the card would not survive one.
//
//     <card>/.local/subsonic.db
//
// Unlike the download cache this is kept across restarts. It belongs to one
// server and one user: signing in to another wipes it.
//
// Every function is safe from any thread. Those that read are quick; the sync
// runs on a thread of its own.

typedef enum {
	SDB_SONGS_ALL,	  // every song, by title
	SDB_SONGS_ARTIST, // one artist's songs; value = the artist
	SDB_SONGS_ALBUM,  // one album, in running order; value = the album id
	SDB_SONGS_GENRE,  // one genre; value = the genre
	SDB_SONGS_SEARCH, // title, artist or album contains value
	SDB_SONGS_RECENT, // newest first
	SDB_SONGS_STARRED, // the ones starred on the server
} sdb_filter_t;

typedef enum {
	SDB_NAMES_ARTISTS,
	SDB_NAMES_ALBUMS,
	SDB_NAMES_GENRES,
} sdb_names_t;

typedef struct {
	char name[SUBSONIC_TITLE_MAX]; // the artist, the album or the genre
	char sub[SUBSONIC_NAME_MAX];   // an album's artist
	char value[SUBSONIC_ID_MAX];   // what to pass back as the filter value
	char cover_id[SUBSONIC_ID_MAX];
	int count; // songs
} sdb_name_t;

// Opens (creating it if need be) the database on the card at `sd_root`. Called
// at startup and when the card changes; NULL or empty closes it. If the
// database belongs to another server or user it is emptied.
void sdb_open(const char *sd_root);
bool sdb_ready(void);

// Songs held.
int sdb_total(void);

// Songs in a list, and a window of them. `value` is the filter's argument
// (NULL for ALL and RECENT). Both return -1 when the database is not there.
int sdb_song_count(sdb_filter_t filter, const char *value);
int sdb_songs(sdb_filter_t filter, const char *value, int offset, int count, subsonic_track_t *out);

// Artists, albums or genres, in the order the library lists them, with how many
// songs each holds.
int sdb_name_count(sdb_names_t kind);
int sdb_names(sdb_names_t kind, int offset, int count, sdb_name_t *out);

// Where each bucket of the A-Z strip starts in a song list sorted by title
// (SDB_SONGS_ALL with no value, or a genre), -1 for an empty one.
bool sdb_song_buckets(sdb_filter_t filter, const char *value, int first[LIBRARY_INDEX_BUCKETS]);
bool sdb_name_buckets(sdb_names_t kind, int first[LIBRARY_INDEX_BUCKETS]);

// A song by the key its cache file is named after.
bool sdb_find(const char *key, subsonic_track_t *out);

// Adds songs a list came with that the sync has not brought in yet (a server
// playlist, say), so that a queue made from them can always be looked up. Does
// not touch those already there.
void sdb_remember(const subsonic_track_t *tracks, int count);

// Whether the server has this song starred, as of the last sync or change.
bool sdb_starred(const char *key);
void sdb_set_starred(const char *key, bool on);

// ---------------------------------------------------------------------------
// By path
//
// The player knows a playing song by the path of its file in the cache. These
// let its star and its play count reach the server without it knowing more than
// that.
// ---------------------------------------------------------------------------

// Whether the song at `path` is starred; false for a path that is not one of
// ours.
bool sdb_path_starred(const char *path);

// Stars or unstars it on the server. The change shows at once and is taken back
// if the server refuses; `done` is called from another thread when it knows,
// with the reason in `error` on failure. `done` may be NULL.
void sdb_path_toggle_star(const char *path, void (*done)(bool ok, const char *error, void *user), void *user);

// Tells the server the song was played. Does nothing for a path that is not
// one of ours.
void sdb_path_scrobble(const char *path);

// ---------------------------------------------------------------------------
// Sync
// ---------------------------------------------------------------------------

// Reads the catalogue from the server on a thread of its own. False when one is
// already running or nothing can be done (not signed in, no database).
bool sdb_sync_start(void);
bool sdb_sync_running(void);
int sdb_sync_found(void); // songs read so far
const char *sdb_sync_error(void); // empty unless the last sync failed
long sdb_last_sync(void);			// when the last one finished, 0 if never

// Called on the sync's thread when it ends, with whether it worked. For the
// page to rebuild its lists; post to the GUI thread from it.
void sdb_set_sync_listener(void (*cb)(bool ok));

#endif /* SUBSONICDB_H */
