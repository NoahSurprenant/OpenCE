/*
STREAM_OUTPUT.H

The headless stream mode's picture and sound, raw to pipes for ffmpeg
(stream_output.c; the Linux desktop build only: elsewhere it is never on).
*/

#ifndef STREAM_OUTPUT_H
#define STREAM_OUTPUT_H

/* whether this run streams (stream.enabled) */
int stream_output_enabled(void);
/* seconds since the process started */
double stream_output_seconds(void);
/* the picture's size (stream.width, stream.height) */
void stream_output_size(int *width, int *height);
/* (d3d8_gl.c, each frame before it is shown) a frame drawn: packed RGB,
width x height, its rows bottom first when bottom_up (as OpenGL reads them) */
void stream_output_frame(const unsigned char *pixels, int width, int height, int bottom_up);
/* whether the sound is streamed (stream.audio set) */
int stream_output_audio_enabled(void);
/* (dsound_sdl.c, on a thread of its own: never returns) the game's sound made
in real time by mix (float stereo frames at 48 kHz) and written to the pipe */
void stream_output_audio_run(void (*mix)(float *buffer, unsigned long frames));

#endif
