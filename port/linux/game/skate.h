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
/* object_damage_aftermath: a blast's push (Halo's acceleration, world units a
tick) on this machine's living skater, given to the skate engine instead of
the biped (TRUE: taken, biped_accelerate not to be called) */
boolean skate_unit_pushed(long unit_index, real_vector3d const *acceleration);
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
skating), its pose already in the unit's nodes, whether it walks off the
board or bails, and where the engine has the board (NULL: nowhere) */
void skate_network_note_local(long player_index, long unit_index, boolean off_board, boolean bailing,
	real const *board_placement);
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

/* ---------- the host's verdicts on skaters' hits (skate_authority.c)
In a network game whether a skater's hit on another player, their vehicle
or another skater knocks the skater over, and the push it gives a player on
foot, are decided once by the host and applied alike on every machine
(port/skate/README.md, "Multiplayer"). */

/* a client's report of its skater's hit, and the host's verdict, on the
wire (skate_report_write, skate_verdict_write); an entry of another format
is skipped */
#define SKATE_REPORT_FORMAT 1
#define SKATE_REPORT_SIZE 20
#define SKATE_VERDICT_FORMAT 1
#define SKATE_VERDICT_SIZE 24

/* skate_network_new_game: nothing known of any hit */
void skate_authority_new_game(void);
/* whether this machine's skater's hits on other players, their vehicles and
other skaters are the host's to decide: on the host in a network game, and
on a client whose host said lately that it decides (an older host does not:
its clients' skaters decide their own hits) */
boolean skate_authority_holds(void);
/* skate.c, after a step: what the skate engine told of its skater's hits on
held obstacles (halo_skate_take_held_hit): the fastest contact this tick
(touched; target_index the object hit, closing world units a second, normal
from it toward the skater), and whether it held a bail */
void skate_authority_engine_hit(long skater_index, long target_index, real closing, union real_vector3d const *normal,
	boolean touched, boolean bail_held);
/* network_distributed.c: (a client) its reports this tick, (the host) its
verdicts this tick, the same to every client, into out (capacity entries
of their size): how many; and the entries of each, as they come */
short skate_authority_reports(byte *out, short capacity);
short skate_authority_verdicts(byte *out, short capacity);
void skate_authority_handle_reports(long machine_index, byte const *data, byte const *end, short count);
void skate_authority_handle_verdicts(byte const *data, byte const *end, short count);
/* skate_network_update_after_objects, each tick: the host's velocities,
bails held too long, pushes measured */
void skate_authority_update(void);
/* skate_network.c: whether another machine's skater rides (on the board,
not bailing), as their latest state says */
boolean skate_network_player_riding(short player_index);
/* skate.c: this machine's skater's unit (NONE: none skates), whether they
ride (on the board, not bailing); a bail the host decided for them, through
the engine (FALSE: they do not skate); and the obstacle bail speed (world
units a second) */
long skate_local_skater_unit(void);
boolean skate_local_skater_riding(void);
boolean skate_force_bail(long unit_index, real closing);
real skate_obstacle_bail_speed(void);

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
