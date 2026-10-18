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
/* each tick: the camera (world units) and the way it looks, which every
skater's sounds are heard from, and the surface under this machine's
skater's board (HALO_SKATE_SURFACE_...) */
void halo_skate_sound_listener(const float *position, const float *forward, const float *up, int surface);
/* off the board: this machine's skater's sounds fade out (other machines'
skaters' go on) */
void halo_skate_sound_stop(void);
/* (the audio thread, dsound_sdl.c) adds the skater's sounds to output,
frames of interleaved stereo float at rate Hz, times gain */
void halo_skate_sound_mix(float *output, unsigned int frames, int rate, float gain);

/* ---------- every skater's sounds in a network game (sound_remote.rs;
skate_network.c sends this machine's skater's, and plays the others') */

/* each sound's number on the wire (its place in sound_set.rs's Sound::ALL):
the loops, then the one-shots. Never renumbered: another build reads them */
enum
{
	HALO_SKATE_SOUND_ROLL = 0,
	HALO_SKATE_SOUND_GRIND = 1,
	HALO_SKATE_SOUND_SLIDE = 2,
	HALO_SKATE_SOUND_POWERSLIDE = 3,
	HALO_SKATE_SOUND_POP = 4,
	HALO_SKATE_SOUND_LAND = 5,
	HALO_SKATE_SOUND_GRIND_START = 6,
	HALO_SKATE_SOUND_GRIND_END = 7,
	HALO_SKATE_SOUND_SLIDE_START = 8,
	HALO_SKATE_SOUND_SLIDE_END = 9,
	HALO_SKATE_SOUND_BOARD_IMPACT = 10,
	HALO_SKATE_SOUND_BAIL = 11,
	HALO_SKATE_SOUND_STEP_OFF = 12,
	HALO_SKATE_SOUND_STEP_ON = 13,
	HALO_SKATE_SOUND_COUNT = 14
};

/* a one-shot: which (HALO_SKATE_SOUND_...), how hard (0 to 1), and the speed
it was measured from (m/s) */
struct halo_skate_sound_event
{
	int sound;
	float strength;
	float speed;
};

/* the loops' state: wheels on the ground (0 to 4), the board's speed (m/s),
the grind (0 none, 1 on the trucks, 2 on the deck), a powerslide (0 or 1) */
struct halo_skate_sound_loops
{
	int wheels;
	float speed;
	int grind;
	int powerslide;
};

/* each tick this machine's skater skates: its one-shots since the last take
(at most capacity, the newest; none older than half a second), oldest first,
into events, and its loops' state into loops: how many */
int halo_skate_sound_take(struct halo_skate_sound_event *events, int capacity, struct halo_skate_sound_loops *loops);
/* ... of another skate session than 0 (a host's bot's skater: halo_skate.h's _in calls),
whose sounds are only kept, for the game to send and play as that player's */
int halo_skate_sound_take_in(int session, struct halo_skate_sound_event *events, int capacity,
	struct halo_skate_sound_loops *loops);
/* each tick, another machine's skater's loops as their latest word says, at
position (their board here, world units), on surface; player is their network
player's index (the mixer's source of their own) */
void halo_skate_sound_remote_loops(int player, const struct halo_skate_sound_loops *loops, const float *position,
	int surface);
/* another machine's skater's one-shot, once, at position, on surface; told of
in the log ("halo-skate: sound (remote player N): ..."). 1 when it had a
sample, else 0 */
int halo_skate_sound_remote_event(int player, const struct halo_skate_sound_event *event, const float *position,
	int surface);
/* another machine's skater's sounds fade out (not skating, or no word) */
void halo_skate_sound_remote_stop(int player);

#endif
