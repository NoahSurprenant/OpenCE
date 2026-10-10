/* halo_skate_sound.h: the skater's sounds (port/skate/halo-skate: sound.rs,
sound_set.rs, mixer.rs; port/skate/README.md, "Sound"). The engine's ticks
make the events and loops themselves; the game says where they are heard
from, and the audio thread mixes them in. */

#ifndef HALO_SKATE_SOUND_H
#define HALO_SKATE_SOUND_H

/* the surface under the board, from the level's material (skate_sound.c) */
enum
{
	HALO_SKATE_SURFACE_CONCRETE,
	HALO_SKATE_SURFACE_METAL,
	HALO_SKATE_SURFACE_WOOD,
	HALO_SKATE_SURFACE_ROUGH
};

/* loads the sound set for the converted data in assets (skate-data/assets;
NULL for none): HALO_SKATE_SOUNDS, skate-data/sounds, skate-data/assets/sounds,
then the set built in. Returns how many of the sounds have samples (the log
names each one without) */
int halo_skate_sound_load(const char *assets);
/* the skate sounds' volume, 0 to 4: audio.effects_volume times skate_volume */
void halo_skate_sound_set_volume(float volume);
/* whether each one-shot (pop, land, ...) is told of in the log; 1 at first */
void halo_skate_sound_set_log(int enabled);
/* each tick while skating: the camera (world units) and the way it looks,
and the surface under the board (HALO_SKATE_SURFACE_...) */
void halo_skate_sound_listener(const float *position, const float *forward, const float *up, int surface);
/* off the board: everything fades out */
void halo_skate_sound_stop(void);
/* (the audio thread, dsound_sdl.c) adds the skater's sounds to output,
frames of interleaved stereo float at rate Hz, times gain */
void halo_skate_sound_mix(float *output, unsigned int frames, int rate, float gain);

#endif
