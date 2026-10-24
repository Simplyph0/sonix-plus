#ifndef SUBSONICCACHE_H
#define SUBSONICCACHE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/subsonic.h"

// Subsonic tracks downloaded to the card and then played like any other file.
//
// The reasons are the ones qobuzcache.h gives for Qobuz: the decoders want a
// path, seeking needs a file, and with the file on disk the queue, the
// equaliser and ReplayGain need no extra code. Playback starts while the track
// is still coming down (growfile.c), and the file is dropped when the player
// shuts down.
//
// What differs is the key. A Qobuz track is a number; a Subsonic id is text
// (Navidrome's are 22 characters), so files are named after the id with
// anything that is not a letter, a digit, '-' or '_' replaced by '_':
//
//     <card>/.local/subsonic-cache/<key>.<suffix>
//     <card>/.local/subsonic-cache/<key>.<suffix>.tags     title, artist, album
//     <card>/.local/subsonic-cache/<key>.jpg               the cover
//
// The download turn, the prebuffer and the abandoning are shared with the other
// services (streamturn.h): one network, one card.

#define SUBSONICCACHE_DIR ".local/subsonic-cache"

// Small enough not to silently fill a 32 GB card; the cache is a waypoint.
#define SUBSONICCACHE_MAX_BYTES (768L * 1024L * 1024L)

#define SUBSONICCACHE_KEY_MAX 64

// How many tracks between automatic sweeps.
#define SUBSONICCACHE_CLEAR_EVERY 10

// How many paths can be protected from the sweep.
#define SUBSONICCACHE_PROTECTED_MAX 64

// Where the card is. Called at startup and on every card change; NULL or empty
// disables the cache (and with it Subsonic playback).
void subsoniccache_set_root(const char *sd_root);
bool subsoniccache_ready(void);

// The file-name form of a track id.
void subsoniccache_key(const char *track_id, char *out, size_t size);

// The path this track would have. Says nothing about whether it exists.
void subsoniccache_path(const char *track_id, const char *suffix, char *out, size_t size);

// True if the track has been downloaded in full. `out` may be NULL.
bool subsoniccache_has(const char *track_id, const char *suffix, char *out, size_t size);

// The key of the track a cache path belongs to, or false if the path is not
// from this cache.
bool subsoniccache_key_from_path(const char *path, char *out, size_t size);

// Starts downloading `url` into the cache and puts the local path in `out`,
// as soon as there is enough for the decoder to open it (see qobuzcache_start
// for the prebuffer, which is the same). Blocks for that long: call from a
// worker thread. `duration_secs` is the track's length (0 when unknown).
bool subsoniccache_start(const char *track_id, const char *url, const char *suffix, int duration_secs, char *out,
						 size_t size);

// Waits until nothing is downloading, without taking the turn.
void subsoniccache_wait_idle(void);

// Abandons downloads in progress.
void subsoniccache_abandon_all(void);

// True while this track is coming down; and while any is.
bool subsoniccache_downloading(const char *track_id);
bool subsoniccache_any_downloading(void);

// Called when the prebuffer will take more than a moment, with the estimate in
// seconds. Called from the download thread.
void subsoniccache_set_slow_start_cb(void (*cb)(int seconds));

// "Subsonic needs the network", even when nothing is downloading at this
// instant -- see qobuzcache_set_network_wanted for the gap this covers.
void subsoniccache_set_network_wanted(bool wanted);
bool subsoniccache_network_wanted(void);

// True if `path` is inside the Subsonic cache.
bool subsoniccache_owns(const char *path);
const char *subsoniccache_dir(void);

// Writes beside the track what the API knows and the file does not: its tags,
// year, genre and ReplayGain (in `<file>.tags`, read by metadata.c) and, when
// `cover_url` is given, the cover (in `<key>.jpg`, found by the cover loader).
void subsoniccache_write_sidecars(const subsonic_track_t *track, const char *cover_url);

// The album id the track belongs to, read back from the sidecar.
bool subsoniccache_album_id(const char *path, char *out, size_t size);

// Why the last subsoniccache_start() failed, as a sentence fit to display.
// Per thread.
const char *subsoniccache_last_error(void);

// Call for every track that enters the cache, with its path. Sweeps every
// SUBSONICCACHE_CLEAR_EVERY calls.
void subsoniccache_note_played(const char *path);

// The tracks the queue still has to play: the sweep skips them.
void subsoniccache_set_protected(const char *const *paths, int count);

// Call on the way out (shutdown, reboot): the cache does not survive a power
// cycle.
void subsoniccache_clear_on_exit(void);

long subsoniccache_bytes(void);
void subsoniccache_clear(void);

#endif /* SUBSONICCACHE_H */
