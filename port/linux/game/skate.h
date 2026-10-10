/* skate.h: Skate 3 mode (skate.c, port/skate/README.md). The local player
gets on a board with J, or with both sticks clicked in, and Skate 3's own
physics drive their biped until they get off. Without HALO_SKATE (a build
without --skate) nobody ever skates. */

#ifndef HALO_SKATE_MODE_H
#define HALO_SKATE_MODE_H

/* game_tick: after the players' actions reach their units, before the
objects update */
void skate_update_before_objects(void);
/* game_tick: after the objects update, before the tick's pose is kept for
drawing */
void skate_update_after_objects(void);

/* whether the unit is on a board: its biped's own movement stops, and its
camera follows it */
boolean skate_unit_is_skating(long unit_index);
/* whether the local player is on a board; if so, the heading their camera
keeps (radians) */
boolean skate_local_player_skating(short local_player_index, real *yaw);

/* the camera's pitch behind a skater, radians */
#define SKATE_CAMERA_PITCH (-0.2f)
/* the seat state a skater reports to the director (director.c): one no seat
uses (they are 1 to 3), so that getting on and off the board changes it and
the director switches between the first-person and following cameras */
#define SKATE_DIRECTOR_SEAT_STATE 4
/* the nearest the following camera comes behind a skater, world units */
#define SKATE_CAMERA_MINIMUM_DISTANCE 0.5f

#endif
