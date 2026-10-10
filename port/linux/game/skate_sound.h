/* skate_sound.h: the skater's sounds (skate_sound.c; port/skate/README.md,
"Sound"), for skate.c. Without HALO_SKATE they do nothing. */

#ifndef HALO_SKATE_SOUND_MODE_H
#define HALO_SKATE_SOUND_MODE_H

/* skate_initialize: loads the sound set for the converted data in assets */
void skate_sound_initialize(char const *assets);
/* each tick (skate_update_after_objects): while skating, where the sounds
are heard from (the camera), the surface under the board at position (world
units) and the volume; off the board (skating FALSE), everything fades out */
void skate_sound_update(boolean skating, long unit_index, float const *position);
/* skate_console_setting: skate_volume and skate_sound_log, with what
skate_console_setting parsed (a number in value when valid, and whether
anything was given): -1 for another word, else whether it was taken */
int skate_sound_console_setting(char const *word, real value, boolean valid, boolean given);

#endif
