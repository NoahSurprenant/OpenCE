/* skate_sound.h: the skater's sounds (skate_sound.c; port/skate/README.md,
"Sound"), for skate.c. Without HALO_SKATE they do nothing. */

#ifndef HALO_SKATE_SOUND_MODE_H
#define HALO_SKATE_SOUND_MODE_H

/* skate_initialize: loads the sound set for the converted data in assets */
void skate_sound_initialize(char const *assets);
/* each tick (skate_update_after_objects): where every skater's sounds are
heard from (the camera) and the volume; while skating, the surface under the
board at position (world units); off the board (skating FALSE), this
machine's skater's sounds fade out (other machines' skaters' go on) */
void skate_sound_update(boolean skating, long unit_index, float const *position);
/* skate_console_setting: skate_volume and skate_sound_log, with what
skate_console_setting parsed (a number in value when valid, and whether
anything was given): -1 for another word, else whether it was taken */
int skate_sound_console_setting(char const *word, real value, boolean valid, boolean given);

/* ---------- every skater's sounds in a network game (skate_network.c) */

/* the loops' state: wheels on the ground (0 to 4), the grind (0 none, 1 on
the trucks, 2 on the deck), a powerslide, the surface under the board
(HALO_SKATE_SURFACE_...) and the board's speed (m/s) */
struct skate_sound_loops
{
	short wheels;
	short grind;
	boolean powerslide;
	short surface;
	real speed;
};
/* a one-shot: which (HALO_SKATE_SOUND_...), how hard (0 to 1) and the speed
it was measured from (m/s) */
struct skate_sound_event
{
	short sound;
	real strength;
	real speed;
};
/* skate_network_note_local, each tick: this machine's skater's one-shots
since the last tick (at most capacity, the newest), oldest first, into
events, and its loops' state into loops: how many. Events NULL takes them to
drop them (not skating). Without the mode, none */
short skate_sound_take_local(struct skate_sound_event *events, short capacity, struct skate_sound_loops *loops);
/* skate_network_update_after_objects: another machine's skater's loops, each
tick, at position (their board here, world units); a one-shot of theirs, once
(TRUE: it had a sample); and their sounds fading out. None of it on a
dedicated server, nor without the mode */
void skate_sound_remote_loops(short player_index, struct skate_sound_loops const *loops, real const *position);
boolean skate_sound_remote_event(short player_index, struct skate_sound_event const *event, short surface,
	real const *position);
void skate_sound_remote_stop(short player_index);

#endif
