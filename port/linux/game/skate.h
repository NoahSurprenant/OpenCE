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

/* ---------- the host's bots' skaters (skate.c; bots_skate.c drives them)
A host with the skate engine and Skate 3 data runs a skate session for each
bot on a board (as many as server.bot_skaters allows), beside its own
player's: the bots' skaters step, collide, are posed, holstered, drawn,
heard and sent to the other machines as the host's own skater is. Without
the mode, or the data, no bot skates. */

/* what a bot's skater is doing (skate_bot_frame) */
struct skate_bot_view
{
	/* its session has the level: it gets on as soon as it asks */
	boolean ready;
	boolean skating;
	boolean off_board;
	boolean bailing;
	boolean grinding;
	boolean airborne;
	/* the biped's origin, its velocity (world units a second: the board's,
	or off the board the skater's own) and the board's forward */
	real position[3];
	real velocity[3];
	real forward[3];
	/* the engine's state (e.g. "Wipeout...", "Grind...") */
	char state[32];
};

struct halo_skate_pad;
/* whether bots may skate here at all: the skate engine has its Skate 3 data
and server.bot_skaters allows one */
boolean skate_bots_available(void);
/* the bots' skaters step slower than the game (the machine has too few
processors for so many): no more bots get on boards meanwhile */
boolean skate_bots_busy(void);
/* (bots_skate.c, before each tick, for every bot that rides or wants to)
the bot of player_index (a datum) wants to be on a board (on_board; FALSE: off
it) with its unit, holding pad (NULL: nothing) this tick; run_over: people on
foot it rides into fast are run over (a peaceful bot's are not). FALSE when it
has no skater, and none is to be had (all in use, or none allowed); a bot
that stops asking for a second is given up */
boolean skate_bot_drive(long player_index, long unit_index, boolean on_board, struct halo_skate_pad const *pad,
	boolean run_over);
/* the bot's skater as its last step left it, into view; FALSE when it has
none */
boolean skate_bot_frame(long player_index, struct skate_bot_view *view);
/* the grind rails the skate engine found in the level (a bot's session that
has it): each rail's points (3 floats each, world units) into points (room
for point_capacity), the count of points before each rail's end into ends
(room for end_capacity); how many rails, 0 for none (yet) */
int skate_bot_rails(float *points, int point_capacity, int *ends, int end_capacity);
/* skate_authority.c: whether the unit is a skater the host's engine runs
(its own player's or a bot's) on the board and not bailing; and whether it is
such a skater at all (alive, on foot or riding) */
boolean skate_host_skater_riding(long unit_index);
boolean skate_host_skater(long unit_index);
/* ... and whether it runs nobody over (a peaceful bot's: skate_bot_drive's
run_over off), whose hits the host takes as bumps */
boolean skate_host_skater_harmless(long unit_index);

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
board or bails, and where the engine has the board (NULL: nowhere); session:
its skate session (0 this machine's player's; another, a host's bot's, whose
sounds are played here too, as another machine's skater's are) */
void skate_network_note_local(long player_index, long unit_index, boolean off_board, boolean bailing,
	real const *board_placement, short session);
/* before the objects update: another machine's skater carried on at its
velocity (its biped's own movement is off); after it: posed */
void skate_network_update_before_objects(void);
void skate_network_update_after_objects(void);
/* where another machine's skater's board is drawn, between its last two
ticks: its unit, and its placement (halo_skate_board_vertices_at); FALSE for
none */
boolean skate_network_board(short player_index, real fraction, long *unit_index, real *placement);
/* a player's sounds on the wire (skate_sound_write; port/skate/README.md,
"Multiplayer"): its player's absolute index, the format, the loops' state
(wheels down, the grind, a powerslide, the surface; the speed in quarters of
a m/s), the newest one-shot's sequence number and how many follow, then the
one-shots of the last SKATE_SOUND_EVENT_TICKS, oldest first, each its sound
(halo_skate_sound.h's HALO_SKATE_SOUND_...), how hard (in 255ths), its speed
(quarters of a m/s) and how many ticks before the entry was written it came.
Sent every tick, so each one-shot goes several times and a lost datagram
loses none; the sequence numbers play each once */
#define SKATE_SOUND_FORMAT 1
#define SKATE_SOUND_SIZE 24
#define SKATE_SOUND_EVENTS 4
#define SKATE_SOUND_EVENT_TICKS 15
/* the player's latest sounds, if they are to be sent (fresh: this machine's
skater's of this tick, or another machine's of the last few, which the host
passes on), into entry (SKATE_SOUND_SIZE bytes): its size, 0 for none */
short skate_network_sound_entry(short player_index, byte *entry);
/* the sounds of a message (machine_index the sender's on the host, NONE on
a client), taken as skate_network_handle_entries takes states */
void skate_network_handle_sounds(long machine_index, byte const *data, byte const *end, short count);

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
/* a skater riding into a person on foot runs them over, as a vehicle does in
Halo (physics.c's biped and vehicle collision), at a closing speed this fast
or more (world units a second: Halo's own least, 1, 3 m/s, the speed apart
within which a vehicle pushes a person aside unhurt): the skater goes on
through them, keeping this share of their speed (skate.c, the engine), and the
host deals them Halo's vehicle collision damage by the speed
(skate_authority.c's skate_run_over_share). Slower, a bump, as before */
#define SKATE_RUN_OVER_SPEED 1.f
#define SKATE_RUN_OVER_KEEP 0.7f

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
from it toward the skater), whether it held a bail, and whether the skater ran a
person on foot over (went through them: SKATE_RUN_OVER_SPEED) */
void skate_authority_engine_hit(long skater_index, long target_index, real closing, union real_vector3d const *normal,
	boolean touched, boolean bail_held, boolean run_over);
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

/* ---------- a skater's hit shapes (skate_hitbox.c)
A skater is hit where their body is drawn: their biped's nodes are the
skate pose whenever Halo places them, not only once the objects updated. */

/* skate.c: this machine's skater's pose as the engine has it now, into
nodes (count of them), and the engine's state's name (state, if not NULL);
FALSE when the unit is not this machine's skater or the pose is not had */
boolean skate_local_pose(long unit_index, struct real_matrix4x3 *nodes, int count, char const **state);
/* skate_network.c: another machine's skater's pose from their latest
state, where their unit is now; FALSE when the unit is no such skater */
boolean skate_network_pose(long unit_index, struct real_matrix4x3 *nodes, short count);
/* object_compute_node_matrices: a skater's nodes made the skate pose, before
their bounding sphere is placed from them; TRUE if they were. Their sphere
is then grown SKATE_BOUNDING_MARGIN (world units): a pose reaches further from
its root than Halo's animations, a hand or a foot past the model's radius by
as much as 0.054 seen */
boolean skate_pose_object_nodes(long object_index);
#define SKATE_BOUNDING_MARGIN 0.1f
/* object_postprocess_node_matrices: whether the object is a skater, whose
nodes Halo's aim and IK leave as posed */
boolean skate_object_posed(long object_index);
/* game_tick, before and after objects_update: debug.skate_hitbox_test's
check of each skater's hit shapes against their drawn body; and once they
are posed for drawing, their nodes kept, for the next tick's check */
enum
{
	_skate_hitbox_before_objects,
	_skate_hitbox_after_objects,
	_skate_hitbox_drawn
};
void skate_hitbox_test(short when);

/* the camera's pitch behind a skater, radians */
#define SKATE_CAMERA_PITCH (-0.2f)
/* the seat state a skater reports to the director (director.c): one no seat
uses (they are 1 to 3), so that getting on and off the board changes it and
the director switches between the first-person and following cameras */
#define SKATE_DIRECTOR_SEAT_STATE 4
/* the nearest the following camera comes behind a skater, world units */
#define SKATE_CAMERA_MINIMUM_DISTANCE 0.5f

#endif
