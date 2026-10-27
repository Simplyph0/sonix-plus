#ifndef GUI_GAMESOUND_H
#define GUI_GAMESOUND_H

#include <stdbool.h>

// Sound effects for the games, made on the spot from a few notes each: no files,
// nothing to load. A thread mixes whatever is sounding into the external PCM
// (audio.h) from gamesound_open() to gamesound_close().
//
// The output is borrowed the way the metronome borrows it: a game that finds music
// playing stays quiet and leaves the music alone, and a track started while the
// game is open takes the output back. Sound is on unless the player turned it
// off in a game's pause menu (the setting is kept).

typedef enum {
	SND_BLIP,	// a menu tap, a cell filled
	SND_TICK,	// a small step: a piece moving, a digit typed
	SND_EAT,	// a point scored
	SND_HIT,	// a ball against a wall or paddle
	SND_BRICK,	// a brick
	SND_MERGE,	// two tiles joined
	SND_LINE,	// a line cleared
	SND_DROP,	// a piece landing
	SND_ROTATE, // a piece turned
	SND_FLAG,	// a flag put down
	SND_REVEAL, // a cell opened
	SND_BOOM,	// a mine, a hit that hurts
	SND_WIN,	// a level, a game won
	SND_LOSE,	// a game lost
	SND_ERROR,	// not allowed
	SND_PAUSE,	// pause and resume
	SND_COUNT,
} snd_t;

// Whether effects are wanted (the setting, not whether the output could be had).
bool gamesound_enabled(void);
void gamesound_set_enabled(bool on);

// Starts the mixer thread and returns at once. Nothing happens, and nothing is
// taken from the music, while sound is off.
void gamesound_open(void);

// Starts `snd` from its beginning, over whatever else is sounding. Any thread.
void gamesound_play(snd_t snd);

// Stops the thread and gives the output back.
void gamesound_close(void);

#endif /* GUI_GAMESOUND_H */
