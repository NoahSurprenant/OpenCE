/*
STREAM_OUTPUT.C

The headless stream mode's picture and sound (stream.enabled; port_config.c
sets up the rest of the run: a spectator of the game it joins, its camera
the director, no window). Each is raw, for ffmpeg to read from a pipe (a
FIFO the caller makes), at a steady rate of its own clock:

- the picture (stream.video): each frame drawn (d3d8_gl.c, before it is
  shown) is read back, and a writer thread sends the latest, stream.fps
  times a second, as packed RGB (stream.width x stream.height, top row
  first): a frame drawn late repeats the last, one drawn early is skipped,
  and the game never waits for the reader;
- the sound (stream.audio): the game's mix (dsound_sdl.c), 48 kHz stereo
  signed 16-bit little-endian, made in real time by a thread of its own,
  as it is when there is no sound device.

Until a reader opens its pipe, each is thrown away (the game goes on), and
a reader that goes away is waited for again. The first frame a reader
takes writes stream.ready_file and logs "stream: streaming", which is what
a probe waits for; the file goes at exit.
*/

#include "platform.h"
#include "port_config.h"
#include "stream_output.h"

/* (the Linux desktop build only: no pipes to write elsewhere) */
#if defined(__linux__) && !defined(HALO_ANDROID) && !defined(HALO_WEB) && !defined(HALO_WINDOWS)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------- constants */

/* the sound's rate and channels (dsound_sdl.c's output) */
#define STREAM_AUDIO_RATE 48000
#define STREAM_AUDIO_CHANNELS 2
/* the sound made at a time: 10 ms */
#define STREAM_AUDIO_FRAMES 480
/* behind the clock by more than this (a reader that stalled), the sound
starts its clock again rather than catching up (nanoseconds) */
#define STREAM_AUDIO_BEHIND 250000000LL

/* ---------- globals */

static struct
{
	int checked;
	int enabled;
	int width;
	int height;
	int fps;
	char video_path[512];
	char audio_path[512];
	char ready_path[512];
	/* when the process began (CLOCK_MONOTONIC, ns: stream_process_start) */
	long long started;
	/* the latest frame drawn (top row first), its number, and the lock */
	pthread_mutex_t lock;
	unsigned char *latest;
	unsigned long latest_number;
	int writer_started;
	int ready;
} stream = { 0, 0, 0, 0, 0, "", "", "", 0, PTHREAD_MUTEX_INITIALIZER };

/* ---------- private code */

static long long stream_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static void stream_sleep_until(long long deadline)
{
	struct timespec until;

	until.tv_sec = (time_t)(deadline / 1000000000LL);
	until.tv_nsec = (long)(deadline % 1000000000LL);
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR)
		;
}

/* when this process began, on CLOCK_MONOTONIC: its start in clock ticks
since the boot (/proc/self/stat's 22nd field) against the time since it
(now if that cannot be read) */
static long long stream_process_start(void)
{
	long long now = stream_now();
	unsigned long long start_ticks = 0;
	struct timespec boot;
	char line[1024];
	FILE *file = fopen("/proc/self/stat", "r");
	char *field;
	int index;

	if (!file)
		return now;
	field = fgets(line, sizeof(line), file) ? strrchr(line, ')') : NULL;
	fclose(file);
	/* (after the name: the 3rd field on, the 22nd is the 20th of them) */
	for (index = 0; field && index < 20; index++)
		field = strchr(field + 1, ' ');
	if (!field || sscanf(field, " %llu", &start_ticks) != 1 || clock_gettime(CLOCK_BOOTTIME, &boot) != 0)
		return now;
	return now - ((long long)boot.tv_sec * 1000000000LL + boot.tv_nsec -
		(long long)(start_ticks * 1000000000ULL / (unsigned long long)sysconf(_SC_CLK_TCK)));
}

static void stream_check(void)
{
	if (stream.checked)
		return;
	stream.checked = 1;
	stream.started = stream_process_start();
	stream.enabled = config_boolean("stream.enabled");
	if (!stream.enabled)
		return;
	stream.width = (int)config_integer("stream.width");
	stream.height = (int)config_integer("stream.height");
	stream.fps = (int)config_integer("stream.fps");
	if (stream.width < 320 || stream.width > 3840)
		stream.width = 1280;
	if (stream.height < 240 || stream.height > 2160)
		stream.height = 720;
	if (stream.fps < 1 || stream.fps > 120)
		stream.fps = 30;
	snprintf(stream.video_path, sizeof(stream.video_path), "%s", config_string("stream.video"));
	snprintf(stream.audio_path, sizeof(stream.audio_path), "%s", config_string("stream.audio"));
	snprintf(stream.ready_path, sizeof(stream.ready_path), "%s", config_string("stream.ready_file"));
}

/* a pipe's writing end once a reader has it open (-1: none yet): tried
without waiting, so the caller goes on (and throws away) meanwhile */
static int stream_open_pipe(const char *path)
{
	int descriptor = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	int flags;

	if (descriptor < 0)
		return -1;
	/* (then written whole, waiting for the reader) */
	flags = fcntl(descriptor, F_GETFL);
	if (flags >= 0)
		fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK);
	return descriptor;
}

/* all of it, or 0 (the reader went away) */
static int stream_write_all(int descriptor, const unsigned char *bytes, size_t size)
{
	while (size)
	{
		ssize_t written = write(descriptor, bytes, size);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return 0;
		bytes += written;
		size -= (size_t)written;
	}
	return 1;
}

static void stream_remove_ready_file(void)
{
	if (stream.ready && stream.ready_path[0])
		unlink(stream.ready_path);
}

/* the first frame a reader took: the stream is live */
static void stream_now_ready(void)
{
	if (stream.ready)
		return;
	stream.ready = 1;
	platform_log("stream: streaming (the first frame %.1f s after the start)",
		(double)(stream_now() - stream.started) / 1e9);
	if (stream.ready_path[0])
	{
		FILE *file = fopen(stream.ready_path, "w");

		if (file)
		{
			fprintf(file, "streaming\n");
			fclose(file);
		}
		else
		{
			platform_log("stream: cannot write %s", stream.ready_path);
		}
		atexit(stream_remove_ready_file);
	}
}

/* the picture's writer: the latest frame, stream.fps times a second */
static void *stream_video_thread(void *unused)
{
	size_t size = (size_t)stream.width * (size_t)stream.height * 3;
	unsigned char *frame = calloc(1, size);
	long long period = 1000000000LL / stream.fps;
	long long next = stream_now();
	long long counted = next;
	unsigned long drawn = 0, sent = 0;
	int descriptor = -1;

	(void)unused;
	if (!frame)
		return NULL;
	for (;;)
	{
		next += period;
		stream_sleep_until(next);
		if (descriptor < 0)
		{
			descriptor = stream_open_pipe(stream.video_path);
			if (descriptor < 0)
				continue;
			platform_log("stream: the picture's reader is there");
		}
		pthread_mutex_lock(&stream.lock);
		if (stream.latest)
			memcpy(frame, stream.latest, size);
		drawn = stream.latest_number;
		pthread_mutex_unlock(&stream.lock);
		if (!stream_write_all(descriptor, frame, size))
		{
			platform_log("stream: the picture's reader went away; waiting for another");
			close(descriptor);
			descriptor = -1;
			continue;
		}
		stream_now_ready();
		sent++;
		/* (how it goes, every half minute: the frames the game drew, which a
		slow renderer draws fewer of than are sent) */
		if (stream_now() - counted >= 30000000000LL)
		{
			static unsigned long drawn_before;

			platform_log("stream: %.1f frames a second drawn, %.1f sent", (double)(drawn - drawn_before) / 30.0,
				(double)sent / 30.0);
			drawn_before = drawn;
			sent = 0;
			counted = stream_now();
		}
		/* (a reader that stalled: on from now, not a burst to catch up) */
		if (stream_now() - next > period * 4)
			next = stream_now();
	}
	return NULL;
}

/* ---------- public code */

int stream_output_enabled(void)
{
	stream_check();
	return stream.enabled;
}

double stream_output_seconds(void)
{
	stream_check();
	return (double)(stream_now() - stream.started) / 1e9;
}

void stream_output_size(int *width, int *height)
{
	stream_check();
	*width = stream.width;
	*height = stream.height;
}

void stream_output_frame(const unsigned char *pixels, int width, int height, int bottom_up)
{
	size_t row = (size_t)stream.width * 3;
	int y;

	stream_check();
	if (!stream.enabled || !stream.video_path[0] || !pixels)
		return;
	pthread_mutex_lock(&stream.lock);
	if (!stream.latest)
		stream.latest = calloc(1, row * (size_t)stream.height);
	if (stream.latest)
	{
		/* (another size than the stream's, which the window should not be:
		its top left, the rest black) */
		int rows = height < stream.height ? height : stream.height;
		size_t bytes = (size_t)(width < stream.width ? width : stream.width) * 3;

		for (y = 0; y < rows; y++)
		{
			const unsigned char *source = pixels + (size_t)(bottom_up ? height - 1 - y : y) * (size_t)width * 3;

			memcpy(stream.latest + (size_t)y * row, source, bytes);
		}
		stream.latest_number++;
	}
	pthread_mutex_unlock(&stream.lock);
	if (!stream.writer_started)
	{
		pthread_t thread;

		stream.writer_started = 1;
		if (pthread_create(&thread, NULL, stream_video_thread, NULL) == 0)
			pthread_detach(thread);
	}
}

int stream_output_audio_enabled(void)
{
	stream_check();
	return stream.enabled && stream.audio_path[0];
}

void stream_output_audio_run(void (*mix)(float *buffer, unsigned long frames))
{
	float buffer[STREAM_AUDIO_FRAMES * STREAM_AUDIO_CHANNELS];
	short samples[STREAM_AUDIO_FRAMES * STREAM_AUDIO_CHANNELS];
	long long period = (long long)STREAM_AUDIO_FRAMES * 1000000000LL / STREAM_AUDIO_RATE;
	long long next = stream_now();
	int descriptor = -1;
	int index;

	platform_log("stream: sound to %s (s16le, %d Hz, %d channels)", stream.audio_path, STREAM_AUDIO_RATE,
		STREAM_AUDIO_CHANNELS);
	for (;;)
	{
		mix(buffer, STREAM_AUDIO_FRAMES);
		if (descriptor < 0)
		{
			descriptor = stream_open_pipe(stream.audio_path);
			if (descriptor >= 0)
				platform_log("stream: the sound's reader is there");
		}
		if (descriptor >= 0)
		{
			for (index = 0; index < STREAM_AUDIO_FRAMES * STREAM_AUDIO_CHANNELS; index++)
			{
				float value = buffer[index] * 32767.0f;

				samples[index] = (short)(value > 32767.0f ? 32767 : value < -32768.0f ? -32768 : value);
			}
			if (!stream_write_all(descriptor, (const unsigned char *)samples, sizeof(samples)))
			{
				platform_log("stream: the sound's reader went away; waiting for another");
				close(descriptor);
				descriptor = -1;
			}
		}
		next += period;
		if (stream_now() - next > STREAM_AUDIO_BEHIND)
			next = stream_now();
		stream_sleep_until(next);
	}
}

#else

int stream_output_enabled(void)
{
	return 0;
}

double stream_output_seconds(void)
{
	return 0.0;
}

void stream_output_size(int *width, int *height)
{
	*width = 0;
	*height = 0;
}

void stream_output_frame(const unsigned char *pixels, int width, int height, int bottom_up)
{
	(void)pixels;
	(void)width;
	(void)height;
	(void)bottom_up;
}

int stream_output_audio_enabled(void)
{
	return 0;
}

void stream_output_audio_run(void (*mix)(float *buffer, unsigned long frames))
{
	(void)mix;
}

#endif
