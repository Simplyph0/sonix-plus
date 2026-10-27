#ifndef _GNU_SOURCE
#define _GNU_SOURCE // pipe2, F_SETPIPE_SZ
#endif

#include "applaunch.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/gui/shell/gui.h"
#include "src/system/audio/audio.h"
#include "src/system/core/panel.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"

extern char **environ;

#define AUDIO_FD 3
// A pipe this small holds about 23 ms of 44.1 kHz stereo: the app cannot run
// far ahead of the sound card, which is what paces it and keeps the sound in
// step with what it draws.
#define AUDIO_PIPE_BYTES 4096
// The output's own buffer: deep enough not to underrun, shallow enough that a
// game's button press is heard now.
#define AUDIO_BUFFER_MS 46

typedef struct {
	pid_t pid;
	int audio_read;
	void (*on_exit)(void *);
	void *user;
	bool killed;
} run_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool running;
static pid_t running_pid = -1;

bool apps_running(void) {
	pthread_mutex_lock(&lock);
	bool r = running;
	pthread_mutex_unlock(&lock);
	return r;
}

void apps_kill(void) {
	pthread_mutex_lock(&lock);
	if (running && running_pid > 0) {
		kill(-running_pid, SIGKILL);
	}
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// The worker: the app's sound, then its end
// ---------------------------------------------------------------------------

static bool read_full(int fd, void *buf, size_t n) {
	uint8_t *p = buf;
	while (n > 0) {
		ssize_t r = read(fd, p, n);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			return false;
		}
		p += r;
		n -= (size_t)r;
	}
	return true;
}

// The sound the app writes, played through the player's own output until the app
// closes the pipe -- which it does by exiting, or never having written.
static void play_audio(int fd) {
	apps_audio_header_t header;
	if (!read_full(fd, &header, sizeof(header)) || memcmp(header.magic, APPS_AUDIO_MAGIC, 4) != 0) {
		return;
	}
	if (header.rate < 8000 || header.rate > 192000 || header.channels < 1 || header.channels > 2) {
		fprintf(stderr, "apps: the app's sound header says %u Hz, %u channels; no sound\n", header.rate,
				header.channels);
		return;
	}
	if (!audio_external_begin_latency((int)header.rate, (int)header.channels, 16, AUDIO_BUFFER_MS)) {
		fprintf(stderr, "apps: no sound output for the app\n");
		return;
	}
	const size_t frame = header.channels * sizeof(int16_t);
	uint8_t buf[AUDIO_PIPE_BYTES];
	size_t held = 0;
	for (;;) {
		ssize_t r = read(fd, buf + held, sizeof(buf) - held);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			break;
		}
		held += (size_t)r;
		size_t frames = held / frame;
		if (frames > 0) {
			audio_external_write(buf, (int)frames);
			size_t used = frames * frame;
			memmove(buf, buf + used, held - used);
			held -= used;
		}
	}
	audio_external_end();
}

static void finish_cb(void *arg) {
	run_t *run = arg;
	panel_app_end();
	power_hold_screen_on(false);

	pthread_mutex_lock(&lock);
	running = false;
	running_pid = -1;
	pthread_mutex_unlock(&lock);

	printf("apps: the app has ended\n");
	if (run->on_exit) {
		run->on_exit(run->user);
	}
	free(run);
}

static void *worker_main(void *arg) {
	run_t *run = arg;
	play_audio(run->audio_read);
	close(run->audio_read);

	int status = 0;
	waitpid(run->pid, &status, 0);
	if (WIFEXITED(status)) {
		printf("apps: exit status %d\n", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		printf("apps: ended by signal %d\n", WTERMSIG(status));
	}
	// Anything the app left running in its group goes with it.
	kill(-run->pid, SIGKILL);

	if (!gui_post(finish_cb, run)) {
		// The queue was full: the interface has to be given the panel back
		// somehow, so try again a moment later rather than leave it dark.
		for (int i = 0; i < 50 && !gui_post(finish_cb, run); i++) {
			usleep(100 * 1000);
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Starting it
// ---------------------------------------------------------------------------

// The player's environment with the app's own on top.
static char **build_env(const app_t *app, const char *rom, int width, int height, bool rotated) {
	size_t count = 0;
	while (environ && environ[count]) {
		count++;
	}
	char **env = calloc(count + 16, sizeof(char *));
	if (!env) {
		return NULL;
	}
	size_t n = 0;
	for (size_t i = 0; i < count; i++) {
		if (strncmp(environ[i], "SONIX_", 6) != 0) {
			env[n++] = strdup(environ[i]);
		}
	}
	char line[900];
	env[n++] = strdup("SONIX_API=1");
	snprintf(line, sizeof(line), "SONIX_APP_DIR=%s", app->dir);
	env[n++] = strdup(line);
	snprintf(line, sizeof(line), "SONIX_SD=%s", storage_sd_root() ? storage_sd_root() : "");
	env[n++] = strdup(line);
	env[n++] = strdup("SONIX_FB=/dev/fb0");
	snprintf(line, sizeof(line), "SONIX_WIDTH=%d", width);
	env[n++] = strdup(line);
	snprintf(line, sizeof(line), "SONIX_HEIGHT=%d", height);
	env[n++] = strdup(line);
	snprintf(line, sizeof(line), "SONIX_ROTATED=%d", rotated ? 1 : 0);
	env[n++] = strdup(line);
	const char *touch = panel_touch_device();
	snprintf(line, sizeof(line), "SONIX_TOUCH=%s", touch ? touch : "");
	env[n++] = strdup(line);
	snprintf(line, sizeof(line), "SONIX_AUDIO_FD=%d", AUDIO_FD);
	env[n++] = strdup(line);
	if (rom && rom[0]) {
		snprintf(line, sizeof(line), "SONIX_ROM=%s", rom);
		env[n++] = strdup(line);
	}
	env[n] = NULL;
	return env;
}

static void free_env(char **env) {
	for (size_t i = 0; env && env[i]; i++) {
		free(env[i]);
	}
	free(env);
}

bool apps_launch(const app_t *app, const char *rom, void (*on_exit)(void *), void *user) {
	if (!app || !app->exec[0]) {
		return false;
	}
	pthread_mutex_lock(&lock);
	if (running) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	running = true;
	pthread_mutex_unlock(&lock);

	int width = 480;
	int height = 720;
	bool rotated = false;
	panel_app_geometry(&width, &height, &rotated);

	int audio[2];
	if (pipe2(audio, O_CLOEXEC) != 0) {
		pthread_mutex_lock(&lock);
		running = false;
		pthread_mutex_unlock(&lock);
		return false;
	}
	fcntl(audio[1], F_SETPIPE_SZ, AUDIO_PIPE_BYTES);

	char **env = build_env(app, rom, width, height, rotated);
	run_t *run = calloc(1, sizeof(*run));
	if (!env || !run) {
		free_env(env);
		free(run);
		close(audio[0]);
		close(audio[1]);
		pthread_mutex_lock(&lock);
		running = false;
		pthread_mutex_unlock(&lock);
		return false;
	}

	// The music stops: the sound output has one writer, and it is about to be the
	// app's.
	audio_stop();
	power_hold_screen_on(true);
	panel_app_begin();

	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attr;
	posix_spawn_file_actions_init(&actions);
	posix_spawnattr_init(&attr);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, audio[1], AUDIO_FD);

	// Whatever else the player has open is not the app's to hold on to.
	long max_fd = sysconf(_SC_OPEN_MAX);
	if (max_fd < 0 || max_fd > 1024) {
		max_fd = 1024;
	}
	for (int fd = AUDIO_FD + 1; fd < max_fd; fd++) {
		if (fd != audio[0] && fd != audio[1] && fcntl(fd, F_GETFD) >= 0) {
			posix_spawn_file_actions_addclose(&actions, fd);
		}
	}

	sigset_t none;
	sigset_t defaults;
	sigemptyset(&none);
	sigemptyset(&defaults);
	sigaddset(&defaults, SIGPIPE);
	sigaddset(&defaults, SIGINT);
	sigaddset(&defaults, SIGTERM);
	sigaddset(&defaults, SIGHUP);
	sigaddset(&defaults, SIGUSR1);
	sigaddset(&defaults, SIGUSR2);
	sigaddset(&defaults, SIGALRM);
	posix_spawnattr_setsigmask(&attr, &none);
	posix_spawnattr_setsigdefault(&attr, &defaults);
	posix_spawnattr_setpgroup(&attr, 0);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);

	// The app starts in its own folder: a shell does the cd, since posix_spawn
	// cannot on this libc, and execs the program in its place.
	char *argv[6];
	int argc = 0;
	argv[argc++] = "sh";
	argv[argc++] = "-c";
	argv[argc++] = "cd \"$1\" || exit 127; shift; exec \"$@\"";
	argv[argc++] = "sh";
	argv[argc++] = (char *)app->dir;
	// the program, and the file it is to open
	char program[sizeof(app->exec)];
	snprintf(program, sizeof(program), "%s", app->exec);
	char *args[4];
	int nargs = 0;
	args[nargs++] = program;
	char rom_arg[1024];
	if (rom && rom[0]) {
		snprintf(rom_arg, sizeof(rom_arg), "%s", rom);
		args[nargs++] = rom_arg;
	}
	char *full_argv[12];
	int full = 0;
	for (int i = 0; i < argc; i++) {
		full_argv[full++] = argv[i];
	}
	for (int i = 0; i < nargs; i++) {
		full_argv[full++] = args[i];
	}
	full_argv[full] = NULL;

	pid_t pid;
	int rc = posix_spawn(&pid, "/bin/sh", &actions, &attr, full_argv, env);
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attr);
	free_env(env);
	close(audio[1]);

	if (rc != 0) {
		fprintf(stderr, "apps: cannot start %s: %s\n", app->exec, strerror(rc));
		close(audio[0]);
		free(run);
		panel_app_end();
		power_hold_screen_on(false);
		pthread_mutex_lock(&lock);
		running = false;
		pthread_mutex_unlock(&lock);
		return false;
	}

	run->pid = pid;
	run->audio_read = audio[0];
	run->on_exit = on_exit;
	run->user = user;
	pthread_mutex_lock(&lock);
	running_pid = pid;
	pthread_mutex_unlock(&lock);
	printf("apps: started %s (pid %d)%s%s\n", app->name, (int)pid, rom ? " with " : "", rom ? rom : "");

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, run) != 0) {
		kill(-pid, SIGKILL);
		waitpid(pid, NULL, 0);
		close(audio[0]);
		free(run);
		panel_app_end();
		power_hold_screen_on(false);
		pthread_mutex_lock(&lock);
		running = false;
		pthread_mutex_unlock(&lock);
		return false;
	}
	pthread_detach(thread);
	return true;
}
