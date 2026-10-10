/* skate.h: Skate 3 mode (skate.c, port/skate/README.md). The local player
gets on a board with J, or with both sticks clicked in, and Skate 3's own
physics drive their biped until they get off. Without HALO_SKATE (a build
without --skate) nobody ever skates. */

#ifndef HALO_SKATE_MODE_H
#define HALO_SKATE_MODE_H

/* main_loop: at startup, once the console is up: the skate session (the
skater, its animations and physics, the slow part) is made in the background,
before any game */
void skate_initialize(void);
/* scenario_switch_structure_bsp: a structure bsp was loaded (a new map's
first, or a switch): its collision is built for skating in the background */
void skate_structure_bsp_changed(void);

/* game_tick: after the players' actions reach their units, before the
objects update */
void skate_update_before_objects(void);
/* game_tick: after the objects update, before the tick's pose is kept for
drawing */
void skate_update_after_objects(void);

/* render_window: the board under the skater, drawn with the objects */
void skate_render_board(void);

/* whether the unit is on a board: its biped's own movement stops, and its
camera follows it */
boolean skate_unit_is_skating(long unit_index);
/* whether the local player is on a board; if so, the heading their camera
keeps (radians) */
boolean skate_local_player_skating(short local_player_index, real *yaw);

/* hs_compile_and_evaluate: a console command tuning Skate 3 mode, one of
SKATE_CONSOLE_SETTINGS, with what follows its word: sets the value for the
session when given a number, and prints it either way. FALSE on a bad
number (or in a build without the mode) */
#define SKATE_CONSOLE_SETTINGS { "skate_board_scale", "skate_feet_offset", "skate_camera_speed" }
boolean skate_console_setting(char const *word, char const *arguments);

/* the camera's pitch behind a skater, radians */
#define SKATE_CAMERA_PITCH (-0.2f)
/* the seat state a skater reports to the director (director.c): one no seat
uses (they are 1 to 3), so that getting on and off the board changes it and
the director switches between the first-person and following cameras */
#define SKATE_DIRECTOR_SEAT_STATE 4
/* the nearest the following camera comes behind a skater, world units */
#define SKATE_CAMERA_MINIMUM_DISTANCE 0.5f

#endif
