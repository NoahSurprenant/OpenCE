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
#define SKATE_CONSOLE_SETTINGS { "skate_board_scale", "skate_feet_offset", "skate_camera_speed", \
	"skate_obstacle_bail_speed", \
	"skate_volume", "skate_sound_log" }
boolean skate_console_setting(char const *word, char const *arguments);

/* ---------- multiplayer (skate_network.c)
Each machine runs the engine for its own skater; the others see them from a
state sent every tick (port/skate/README.md, "Multiplayer"). */

/* a player's state on the wire (skate_state_write): its player's absolute
index, the flags, the node count and the format, the root node's and the
board's offsets from the unit's origin (16 bits a part, 1/1024 world units),
the board's turn, then each node's turn from its bind pose (a quaternion in
32 bits: its smallest three parts, 10 bits each) */
#define SKATE_STATE_FORMAT 1
#define SKATE_STATE_HEADER_SIZE 20
#define SKATE_STATE_MAXIMUM_NODES 64
#define SKATE_STATE_MAXIMUM_SIZE (SKATE_STATE_HEADER_SIZE + 4 * SKATE_STATE_MAXIMUM_NODES)

/* network_distributed.c: a new game, nothing known of anyone's skating */
void skate_network_new_game(void);
/* the player's latest state, if it is to be sent (fresh: this machine's
skater's of this tick, or another machine's of the last second and a half),
into entry (SKATE_STATE_MAXIMUM_SIZE bytes): its size, 0 for none */
short skate_network_entry(short player_index, byte *entry, boolean *skating);
/* the states of a message (machine_index the sender's on the host, NONE on a
client); each checked, and on the host taken only for that machine's players */
void skate_network_handle_entries(long machine_index, byte const *data, byte const *end, short count);
/* whether another machine's player skates, as their latest state says */
boolean skate_network_player_skating(short player_index);
/* ... whether the unit is another machine's skater's (skate_unit_is_skating) */
boolean skate_network_unit_skating(long unit_index);

/* skate.c, each tick: this machine's skater (unit NONE: none, or not
skating), its pose already in the unit's nodes, and where the engine has the
board (NULL: nowhere) */
void skate_network_note_local(long player_index, long unit_index, boolean off_board, real const *board_placement);
/* before the objects update: another machine's skater carried on at its
velocity (its biped's own movement is off); after it: posed */
void skate_network_update_before_objects(void);
void skate_network_update_after_objects(void);
/* where another machine's skater's board is drawn, between its last two
ticks: its unit, and its placement (halo_skate_board_vertices_at); FALSE for
none */
boolean skate_network_board(short player_index, real fraction, long *unit_index, real *placement);
/* the most players there are (skate_network_board) */
#define SKATE_NETWORK_PLAYERS HALO_PORT_MAXIMUM_NETWORK_PLAYERS
/* player_control.c: debug.network_test_skate_watch turns the local player
to another machine's skater, to see them (the network test) */
boolean skate_local_player_watching(short local_player_index, real *yaw, real *pitch);
/* debug.network_test_skate_block (a host's player only): every five seconds,
where the unit steps into the way of another machine's nearest skater, ahead of
them the way they go, and the yaw (if asked) that faces them; FALSE when it is
not time, or no skater goes fast enough. on_board: the unit skates (skate.c
puts it on the board there) */
boolean skate_network_block_spot(long unit_index, boolean on_board, union real_point3d *spot, real *yaw);
/* skate.c: a unit's weapon holstered on its posed nodes */
struct real_matrix4x3;
void skate_holster_weapon(long unit_index, struct real_matrix4x3 const *node_matrices, int node_count);

/* the camera's pitch behind a skater, radians */
#define SKATE_CAMERA_PITCH (-0.2f)
/* the seat state a skater reports to the director (director.c): one no seat
uses (they are 1 to 3), so that getting on and off the board changes it and
the director switches between the first-person and following cameras */
#define SKATE_DIRECTOR_SEAT_STATE 4
/* the nearest the following camera comes behind a skater, world units */
#define SKATE_CAMERA_MINIMUM_DISTANCE 0.5f

#endif
