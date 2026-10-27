#include "gamesound.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"

#define RATE 44100
#define MIX_FRAMES 441 // 10 ms
#define BUFFER_MS 46
#define VOICES 4

typedef enum { W_SQUARE, W_TRI, W_NOISE } wave_t;

// One note: the pitch slides from `f0` to `f1` Hz over `ms`.
typedef struct {
	uint16_t f0;
	uint16_t f1;
	uint16_t ms;
	uint8_t wave;
	uint8_t vol; // 0..100
} note_t;

#define END {0, 0, 0, 0, 0}
#define MAX_NOTES 6

static const note_t EFFECTS[SND_COUNT][MAX_NOTES] = {
	[SND_BLIP] = {{880, 880, 40, W_SQUARE, 45}, END},
	[SND_TICK] = {{520, 520, 18, W_SQUARE, 30}, END},
	[SND_EAT] = {{660, 660, 45, W_SQUARE, 45}, {990, 990, 70, W_SQUARE, 45}, END},
	[SND_HIT] = {{330, 330, 35, W_SQUARE, 50}, END},
	[SND_BRICK] = {{1040, 1040, 30, W_SQUARE, 45}, {780, 780, 30, W_TRI, 50}, END},
	[SND_MERGE] = {{440, 700, 70, W_TRI, 70}, END},
	[SND_LINE] = {{523, 523, 60, W_SQUARE, 40}, {659, 659, 60, W_SQUARE, 40}, {784, 784, 60, W_SQUARE, 40}, {1047, 1047, 110, W_SQUARE, 40}, END},
	[SND_DROP] = {{180, 90, 60, W_TRI, 80}, END},
	[SND_ROTATE] = {{600, 760, 30, W_SQUARE, 28}, END},
	[SND_FLAG] = {{500, 500, 30, W_TRI, 60}, {750, 750, 50, W_TRI, 60}, END},
	[SND_REVEAL] = {{300, 300, 22, W_NOISE, 25}, END},
	[SND_BOOM] = {{200, 40, 380, W_NOISE, 90}, END},
	[SND_WIN] = {{523, 523, 90, W_SQUARE, 40}, {659, 659, 90, W_SQUARE, 40}, {784, 784, 90, W_SQUARE, 40}, {1047, 1047, 220, W_SQUARE, 40}, END},
	[SND_LOSE] = {{392, 392, 140, W_TRI, 70}, {330, 330, 140, W_TRI, 70}, {262, 196, 320, W_TRI, 70}, END},
	[SND_ERROR] = {{160, 160, 90, W_SQUARE, 40}, END},
	[SND_PAUSE] = {{700, 500, 70, W_TRI, 55}, END},
};

typedef struct {
	int snd;	  // -1: free
	int note;	  // index in the effect
	int pos;	  // frames into the note
	uint32_t phase; // 16.16 fraction of a cycle
	uint32_t noise;
} voice_t;

static voice_t voices[VOICES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mixer;
static bool mixer_live;
static volatile bool stop_flag;
static volatile bool thread_alive;
static volatile bool gave_up; // the output is someone else's: not asked for again until the game is reopened
static bool setting_loaded;
static bool setting_on = true;

static bool opened;
static void start_thread(void);
static bool opened_flag(void) { return opened; }

bool gamesound_enabled(void) {
	if (!setting_loaded) {
		setting_on = config_get_int("games", "sound", 1) != 0;
		setting_loaded = true;
	}
	return setting_on;
}

void gamesound_set_enabled(bool on) {
	setting_on = on;
	setting_loaded = true;
	config_set_int("games", "sound", on ? 1 : 0);
	config_save();
	if (on && opened_flag()) {
		gave_up = false;
		if (!thread_alive) {
			start_thread();
		}
	}
}

static int16_t sample_of(voice_t *v, const note_t *n) {
	int total = (int)((int64_t)n->ms * RATE / 1000);
	int freq = n->f0 + (int)((int64_t)(n->f1 - n->f0) * v->pos / (total ? total : 1));
	if (freq < 1) {
		freq = 1;
	}
	v->phase += (uint32_t)((uint64_t)freq * 65536 / RATE);
	uint32_t ph = v->phase & 0xFFFF;
	int32_t s;
	switch (n->wave) {
	case W_TRI:
		s = ph < 32768 ? (int32_t)ph * 2 - 32768 : 32767 - ((int32_t)ph - 32768) * 2;
		break;
	case W_NOISE:
		// A new random value only each time the phase wraps, so the pitch still
		// means something: low for a rumble, high for a hiss.
		if ((v->phase >> 16) != ((v->phase - (uint32_t)((uint64_t)freq * 65536 / RATE)) >> 16)) {
			v->noise = v->noise * 1664525u + 1013904223u;
		}
		s = (int32_t)(v->noise >> 16) - 32768;
		break;
	default:
		s = ph < 32768 ? 32767 : -32768;
		break;
	}
	// A short fade at the end of the note takes the click out.
	int left = total - v->pos;
	int fade = RATE / 400;
	int32_t amp = n->vol * 32767 / 100 / 2;
	if (left < fade) {
		amp = amp * left / fade;
	}
	return (int16_t)(s * amp / 32768);
}

static void mix_into(int32_t *mix) {
	memset(mix, 0, sizeof(int32_t) * MIX_FRAMES);
	pthread_mutex_lock(&lock);
	for (int i = 0; i < VOICES; i++) {
		voice_t *v = &voices[i];
		if (v->snd < 0) {
			continue;
		}
		for (int f = 0; f < MIX_FRAMES && v->snd >= 0; f++) {
			const note_t *n = &EFFECTS[v->snd][v->note];
			if (n->ms == 0) {
				v->snd = -1;
				break;
			}
			mix[f] += sample_of(v, n);
			if (++v->pos >= (int)((int64_t)n->ms * RATE / 1000)) {
				v->note++;
				v->pos = 0;
				if (v->note >= MAX_NOTES || EFFECTS[v->snd][v->note].ms == 0) {
					v->snd = -1;
				}
			}
		}
	}
	pthread_mutex_unlock(&lock);
}

// Opens the output and mixes until told to stop, or until the output is wanted
// back. Off the interface thread: asking another source to let go takes a while.
static void *mixer_main(void *unused) {
	(void)unused;
	if (stop_flag || !audio_external_begin_yielding(RATE, 2, 16, BUFFER_MS)) {
		gave_up = true; // music is playing, or no PCM: the game is silent
		thread_alive = false;
		return NULL;
	}

	int16_t out[MIX_FRAMES * 2];
	int32_t mix[MIX_FRAMES];
	while (!stop_flag) {
		if (audio_external_wanted()) {
			gave_up = true; // a track was started: it has the output
			break;
		}
		mix_into(mix);
		for (int i = 0; i < MIX_FRAMES; i++) {
			int32_t v = mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i];
			out[i * 2] = (int16_t)v;
			out[i * 2 + 1] = (int16_t)v;
		}
		audio_external_write(out, MIX_FRAMES);
	}
	audio_external_end();
	thread_alive = false;
	return NULL;
}

static void start_thread(void) {
	if (mixer_live) {
		pthread_join(mixer, NULL);
		mixer_live = false;
	}
	stop_flag = false;
	thread_alive = true;
	if (pthread_create(&mixer, NULL, mixer_main, NULL) == 0) {
		mixer_live = true;
	} else {
		thread_alive = false;
	}
}

void gamesound_open(void) {
	opened = true;
	gave_up = false;
	for (int i = 0; i < VOICES; i++) {
		voices[i].snd = -1;
		voices[i].noise = 12345u + (uint32_t)i;
	}
	if (gamesound_enabled()) {
		start_thread();
	}
}

void gamesound_play(snd_t snd) {
	if (!opened || snd < 0 || snd >= SND_COUNT || !gamesound_enabled()) {
		return;
	}
	pthread_mutex_lock(&lock);
	int slot = -1;
	for (int i = 0; i < VOICES; i++) {
		if (voices[i].snd < 0) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		slot = 0; // all busy: the oldest effect gives way
	}
	voices[slot].snd = snd;
	voices[slot].note = 0;
	voices[slot].pos = 0;
	voices[slot].phase = 0;
	pthread_mutex_unlock(&lock);

	// The thread is gone when the output was taken (gave_up) or never opened.
	if (!thread_alive && !gave_up) {
		start_thread();
	}
}

void gamesound_close(void) {
	opened = false;
	if (mixer_live) {
		stop_flag = true;
		pthread_join(mixer, NULL);
		mixer_live = false;
	}
}
