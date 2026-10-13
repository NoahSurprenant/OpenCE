/* skate_network.c: Skate 3 mode in a network game (skate.h,
port/skate/README.md "Multiplayer", port/linux/NETCODE.md)

Each machine runs the skate engine for its own skater alone (skate.c). After
each tick it notes its skater's state: whether they skate, where the board
is, and the biped's pose, as each node's turn from its bind pose and the
root node's place. The skate rig keeps every node's bone lengths (rig.rs: a
node is its parent's place plus its parent's turn of the bind pose's offset
between them), so those turns are the whole pose, and any machine rebuilds
it from the biped's own model. A client sends its own players' states to the
host every tick (_distributed_message_skate_up); the host sends each client
the states of the players on other machines, its own too
(_distributed_message_skate_down), as often as it sends their units.

Another machine's skater is posed from the latest state there is, around
where this machine has their unit, their weapon holstered, and their board
drawn there (skate.c, with the skate engine's board). Halo's own movement and
animation of their biped stop (bipeds.c), as for one's own skater; the unit
glides to the host's word as before, carried on at its velocity in between.
The host lets a skater's own client place them as fast as a board goes
(network_distributed.c), and takes no hits from a skater, who has no weapon
in hand.

A build without the mode still sends nothing but poses others (it draws no
board); a build older than this drops both messages as kinds it does not
know, and its players see Halo's own biped.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "objects/damage.h"
#include "game/game_globals.h"
#include "items/weapon_definitions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "units/units.h"
#include "network_distributed.h"

#include "skate.h"

#include <math.h>
#include <string.h>

/* the platform layer's (port_config.c, the port's log) */
const char *config_string(const char *name);
int config_boolean(const char *name);
double config_real(const char *name);
void platform_log(const char *format, ...);
/* network_distributed.c's: the host's takes of skaters' positions */
void network_distributed_skate_predictions(long *taken, long *refused, long *refused_on_foot);

/* the flags of a state (skate_state_write) */
enum
{
	/* on the board, or off it walking in the skate engine */
	_skate_state_skating_bit,
	/* the board's place is given */
	_skate_state_board_bit,
	/* the skater walks, off the board */
	_skate_state_off_board_bit,
	/* the skater bails (the engine's wipeout): their machine's word, for the
	host's verdicts (skate_authority.c) and the network test's log */
	_skate_state_bailing_bit,
};

/* the offsets' scale: 1/1024 world units, to 32 world units either way */
#define SKATE_STATE_OFFSET_SCALE 1024.0f
#define SKATE_STATE_MAXIMUM_OFFSET 31.9f
/* a quaternion's smallest three parts, each within 1/sqrt(2) of 0, in 10
bits */
#define SKATE_QUATERNION_BITS 10
#define SKATE_QUATERNION_STEPS 1023.0f
#define SKATE_QUATERNION_RANGE 0.70710678f
/* ticks another machine's state counts for after it came: past the sixth
tick a far player is sent at, through a few datagrams lost */
#define SKATE_NETWORK_STALE_TICKS 45
/* world units a tick: past any skater's speed (90 m/s), what carries on */
#define SKATE_NETWORK_MAXIMUM_CARRY 1.0f
/* world units the board may move in a tick before it is drawn there at once */
#define SKATE_NETWORK_BOARD_SNAP 1.0f
/* the network test's watcher stands this near another's skater when they
are further than SKATE_NETWORK_WATCH_DISTANCE (world units; the host's own
player only, which is the host's to place) */
#define SKATE_NETWORK_WATCH_DISTANCE 8.0f
#define SKATE_NETWORK_WATCH_NEAR 3.0f
/* debug.network_test_skate_block: every this many ticks (5 seconds) the
watcher steps into the way of a skater going faster than
SKATE_NETWORK_BLOCK_SPEED (world units a second, 1.5 m/s), this far ahead of
where the host has them (world units, 4.6 m: well under a second at any
pushed speed, so the carve does not take them round it) */
#define SKATE_NETWORK_BLOCK_TICKS (5 * TICKS_PER_SECOND)
#define SKATE_NETWORK_BLOCK_SPEED 0.5f
#define SKATE_NETWORK_BLOCK_AHEAD 1.5f
/* ... but not this many ticks either side of the host's own skater's
teleport (debug.network_test_skate_teleport: a second) */
#define SKATE_NETWORK_BLOCK_CLEAR TICKS_PER_SECOND
/* a spot the watcher is put at has ground this near under it (world units)
and nothing of the level's between it and the skater */
#define SKATE_NETWORK_SPOT_GROUND 1.0f
/* the skeletons rebuilt from their model, by definition */
#define SKATE_NETWORK_SKELETONS 4

/* a state, read (skate_state_read) */
struct skate_state
{
	byte flags;
	byte node_count;
	real_vector3d root_offset;
	real_vector3d board_offset;
	real board_rotation[4];
	real turns[SKATE_STATE_MAXIMUM_NODES][4];
};

/* ---------- the state on the wire
(tools/harness/tests/skate_state.c takes these as they are) */

static long skate_round(real value)
{
	return (long)(value < 0.f ? value - 0.5f : value + 0.5f);
}

/* a unit quaternion (x, y, z, w) in 32 bits: which part is largest (made
positive: q and -q are one turn), then the other three, each in 10 bits */
static unsigned long skate_quaternion_pack(real const *quaternion)
{
	real q[4];
	real length;
	unsigned long packed;
	short largest = 0;
	short index;
	short shift = 2 * SKATE_QUATERNION_BITS;

	length = (real)sqrt(quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
		quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3]);
	/* (none, or not a number: no turn) */
	if (!(length > 0.0001f && length < 10.f))
		return 3UL << 30 | 511UL << 20 | 511UL << 10 | 511UL;
	for (index = 0; index < 4; index++)
	{
		q[index] = quaternion[index] / length;
		if (fabs(q[index]) > fabs(q[largest]))
			largest = index;
	}
	if (q[largest] < 0.f)
	{
		for (index = 0; index < 4; index++)
			q[index] = -q[index];
	}
	packed = (unsigned long)largest << 30;
	for (index = 0; index < 4; index++)
	{
		long step;

		if (index == largest)
			continue;
		step = skate_round((q[index] / SKATE_QUATERNION_RANGE + 1.f) * 0.5f * SKATE_QUATERNION_STEPS);
		if (step < 0)
			step = 0;
		if (step > (long)SKATE_QUATERNION_STEPS)
			step = (long)SKATE_QUATERNION_STEPS;
		packed |= (unsigned long)step << shift;
		shift -= SKATE_QUATERNION_BITS;
	}
	return packed;
}

/* ... and back, made one long; FALSE for parts that are no unit quaternion's */
static boolean skate_quaternion_unpack(unsigned long packed, real *quaternion)
{
	short largest = (short)(packed >> 30 & 3);
	short index;
	short shift = 2 * SKATE_QUATERNION_BITS;
	real sum = 0.f;
	real length;

	for (index = 0; index < 4; index++)
	{
		real part;

		if (index == largest)
			continue;
		part = ((real)(packed >> shift & 1023UL) / SKATE_QUATERNION_STEPS * 2.f - 1.f) * SKATE_QUATERNION_RANGE;
		quaternion[index] = part;
		sum += part * part;
		shift -= SKATE_QUATERNION_BITS;
	}
	/* (the largest part is at least each other's: the three no more than
	three quarters, with some rounding) */
	if (sum > 0.76f)
		return FALSE;
	quaternion[largest] = (real)sqrt(1.f - sum);
	length = (real)sqrt(sum + quaternion[largest] * quaternion[largest]);
	for (index = 0; index < 4; index++)
		quaternion[index] /= length;
	return TRUE;
}

static void skate_put_short(byte *out, real value)
{
	long step = skate_round(value * SKATE_STATE_OFFSET_SCALE);
	short part = (short)(step < -32767 ? -32767 : step > 32767 ? 32767 : step);

	memcpy(out, &part, sizeof(part));
}

static real skate_get_short(byte const *data)
{
	short part;

	memcpy(&part, data, sizeof(part));
	return (real)part / SKATE_STATE_OFFSET_SCALE;
}

static void skate_put_long(byte *out, unsigned long value)
{
	unsigned int part = (unsigned int)value;

	memcpy(out, &part, sizeof(part));
}

static unsigned long skate_get_long(byte const *data)
{
	unsigned int part;

	memcpy(&part, data, sizeof(part));
	return part;
}

/* whether an offset can be sent: a number, within SKATE_STATE_MAXIMUM_OFFSET */
static boolean skate_offset_valid(real_vector3d const *offset)
{
	return fabs(offset->i) <= SKATE_STATE_MAXIMUM_OFFSET && fabs(offset->j) <= SKATE_STATE_MAXIMUM_OFFSET &&
		fabs(offset->k) <= SKATE_STATE_MAXIMUM_OFFSET;
}

/* a player's state into out (SKATE_STATE_MAXIMUM_SIZE bytes): its size. An
offset that cannot be sent leaves out the pose, or the board, it places */
static short skate_state_write(byte player_index, struct skate_state const *state, byte *out)
{
	byte flags = state->flags;
	byte node_count = state->node_count <= SKATE_STATE_MAXIMUM_NODES ? state->node_count : 0;
	short index;

	if (!skate_offset_valid(&state->root_offset))
		node_count = 0;
	if (!skate_offset_valid(&state->board_offset))
		flags &= (byte)~FLAG(_skate_state_board_bit);
	memset(out, 0, SKATE_STATE_HEADER_SIZE);
	out[0] = player_index;
	out[1] = flags;
	out[2] = node_count;
	out[3] = SKATE_STATE_FORMAT;
	if (node_count)
	{
		skate_put_short(out + 4, state->root_offset.i);
		skate_put_short(out + 6, state->root_offset.j);
		skate_put_short(out + 8, state->root_offset.k);
	}
	if (TEST_FLAG(flags, _skate_state_board_bit))
	{
		skate_put_short(out + 10, state->board_offset.i);
		skate_put_short(out + 12, state->board_offset.j);
		skate_put_short(out + 14, state->board_offset.k);
		skate_put_long(out + 16, skate_quaternion_pack(state->board_rotation));
	}
	for (index = 0; index < node_count; index++)
		skate_put_long(out + SKATE_STATE_HEADER_SIZE + 4 * index, skate_quaternion_pack(state->turns[index]));
	return (short)(SKATE_STATE_HEADER_SIZE + 4 * node_count);
}

/* a state from data, read no further than end: its size, 0 for one that is
not (cut short, of another format, too many nodes, a turn no turn). Flags
this build does not know are left be */
static short skate_state_read(byte const *data, byte const *end, byte *player_index, struct skate_state *state)
{
	short size;
	short index;

	if (end - data < SKATE_STATE_HEADER_SIZE || data[3] != SKATE_STATE_FORMAT || data[2] > SKATE_STATE_MAXIMUM_NODES)
		return 0;
	size = (short)(SKATE_STATE_HEADER_SIZE + 4 * data[2]);
	if (end - data < size)
		return 0;
	memset(state, 0, sizeof(*state));
	*player_index = data[0];
	state->flags = data[1];
	state->node_count = data[2];
	state->root_offset.i = skate_get_short(data + 4);
	state->root_offset.j = skate_get_short(data + 6);
	state->root_offset.k = skate_get_short(data + 8);
	state->board_offset.i = skate_get_short(data + 10);
	state->board_offset.j = skate_get_short(data + 12);
	state->board_offset.k = skate_get_short(data + 14);
	state->board_rotation[3] = 1.f;
	if (TEST_FLAG(state->flags, _skate_state_board_bit) &&
		!skate_quaternion_unpack(skate_get_long(data + 16), state->board_rotation))
	{
		return 0;
	}
	for (index = 0; index < state->node_count; index++)
	{
		if (!skate_quaternion_unpack(skate_get_long(data + SKATE_STATE_HEADER_SIZE + 4 * index), state->turns[index]))
			return 0;
	}
	return size;
}

/* ---------- the pose as turns
A matrix's turn is of its forward, left and up (the columns that the x, y
and z axes go to). */

static void skate_quaternion_from_matrix(real_matrix4x3 const *matrix, real *q)
{
	real m00 = matrix->forward.i, m10 = matrix->forward.j, m20 = matrix->forward.k;
	real m01 = matrix->left.i, m11 = matrix->left.j, m21 = matrix->left.k;
	real m02 = matrix->up.i, m12 = matrix->up.j, m22 = matrix->up.k;
	real trace = m00 + m11 + m22;
	real s;

	if (trace > 0.f)
	{
		s = (real)sqrt(trace + 1.f) * 2.f;
		q[3] = 0.25f * s;
		q[0] = (m21 - m12) / s;
		q[1] = (m02 - m20) / s;
		q[2] = (m10 - m01) / s;
	}
	else if (m00 > m11 && m00 > m22)
	{
		s = (real)sqrt(1.f + m00 - m11 - m22) * 2.f;
		q[3] = (m21 - m12) / s;
		q[0] = 0.25f * s;
		q[1] = (m01 + m10) / s;
		q[2] = (m02 + m20) / s;
	}
	else if (m11 > m22)
	{
		s = (real)sqrt(1.f + m11 - m00 - m22) * 2.f;
		q[3] = (m02 - m20) / s;
		q[0] = (m01 + m10) / s;
		q[1] = 0.25f * s;
		q[2] = (m12 + m21) / s;
	}
	else
	{
		s = (real)sqrt(1.f + m22 - m00 - m11) * 2.f;
		q[3] = (m10 - m01) / s;
		q[0] = (m02 + m20) / s;
		q[1] = (m12 + m21) / s;
		q[2] = 0.25f * s;
	}
}

/* (a times b: b's turn, then a's) */
static void skate_quaternion_multiply(real const *a, real const *b, real *out)
{
	real q[4];

	q[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	q[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	q[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	q[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	memcpy(out, q, sizeof(q));
}

static void skate_quaternion_rotate(real const *q, real_vector3d const *vector, real_vector3d *out)
{
	/* v + 2w(u x v) + 2u x (u x v), u the quaternion's vector part */
	real ux = q[0], uy = q[1], uz = q[2], w = q[3];
	real cx = uy * vector->k - uz * vector->j;
	real cy = uz * vector->i - ux * vector->k;
	real cz = ux * vector->j - uy * vector->i;
	real_vector3d result;

	result.i = vector->i + 2.f * (w * cx + uy * cz - uz * cy);
	result.j = vector->j + 2.f * (w * cy + uz * cx - ux * cz);
	result.k = vector->k + 2.f * (w * cz + ux * cy - uy * cx);
	*out = result;
}

/* whether a skeleton can be rebuilt from turns: the first node its root, and
every other node's parent before it */
static boolean skate_skeleton_valid(short const *parents, short count)
{
	short index;

	if (count <= 0 || count > SKATE_STATE_MAXIMUM_NODES || parents[0] != NONE)
		return FALSE;
	for (index = 1; index < count; index++)
	{
		if (parents[index] < 0 || parents[index] >= index)
			return FALSE;
	}
	return TRUE;
}

/* the posed nodes as turns from the bind pose (defaults: each node's matrix
in it), and the root's place from origin (the unit's) */
static void skate_pose_turns(real_matrix4x3 const *nodes, real_matrix4x3 const *defaults, short count,
	real_point3d const *origin, struct skate_state *state)
{
	short index;

	state->node_count = (byte)count;
	state->root_offset.i = nodes[0].position.x - origin->x;
	state->root_offset.j = nodes[0].position.y - origin->y;
	state->root_offset.k = nodes[0].position.z - origin->z;
	for (index = 0; index < count; index++)
	{
		real posed[4], bind[4];

		skate_quaternion_from_matrix(&nodes[index], posed);
		skate_quaternion_from_matrix(&defaults[index], bind);
		bind[0] = -bind[0]; bind[1] = -bind[1]; bind[2] = -bind[2];
		skate_quaternion_multiply(posed, bind, state->turns[index]);
	}
}

/* ... and back, as rig.rs poses them: each node's turn times its bind pose's,
at its parent's place plus its parent's turn of their offset in the bind
pose; the root at origin plus its offset. FALSE when the state has not this
skeleton's nodes */
static boolean skate_pose_rebuild(struct skate_state const *state, real_matrix4x3 const *defaults,
	short const *parents, short count, real_point3d const *origin, real_matrix4x3 *nodes)
{
	short index;

	if (state->node_count != count || !skate_skeleton_valid(parents, count))
		return FALSE;
	for (index = 0; index < count; index++)
	{
		real const *turn = state->turns[index];
		real_vector3d axis;

		nodes[index].scale = defaults[index].scale;
		/* (the turn of each of the bind pose's axes) */
		skate_quaternion_rotate(turn, &defaults[index].forward, &axis);
		nodes[index].forward = axis;
		skate_quaternion_rotate(turn, &defaults[index].left, &axis);
		nodes[index].left = axis;
		skate_quaternion_rotate(turn, &defaults[index].up, &axis);
		nodes[index].up = axis;
		if (index == 0)
		{
			nodes[0].position.x = origin->x + state->root_offset.i;
			nodes[0].position.y = origin->y + state->root_offset.j;
			nodes[0].position.z = origin->z + state->root_offset.k;
		}
		else
		{
			short parent = parents[index];
			real_vector3d offset, turned;

			offset.i = defaults[index].position.x - defaults[parent].position.x;
			offset.j = defaults[index].position.y - defaults[parent].position.y;
			offset.k = defaults[index].position.z - defaults[parent].position.z;
			skate_quaternion_rotate(state->turns[parent], &offset, &turned);
			nodes[index].position.x = nodes[parent].position.x + turned.i;
			nodes[index].position.y = nodes[parent].position.y + turned.j;
			nodes[index].position.z = nodes[parent].position.z + turned.k;
		}
	}
	return TRUE;
}

/* ---------- the players' states */

static struct skate_network_player
{
	/* this machine's game time it was noted, NONE for never */
	long time;
	/* this machine's own skater's */
	boolean local;
	short size;
	byte entry[SKATE_STATE_MAXIMUM_SIZE];
	/* the board where it was drawn the last two ticks: the unit, and each
	placement (a quaternion, then a position) */
	long board_unit_index;
	real boards[2][7];
	short latest;
	boolean has_latest;
	boolean has_previous;
	/* (another machine's) its state said it bails, last tick */
	boolean bailing;
} skate_network_players[SKATE_NETWORK_PLAYERS];

/* each model's bind pose, for posing others' skaters */
static struct skate_network_skeleton
{
	long definition_index;
	short count;
	short parents[SKATE_STATE_MAXIMUM_NODES];
	real_matrix4x3 defaults[SKATE_STATE_MAXIMUM_NODES];
} skate_network_skeletons[SKATE_NETWORK_SKELETONS];

static struct
{
	short next_skeleton;
	/* a network test logs what arrives once a second (debug.network_test) */
	boolean log;
	boolean watch;
	/* debug.network_test_skate_block, and when the watcher last stepped in */
	boolean block;
	long blocked_time;
	long logged_time;
} skate_network_globals;

void skate_network_new_game(void)
{
	short index;

	for (index = 0; index < SKATE_NETWORK_PLAYERS; index++)
	{
		skate_network_players[index].time = NONE;
		skate_network_players[index].has_latest = FALSE;
		skate_network_players[index].board_unit_index = NONE;
		skate_network_players[index].bailing = FALSE;
	}
	skate_authority_new_game();
	skate_network_globals.log = config_string("debug.network_test") && *config_string("debug.network_test");
	skate_network_globals.watch = config_boolean("debug.network_test_skate_watch");
	skate_network_globals.block = config_boolean("debug.network_test_skate_block");
	skate_network_globals.blocked_time = NONE;
	skate_network_globals.logged_time = NONE;
}

/* the unit's model's bind pose, NULL with none to pose by */
static struct skate_network_skeleton *skate_network_skeleton(long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct object_definition *definition = object_definition_get(unit->definition_index);
	struct skate_network_skeleton *skeleton;
	struct model *model;
	short index;

	for (index = 0; index < SKATE_NETWORK_SKELETONS; index++)
	{
		if (skate_network_skeletons[index].definition_index == unit->definition_index &&
			skate_network_skeletons[index].count)
		{
			return skate_network_skeletons[index].count > 0 ? &skate_network_skeletons[index] : NULL;
		}
	}
	if (definition->object.model.index == NONE)
		return NULL;
	model = model_definition_get(definition->object.model.index);
	skeleton = &skate_network_skeletons[skate_network_globals.next_skeleton];
	skate_network_globals.next_skeleton = (short)((skate_network_globals.next_skeleton + 1) % SKATE_NETWORK_SKELETONS);
	skeleton->definition_index = unit->definition_index;
	skeleton->count = (short)model->nodes.count;
	if (model->nodes.count <= 0 || model->nodes.count > SKATE_STATE_MAXIMUM_NODES)
		skeleton->count = -1;
	for (index = 0; index < skeleton->count; index++)
	{
		struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);

		skeleton->parents[index] = node->parent_node_index;
		matrix4x3_inverse(&node->runtime_default_inverse_matrix, &skeleton->defaults[index]);
	}
	if (skeleton->count > 0 && !skate_skeleton_valid(skeleton->parents, skeleton->count))
	{
		platform_log("skate network: a biped's nodes are not parents first; its skating pose is not sent");
		skeleton->count = -1;
	}
	return skeleton->count > 0 ? skeleton : NULL;
}

/* the biped a player's state poses: their living unit on foot */
static long skate_network_player_unit(short player_index)
{
	struct player_datum *player = distributed_player(player_index);
	long unit_index = distributed_living_unit(player);
	struct object_datum *object = unit_index != NONE ? object_try_and_get(unit_index) : NULL;

	if (!object || object->object.type != _object_type_biped || object->object.parent_object_index != NONE ||
		(object->object.damage_flags & FLAG(_object_dead_bit)))
	{
		return NONE;
	}
	return unit_index;
}

static boolean skate_network_fresh(struct skate_network_player const *state)
{
	long age = game_time_get() - state->time;

	return state->time != NONE && age >= 0 && age <= (state->local ? 1 : SKATE_NETWORK_STALE_TICKS);
}

short skate_network_entry(short player_index, byte *entry, boolean *skating)
{
	struct skate_network_player *state;

	if (player_index < 0 || player_index >= SKATE_NETWORK_PLAYERS)
		return 0;
	state = &skate_network_players[player_index];
	/* (this machine's off goes as long as another's counts, for a lost one) */
	if (!state->size || !(skate_network_fresh(state) ||
		(state->local && game_time_get() - state->time <= SKATE_NETWORK_STALE_TICKS &&
		!TEST_FLAG(state->entry[1], _skate_state_skating_bit))))
	{
		return 0;
	}
	memcpy(entry, state->entry, state->size);
	if (skating)
		*skating = TEST_FLAG(state->entry[1], _skate_state_skating_bit);
	return state->size;
}

void skate_network_handle_entries(long machine_index, byte const *data, byte const *end, short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		struct skate_state state;
		byte player_index;
		short size = skate_state_read(data, end, &player_index, &state);
		struct skate_network_player *player;

		if (!size)
			break;
		/* a machine speaks for its own players, and hears of the others' */
		if (player_index < SKATE_NETWORK_PLAYERS && (machine_index == NONE ?
			!(distributed_player(player_index) && distributed_player(player_index)->local_player_index != NONE) :
			distributed_machine_has_player(machine_index, player_index)))
		{
			player = &skate_network_players[player_index];
			player->time = game_time_get();
			player->local = FALSE;
			player->size = size;
			memcpy(player->entry, data, size);
		}
		data += size;
	}
}

boolean skate_network_player_skating(short player_index)
{
	struct skate_network_player const *state;

	if (player_index < 0 || player_index >= SKATE_NETWORK_PLAYERS)
		return FALSE;
	state = &skate_network_players[player_index];
	return !state->local && skate_network_fresh(state) && TEST_FLAG(state->entry[1], _skate_state_skating_bit);
}

boolean skate_network_player_riding(short player_index)
{
	struct skate_network_player const *state;

	if (!skate_network_player_skating(player_index))
		return FALSE;
	state = &skate_network_players[player_index];
	return !TEST_FLAG(state->entry[1], _skate_state_off_board_bit) && !TEST_FLAG(state->entry[1], _skate_state_bailing_bit);
}

boolean skate_network_unit_skating(long unit_index)
{
	struct object_datum *object = unit_index != NONE ? object_try_and_get(unit_index) : NULL;
	long player_index;
	short absolute_index;

	if (!object || object->object.type != _object_type_biped)
		return FALSE;
	player_index = ((struct unit_datum *)object)->unit.player_index;
	if (player_index == NONE)
		return FALSE;
	absolute_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	return skate_network_player_skating(absolute_index) && skate_network_player_unit(absolute_index) == unit_index;
}

/* (the sender) its own skater's state, rebuilt as another machine will, and
how far that is from the pose: the most a node is off, world units and
degrees (logged in a network test) */
static void skate_network_pose_error(long unit_index, real_matrix4x3 const *nodes, short count,
	byte const *entry, short size, real *distance, real *degrees)
{
	struct skate_network_skeleton *skeleton = skate_network_skeleton(unit_index);
	struct skate_state state;
	real_matrix4x3 rebuilt[SKATE_STATE_MAXIMUM_NODES];
	byte player_index;
	short index;

	*distance = -1.f;
	*degrees = -1.f;
	if (!skeleton || !skate_state_read(entry, entry + size, &player_index, &state) ||
		!skate_pose_rebuild(&state, skeleton->defaults, skeleton->parents, skeleton->count,
		&object_get(unit_index)->object.position, rebuilt))
	{
		return;
	}
	*distance = 0.f;
	*degrees = 0.f;
	for (index = 0; index < count; index++)
	{
		real dx = rebuilt[index].position.x - nodes[index].position.x;
		real dy = rebuilt[index].position.y - nodes[index].position.y;
		real dz = rebuilt[index].position.z - nodes[index].position.z;
		real off = (real)sqrt(dx * dx + dy * dy + dz * dz);
		real cosine = (rebuilt[index].forward.i * nodes[index].forward.i + rebuilt[index].forward.j *
			nodes[index].forward.j + rebuilt[index].forward.k * nodes[index].forward.k +
			rebuilt[index].up.i * nodes[index].up.i + rebuilt[index].up.j * nodes[index].up.j +
			rebuilt[index].up.k * nodes[index].up.k) * 0.5f;
		real angle = (real)acos(cosine > 1.f ? 1.f : cosine < -1.f ? -1.f : cosine) * 180.f / _pi;

		if (off > *distance)
			*distance = off;
		if (angle > *degrees)
			*degrees = angle;
	}
}

void skate_network_note_local(long player_index, long unit_index, boolean off_board, boolean bailing,
	real const *board_placement)
{
	struct skate_network_player *player;
	struct skate_state state;
	short absolute_index;
	struct object_datum *object;

	if (player_index == NONE)
		return;
	absolute_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	if (absolute_index >= SKATE_NETWORK_PLAYERS)
		return;
	player = &skate_network_players[absolute_index];
	memset(&state, 0, sizeof(state));
	state.board_rotation[3] = 1.f;
	if (unit_index == NONE)
	{
		/* off the board: said once, then sent for as long as a state counts */
		if (player->local && player->size && TEST_FLAG(player->entry[1], _skate_state_skating_bit))
		{
			player->size = skate_state_write((byte)absolute_index, &state, player->entry);
			player->time = game_time_get();
		}
		return;
	}
	object = object_get(unit_index);
	state.flags = FLAG(_skate_state_skating_bit) | (off_board ? FLAG(_skate_state_off_board_bit) : 0) |
		(bailing ? FLAG(_skate_state_bailing_bit) : 0);
	{
		struct skate_network_skeleton *skeleton = skate_network_skeleton(unit_index);
		short count = (short)(object->object.node_matrices.size / (int)sizeof(real_matrix4x3));

		if (skeleton && count == skeleton->count)
		{
			skate_pose_turns((real_matrix4x3 const *)object_header_block_get(unit_index, &object->object.node_matrices),
				skeleton->defaults, count, &object->object.position, &state);
		}
	}
	if (board_placement)
	{
		state.flags |= FLAG(_skate_state_board_bit);
		memcpy(state.board_rotation, board_placement, 4 * sizeof(real));
		state.board_offset.i = board_placement[4] - object->object.position.x;
		state.board_offset.j = board_placement[5] - object->object.position.y;
		state.board_offset.k = board_placement[6] - object->object.position.z;
	}
	player->size = skate_state_write((byte)absolute_index, &state, player->entry);
	player->time = game_time_get();
	player->local = TRUE;
	if (skate_network_globals.log && game_time_get() % TICKS_PER_SECOND == 0)
	{
		real distance, degrees;
		real_vector3d const *velocity = &object->object.translational_velocity;

		skate_network_pose_error(unit_index, (real_matrix4x3 const *)object_header_block_get(unit_index,
			&object->object.node_matrices), state.node_count, player->entry, player->size, &distance, &degrees);
		platform_log("skate network: player %d skates here at (%.2f %.2f %.2f) %.1f m/s%s, its state %d bytes "
			"(%d nodes%s, %.2f %.2f %.2f from it), rebuilt within %.4f world units and %.2f degrees", absolute_index,
			object->object.position.x, object->object.position.y, object->object.position.z,
			sqrt(velocity->i * velocity->i + velocity->j * velocity->j + velocity->k * velocity->k) *
			TICKS_PER_SECOND * 3.048f, off_board ? " off the board" : "", player->size, player->entry[2],
			TEST_FLAG(player->entry[1], _skate_state_board_bit) ? ", the board" : "", state.board_offset.i,
			state.board_offset.j, state.board_offset.k, distance, degrees);
	}
}

void skate_network_update_before_objects(void)
{
	short index;

	for (index = 0; index < SKATE_NETWORK_PLAYERS; index++)
	{
		long unit_index;
		struct object_datum *object;
		real_vector3d const *velocity;
		real_point3d position;

		if (!skate_network_player_skating(index) || (unit_index = skate_network_player_unit(index)) == NONE)
			continue;
		/* (its own movement off, bipeds.c: on at the speed it last had, as
		Halo's physics would carry it, until the next word on it) */
		object = object_get(unit_index);
		velocity = &object->object.translational_velocity;
		if (!(velocity->i * velocity->i + velocity->j * velocity->j + velocity->k * velocity->k <=
			SKATE_NETWORK_MAXIMUM_CARRY * SKATE_NETWORK_MAXIMUM_CARRY))
		{
			continue;
		}
		position.x = object->object.position.x + velocity->i;
		position.y = object->object.position.y + velocity->j;
		position.z = object->object.position.z + velocity->k;
		object_set_position(unit_index, &position, &object->object.forward, &object->object.up);
	}
}

/* the board drawn at a placement this tick, after the last (snapped there
when it moved further than a board goes, or is of another unit) */
static void skate_network_board_note(struct skate_network_player *player, long unit_index, real const *placement)
{
	short next = player->has_latest ? (short)(player->latest ^ 1) : player->latest;

	memcpy(player->boards[next], placement, 7 * sizeof(real));
	player->has_previous = player->has_latest && player->board_unit_index == unit_index;
	if (player->has_previous)
	{
		real const *previous = player->boards[next ^ 1];
		real dx = placement[4] - previous[4], dy = placement[5] - previous[5], dz = placement[6] - previous[6];

		if (!(dx * dx + dy * dy + dz * dz <= SKATE_NETWORK_BOARD_SNAP * SKATE_NETWORK_BOARD_SNAP))
			player->has_previous = FALSE;
	}
	player->has_latest = TRUE;
	player->latest = next;
	player->board_unit_index = unit_index;
}


/* ---------- debug.network_test_skate_shoot
A host's watcher shoots skaters with real projectiles, as Halo's own
weapons fire them, at the seconds skate_shoot_seconds gives (a grenade kills
a skater, and a watcher near, as it would anyone, so not often): a burst of
three rounds of its weapon from a little way off, then a grenade thrown at
their chest, each aimed at the skater's posed body. The skater is another
machine's (where the host has them, posed from their state) or, once the
host skates too (debug.network_test_skate_duel), the host's own at the
seconds given so. The log tells what each did: the burst's damage (shield
and body before and after), whether the grenade bounced off the skater (its
velocity turned back near them), and where the skater was when it went off
and a little later.

So that a shot tests the skater and not where they happen to be, every shot
is fired along a clear line (skate_shoot_clear): from the side of the
skater nothing stands on, the first thing a projectile there would meet being
the skater. In the duel the other skater, who just rode into this one, is
often right in front of them, and a burst from the front hit them instead. A
burst that did no harm all the same (the skater bailing low, under it) is
fired again, a fresh line each time, till one harms them
(SKATE_BURST_TRIES). A shot due while another's grenade is about waits for
it. */
/* when it shoots (seconds into the game, or as soon after as nothing shot
before is about), whether at its own skater, and whether a burst and a
grenade */
static struct
{
	short seconds;
	boolean own;
	boolean burst;
	boolean grenade;
} const skate_shoot_seconds[] = { { 20, FALSE, FALSE, TRUE }, { 38, FALSE, TRUE, TRUE }, { 64, TRUE, TRUE, FALSE },
	{ 96, FALSE, TRUE, TRUE } };
/* and a grenade alone at its own skater (any push on them till it goes off
is its bounce's), from that many seconds in to that many, on a second when
they skate, alive, alone, free (skate_shoot_free), nothing else shot is
about, the line to them is clear and there is room to push them
(skate_shoot_room),
till that many have gone off hurting them without killing them. Each is
thrown with them healed, a health pack's body and a full shield (a frag at
their feet then hurts them without killing them), and one that went off away
from them, or at once, is thrown again */
#define SKATE_OWN_GRENADES 1
#define SKATE_OWN_GRENADES_FROM 70
#define SKATE_OWN_GRENADES_UNTIL 125
/* world units from the skater's middle the round starts and the grenade is
thrown from */
#define SKATE_SHOOT_DISTANCE 1.5f
#define SKATE_THROW_DISTANCE 0.8f
/* bursts at a skater, at most, till one harms them */
#define SKATE_BURST_TRIES 8
/* and for that many seconds at most */
#define SKATE_BURST_SECONDS 15
/* the skater's course as a grenade goes off: how far they went over that
many ticks before it (where they really went, not their velocity, which a
skater pressed against something keeps while going nowhere) */
#define SKATE_COURSE_TICKS 6
/* world units open beyond the host's own skater, the way a grenade at them
is thrown (and the way its blast pushes them, back from where it lands): a
skater pushed into a wall goes nowhere, as anyone would */
#define SKATE_PUSH_ROOM 1.5f
/* and no other skater within that many world units of them */
#define SKATE_OWN_GRENADE_ALONE 3.f
/* and free (skate_shoot_free): their speed over that many ticks within so
many metres a second of their velocity */
#define SKATE_FREE_TICKS 10
#define SKATE_FREE_SPEED 0.4f

static struct
{
	long target_index;
	boolean local;
	long bullet_time;
	real shield;
	real body;
	long grenade_index;
	real_vector3d grenade_direction;
	boolean bounced;
	long detonated_time;
	real_point3d detonated_at;
	/* (grenades alone at its own skater that went off hurting them) */
	short own_grenades;
	/* (where the grenade was last, the skater's shield and body then, and their
	velocity and course, a tick's going, as it went off) */
	real_point3d grenade_at;
	real grenade_shield;
	real grenade_body;
	real_vector3d detonated_velocity;
	real_vector3d detonated_course;
	/* (when the grenade was thrown) */
	long grenade_time;
	/* (a burst still wanted, at this machine's skater or another's, and how
	many fired for it) */
	boolean burst_wanted;
	boolean burst_local;
	short bursts;
	/* (when it was first wanted) */
	long burst_time;
	/* (the next of skate_shoot_seconds) */
	short next;
	/* (where the skater was the last ticks a grenade was about, and when) */
	real_point3d course[SKATE_COURSE_TICKS];
	long course_time[SKATE_COURSE_TICKS];
	short course_count;
	/* (where the host's own skater was the last ticks they skated) */
	real_point3d own_places[SKATE_FREE_TICKS];
	short own_place_count;
} skate_shoot;

/* whether the host's own skater goes as fast as their velocity says, over
the last SKATE_FREE_TICKS ticks (still, or riding freely): not pinned, their
engine's board going on against something while they go nowhere, as a
skater put down by a teleporter or the duel can be. A pinned skater a blast
pushes goes nowhere either */
static boolean skate_shoot_free(struct object_datum const *watcher)
{
	real_point3d const *first = &skate_shoot.own_places[0];
	real went, speed;

	if (skate_shoot.own_place_count < SKATE_FREE_TICKS)
		return FALSE;
	went = (real)sqrt((watcher->object.position.x - first->x) * (watcher->object.position.x - first->x) +
		(watcher->object.position.y - first->y) * (watcher->object.position.y - first->y)) / (SKATE_FREE_TICKS - 1);
	speed = (real)sqrt(watcher->object.translational_velocity.i * watcher->object.translational_velocity.i +
		watcher->object.translational_velocity.j * watcher->object.translational_velocity.j);
	return fabs(went - speed) * TICKS_PER_SECOND * 3.048f < SKATE_FREE_SPEED;
}

static boolean skate_shoot_alive(long object_index)
{
	struct object_datum *object = object_index != NONE ? object_try_and_get(object_index) : NULL;

	return object && !TEST_FLAG(object->object.damage_flags, _object_dead_bit);
}

/* the skater to shoot: the host's own, alive and skating, or the nearest
living skater of another machine */
static long skate_shoot_target(long watcher_index, boolean local)
{
	long target_index = NONE;
	real nearest = 0.f;
	short index;

	if (local)
		return skate_unit_is_skating(watcher_index) && skate_shoot_alive(watcher_index) ? watcher_index : NONE;
	for (index = 0; index < SKATE_NETWORK_PLAYERS; index++)
	{
		long skater_index = skate_network_player_skating(index) ? skate_network_player_unit(index) : NONE;
		real distance;

		if (skater_index == NONE || skater_index == watcher_index || !skate_shoot_alive(skater_index))
			continue;
		distance = distance_squared3d(&object_get(skater_index)->object.position,
			&object_get(watcher_index)->object.position);
		if (target_index == NONE || distance < nearest)
		{
			target_index = skater_index;
			nearest = distance;
		}
	}
	return target_index;
}

/* where a shot at target_index aims: their middle, or (a skater lying in a
bail, as the host's verdicts often have them) their root node, low, rather
than over them */
static void skate_shoot_aim(long target_index, real_point3d *at)
{
	struct object_datum *target = object_get(target_index);

	*at = target->object.bounding_sphere_center;
	if (target->object.node_matrices.size >= (int)sizeof(real_matrix4x3))
	{
		real_matrix4x3 const *root = (real_matrix4x3 const *)object_header_block_get(target_index,
			&target->object.node_matrices);

		if (root->position.z < at->z - 0.2f)
			*at = root->position;
	}
}

/* the level's ground straight under point (within 2 world units, not water):
whether there is any, its height and how level it is */
static boolean skate_shoot_ground(long target_index, real_point3d const *point, real *height, real *level)
{
	struct collision_result collision;
	real_vector3d down;

	down.i = 0.f;
	down.j = 0.f;
	down.k = -2.f;
	if (!collision_test_vector(_collision_test_environment_flags, point, &down, target_index, &collision) ||
		collision.type == _collision_result_media)
	{
		return FALSE;
	}
	*height = collision.point.z;
	*level = collision.plane.n.k;
	return TRUE;
}

/* room for target_index to be pushed beyond world units along direction
from at (their middle): nothing in the way at their middle or just over their
feet, and level ground under them, half way and at the end, all at one height
(not a slope, a ledge, a creek's bank or its water). A skater pushed into a
wall, or sliding down a slope, does not go away from a blast as the test
measures it */
static boolean skate_shoot_room(long target_index, real_point3d const *at, real_vector3d const *direction,
	real beyond)
{
	struct collision_result collision;
	real_point3d feet = object_get(target_index)->object.position;
	real_vector3d past;
	real height0, height, level;
	short step;

	feet.z += 0.15f;
	past.i = direction->i * beyond;
	past.j = direction->j * beyond;
	past.k = 0.f;
	if (collision_test_vector(_collision_test_environment_flags, at, &past, target_index, &collision) ||
		collision_test_vector(_collision_test_environment_flags, &feet, &past, target_index, &collision) ||
		!skate_shoot_ground(target_index, at, &height0, &level) || level < 0.97f)
	{
		return FALSE;
	}
	for (step = 1; step <= 2; step++)
	{
		real_point3d point = *at;

		point.x += past.i * step / 2.f;
		point.y += past.j * step / 2.f;
		if (!skate_shoot_ground(target_index, &point, &height, &level) || level < 0.97f ||
			fabs(height - height0) > 0.25f)
		{
			return FALSE;
		}
	}
	return TRUE;
}

/* a clear line to target_index from distance world units off, level: the
way they face first, then turning round them 45 degrees at a time, either
side, to their back. Clear is nothing between their middle and where the
shot starts (not another skater, the watcher, a rock: a start inside a rock
does not hit it from within), and a projectile's test from there (Halo's
own, ignoring the shot's owner) meeting the skater first; with beyond, room
beyond them for that many world units too (skate_shoot_room). Its direction
is from the start to the skater */
static boolean skate_shoot_clear(long target_index, long owner_index, real distance, real beyond,
	real_vector3d *direction)
{
	struct object_datum *target = object_get(target_index);
	real_point3d at;
	real facing;
	short attempt;

	skate_shoot_aim(target_index, &at);
	facing = (real)atan2(-target->object.forward.j, -target->object.forward.i);
	for (attempt = 0; attempt < 8; attempt++)
	{
		real angle = facing + (real)((attempt + 1) / 2) * (attempt % 2 ? 1.f : -1.f) * _pi / 4.f;
		struct collision_result collision;
		real_point3d start;
		real_vector3d back, along;

		direction->i = (real)cos(angle);
		direction->j = (real)sin(angle);
		direction->k = 0.f;
		start.x = at.x - direction->i * distance;
		start.y = at.y - direction->j * distance;
		start.z = at.z;
		back.i = -direction->i * distance;
		back.j = -direction->j * distance;
		back.k = 0.f;
		along.i = direction->i * (distance + 0.5f);
		along.j = direction->j * (distance + 0.5f);
		along.k = 0.f;
		if (!collision_test_vector(_collision_test_for_projectiles_flags, &at, &back, target_index, &collision) &&
			collision_test_vector(_collision_test_for_projectiles_flags, &start, &along, owner_index, &collision) &&
			collision.type == _collision_result_object && collision.object_index == target_index &&
			(!(beyond > 0.f) || skate_shoot_room(target_index, &at, direction, beyond)))
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* a projectile of definition_index aimed at target_index (skate_shoot_aim,
raised by height world units) from distance world units off, along
direction (from it to the target) */
static long skate_shoot_projectile(long definition_index, long owner_index, long target_index,
	real_vector3d const *direction, real distance, real height)
{
	struct object_placement_data data;
	real_point3d at;

	skate_shoot_aim(target_index, &at);
	object_placement_data_new(&data, definition_index, owner_index);
	data.position.x = at.x - direction->i * distance;
	data.position.y = at.y - direction->j * distance;
	data.position.z = at.z - direction->k * distance + height;
	data.forward = *direction;
	return object_new(&data);
}

/* the first projectile of the watcher's weapon's first trigger, and Halo's
first grenade */
static long skate_shoot_bullet_definition(long watcher_index)
{
	struct unit_datum *unit = (struct unit_datum *)object_get(watcher_index);
	long weapon_index = unit->unit.current_weapon_index != NONE ?
		unit->unit.weapon_object_indices[unit->unit.current_weapon_index] : NONE;

	if (weapon_index != NONE)
	{
		struct weapon_definition *weapon = weapon_definition_get(object_get(weapon_index)->definition_index);

		if (weapon->weapon.triggers.count > 0)
			return TAG_BLOCK_GET_ELEMENT(&weapon->weapon.triggers, 0, struct weapon_trigger_definition)->projectile.index;
	}
	return NONE;
}

static long skate_shoot_grenade_definition(void)
{
	return global_game_globals && global_game_globals->grenades.count > 0 ?
		TAG_BLOCK_GET_ELEMENT(&global_game_globals->grenades, 0, struct game_globals_grenade)->projectile.index : NONE;
}

/* the wanted burst, if a skater of its kind is alive and a line to them
clear: whether it was fired */
static boolean skate_shoot_burst(long watcher_index, long now)
{
	long target_index = skate_shoot_target(watcher_index, skate_shoot.burst_local);
	long owner_index = skate_shoot.burst_local ? NONE : watcher_index;
	long definition_index = skate_shoot_bullet_definition(watcher_index);
	struct object_datum *target;
	real_vector3d direction;
	short shot;

	if (target_index == NONE || definition_index == NONE ||
		!skate_shoot_clear(target_index, owner_index, SKATE_SHOOT_DISTANCE, 0.f, &direction))
	{
		return FALSE;
	}
	target = object_get(target_index);
	skate_shoot.target_index = target_index;
	skate_shoot.local = skate_shoot.burst_local;
	skate_shoot.shield = target->object.shield_vitality;
	skate_shoot.body = target->object.body_vitality;
	skate_shoot.bursts++;
	/* (three: at the chest, and a little above and below) */
	for (shot = -1; shot <= 1; shot++)
	{
		if (skate_shoot_projectile(definition_index, owner_index, target_index, &direction, SKATE_SHOOT_DISTANCE,
			shot * 0.12f) != NONE)
		{
			skate_shoot.bullet_time = now;
		}
	}
	skate_shoot.burst_wanted = FALSE;
	return TRUE;
}

/* a grenade at target_index along direction */
static void skate_shoot_grenade(long watcher_index, long target_index, boolean local, real_vector3d const *direction,
	long now)
{
	long definition_index = skate_shoot_grenade_definition();

	skate_shoot.target_index = target_index;
	skate_shoot.local = local;
	skate_shoot.bounced = FALSE;
	skate_shoot.grenade_direction = *direction;
	skate_shoot.grenade_time = now;
	skate_shoot.course_count = 0;
	skate_shoot.grenade_index = definition_index != NONE ? skate_shoot_projectile(definition_index,
		local ? NONE : watcher_index, target_index, direction, SKATE_THROW_DISTANCE, 0.f) : NONE;
}

static void skate_network_test_shoot(void)
{
	static int enabled = -1;
	long watcher_index = player_control_get_unit_index(0);
	struct object_datum *watcher = watcher_index != NONE ? object_try_and_get(watcher_index) : NULL;
	long now = game_time_get();
	boolean due;

	if (enabled < 0)
	{
		enabled = config_boolean("debug.network_test_skate_shoot");
		memset(&skate_shoot, 0, sizeof(skate_shoot));
		skate_shoot.target_index = NONE;
		skate_shoot.bullet_time = NONE;
		skate_shoot.grenade_index = NONE;
		skate_shoot.detonated_time = NONE;
	}
	if (!enabled || !watcher || game_connection() == _game_connection_network_client || !game_in_progress())
		return;
	/* (the host's own skater's last places) */
	if (!skate_unit_is_skating(watcher_index))
		skate_shoot.own_place_count = 0;
	else
	{
		if (skate_shoot.own_place_count == SKATE_FREE_TICKS)
		{
			memmove(&skate_shoot.own_places[0], &skate_shoot.own_places[1],
				sizeof(skate_shoot.own_places[0]) * (SKATE_FREE_TICKS - 1));
			skate_shoot.own_place_count--;
		}
		skate_shoot.own_places[skate_shoot.own_place_count++] = watcher->object.position;
	}
	/* (what the last shots did) */
	if (skate_shoot.target_index != NONE && !object_try_and_get(skate_shoot.target_index))
	{
		/* (the skater shot at gone, a body taken away: what was about of it
		forgotten, a wanted burst fired at whoever is there next) */
		platform_log("skate test: the skater shot at (%s) is gone at tick %ld",
			skate_shoot.local ? "this machine's" : "another machine's", now);
		skate_shoot.target_index = NONE;
		skate_shoot.grenade_index = NONE;
		skate_shoot.detonated_time = NONE;
		if (skate_shoot.bullet_time != NONE && skate_shoot.bursts < SKATE_BURST_TRIES)
			skate_shoot.burst_wanted = TRUE;
		skate_shoot.bullet_time = NONE;
	}
	if (skate_shoot.target_index != NONE)
	{
		struct object_datum *target = object_get(skate_shoot.target_index);

		if (skate_shoot.bullet_time != NONE && now - skate_shoot.bullet_time == TICKS_PER_SECOND / 2)
		{
			boolean harmed = target->object.shield_vitality < skate_shoot.shield ||
				target->object.body_vitality < skate_shoot.body;

			platform_log("skate test: a burst at a skater (%s): shield %.2f to %.2f, body %.2f to %.2f (burst %d)",
				skate_shoot.local ? "this machine's" : "another machine's", skate_shoot.shield,
				target->object.shield_vitality, skate_shoot.body, target->object.body_vitality, skate_shoot.bursts);
			skate_shoot.bullet_time = NONE;
			/* (no harm done: again, while tries are left) */
			if (!harmed && skate_shoot.bursts < SKATE_BURST_TRIES)
				skate_shoot.burst_wanted = TRUE;
		}
		if (skate_shoot.grenade_index != NONE)
		{
			struct object_datum *grenade = object_try_and_get(skate_shoot.grenade_index);

			if (grenade && grenade->object.type == _object_type_projectile)
			{
				real_vector3d const *v = &grenade->object.translational_velocity;
				real near = (real)sqrt(distance_squared3d(&grenade->object.position,
					&target->object.bounding_sphere_center));
				short index;

				skate_shoot.grenade_at = grenade->object.position;
				skate_shoot.grenade_shield = target->object.shield_vitality;
				skate_shoot.grenade_body = target->object.body_vitality;
				/* (the skater's last places) */
				if (skate_shoot.course_count == SKATE_COURSE_TICKS)
				{
					for (index = 1; index < SKATE_COURSE_TICKS; index++)
					{
						skate_shoot.course[index - 1] = skate_shoot.course[index];
						skate_shoot.course_time[index - 1] = skate_shoot.course_time[index];
					}
					skate_shoot.course_count--;
				}
				skate_shoot.course[skate_shoot.course_count] = target->object.position;
				skate_shoot.course_time[skate_shoot.course_count] = now;
				skate_shoot.course_count++;

				/* (turned more than 60 degrees from its throw near them) */
				if (!skate_shoot.bounced && near < 1.f && v->i * skate_shoot.grenade_direction.i +
					v->j * skate_shoot.grenade_direction.j + v->k * skate_shoot.grenade_direction.k <
					0.5f * magnitude3d(v))
				{
					skate_shoot.bounced = TRUE;
					platform_log("skate test: the grenade bounced off the skater (%s), %.2f world units from them at tick %ld",
						skate_shoot.local ? "this machine's" : "another machine's", near, now);
				}
			}
			else
			{
				skate_shoot.grenade_index = NONE;
				skate_shoot.detonated_time = now;
				skate_shoot.detonated_at = target->object.position;
				skate_shoot.detonated_velocity = target->object.translational_velocity;
				skate_shoot.detonated_course.i = 0.f;
				skate_shoot.detonated_course.j = 0.f;
				skate_shoot.detonated_course.k = 0.f;
				if (skate_shoot.course_count > 0 && now > skate_shoot.course_time[0])
				{
					real ticks = (real)(now - skate_shoot.course_time[0]);

					skate_shoot.detonated_course.i = (target->object.position.x - skate_shoot.course[0].x) / ticks;
					skate_shoot.detonated_course.j = (target->object.position.y - skate_shoot.course[0].y) / ticks;
					skate_shoot.detonated_course.k = (target->object.position.z - skate_shoot.course[0].z) / ticks;
				}
				/* (and on its fuse: one that went off at once, on something in its
				way, is tried again) */
				if (skate_shoot.local && target->object.body_vitality > 0.f &&
					now - skate_shoot.grenade_time >= TICKS_PER_SECOND &&
					(target->object.shield_vitality < skate_shoot.grenade_shield ||
					target->object.body_vitality < skate_shoot.grenade_body))
				{
					skate_shoot.own_grenades++;
				}
				platform_log("skate test: the grenade went off at tick %ld (%s), %.2f world units from the skater (%s): "
					"shield %.2f to %.2f, body %.2f to %.2f",
					now, skate_shoot.bounced ? "after bouncing off them" : "without bouncing off them",
					(real)sqrt(distance_squared3d(&skate_shoot.grenade_at, &target->object.position)),
					skate_shoot.local ? "this machine's" : "another machine's", skate_shoot.grenade_shield,
					target->object.shield_vitality, skate_shoot.grenade_body, target->object.body_vitality);
			}
		}
		if (skate_shoot.detonated_time != NONE && now - skate_shoot.detonated_time == TICKS_PER_SECOND / 3)
		{
			/* (how far they went from the blast, level, past where their
			course would have had them, and how much faster away from it) */
			real_vector3d away;
			real past = 0.f, faster = 0.f;
			long ticks = now - skate_shoot.detonated_time;

			away.i = skate_shoot.detonated_at.x - skate_shoot.grenade_at.x;
			away.j = skate_shoot.detonated_at.y - skate_shoot.grenade_at.y;
			away.k = 0.f;
			if (normalize3d(&away) != 0.f)
			{
				past = (target->object.position.x - skate_shoot.detonated_at.x -
					skate_shoot.detonated_course.i * ticks) * away.i +
					(target->object.position.y - skate_shoot.detonated_at.y - skate_shoot.detonated_course.j * ticks) *
					away.j;
				faster = ((target->object.translational_velocity.i - skate_shoot.detonated_velocity.i) * away.i +
					(target->object.translational_velocity.j - skate_shoot.detonated_velocity.j) * away.j) *
					TICKS_PER_SECOND * 3.048f;
			}
			platform_log("skate test: after the grenade the skater (%s) was %.2f world units on, going %.1f m/s, "
				"shield %.2f, body %.2f; %.2f world units further from the blast than their course (%.1f m/s), "
				"%.1f m/s faster away from it", skate_shoot.local ? "this machine's" : "another machine's",
				(real)sqrt(distance_squared3d(&skate_shoot.detonated_at, &target->object.position)),
				magnitude3d(&target->object.translational_velocity) * TICKS_PER_SECOND * 3.048f,
				target->object.shield_vitality, target->object.body_vitality, past,
				magnitude3d(&skate_shoot.detonated_course) * TICKS_PER_SECOND * 3.048f, faster);
			skate_shoot.detonated_time = NONE;
		}
	}
	/* (a burst not fired, or not harming them, in SKATE_BURST_SECONDS: given up) */
	if (skate_shoot.burst_wanted && now - skate_shoot.burst_time > SKATE_BURST_SECONDS * TICKS_PER_SECOND)
	{
		platform_log("skate test: no burst at a skater (%s) harmed them in %d seconds, %d fired; given up",
			skate_shoot.burst_local ? "this machine's" : "another machine's", SKATE_BURST_SECONDS, skate_shoot.bursts);
		skate_shoot.burst_wanted = FALSE;
	}
	/* (a burst wanted, fired once no grenade is about, and the line clear) */
	if (skate_shoot.burst_wanted && skate_shoot.grenade_index == NONE && skate_shoot.detonated_time == NONE &&
		skate_shoot_burst(watcher_index, now))
	{
		platform_log("skate test: shooting a skater (%s) %.2f world units away, a burst (again, %d) and a grenade (none) "
			"at tick %ld", skate_shoot.local ? "this machine's" : "another machine's",
			(real)sqrt(distance_squared3d(&object_get(skate_shoot.target_index)->object.position,
			&watcher->object.position)), skate_shoot.bursts, now);
	}
	/* (nothing shot about: the next shot may go) */
	if (skate_shoot.burst_wanted || skate_shoot.bullet_time != NONE || skate_shoot.grenade_index != NONE ||
		skate_shoot.detonated_time != NONE)
	{
		return;
	}
	due = skate_shoot.next < (short)NUMBEROF(skate_shoot_seconds) &&
		now >= skate_shoot_seconds[skate_shoot.next].seconds * TICKS_PER_SECOND;
	if (due)
	{
		boolean local = skate_shoot_seconds[skate_shoot.next].own;
		boolean burst = skate_shoot_seconds[skate_shoot.next].burst;
		boolean grenade = skate_shoot_seconds[skate_shoot.next].grenade;
		long target_index = skate_shoot_target(watcher_index, local);
		real_vector3d direction;
		boolean clear;

		/* (a shot at the host's own skater who does not skate now waits; one
		at another machine's whom there is none of waits too) */
		if (target_index == NONE)
			return;
		clear = skate_shoot_clear(target_index, local ? NONE : watcher_index, SKATE_THROW_DISTANCE, 0.f,
			&direction);
		skate_shoot.next++;
		skate_shoot.bursts = 0;
		skate_shoot.bullet_time = NONE;
		skate_shoot.burst_local = local;
		skate_shoot.burst_wanted = burst;
		skate_shoot.burst_time = now;
		if (burst)
			skate_shoot_burst(watcher_index, now);
		if (grenade)
		{
			/* (a line not clear at a grenade's throw: from their front all the
			same, as before; it bounces off whatever is there) */
			if (!clear)
			{
				struct object_datum *target = object_get(target_index);

				direction.i = -target->object.forward.i;
				direction.j = -target->object.forward.j;
				direction.k = 0.f;
				if (normalize3d(&direction) == 0.f)
				{
					direction.i = 1.f;
					direction.j = 0.f;
				}
			}
			skate_shoot_grenade(watcher_index, target_index, local, &direction, now);
		}
		platform_log("skate test: shooting a skater (%s) %.2f world units away, a burst%s and a grenade%s at tick %ld",
			local ? "this machine's" : "another machine's",
			(real)sqrt(distance_squared3d(&object_get(target_index)->object.position, &watcher->object.position)),
			skate_shoot.bullet_time != NONE ? "" : burst ? " (later: no clear line)" : " (none)",
			skate_shoot.grenade_index != NONE ? "" : " (none)", now);
	}
	else if (skate_shoot.own_grenades < SKATE_OWN_GRENADES && now >= SKATE_OWN_GRENADES_FROM * TICKS_PER_SECOND &&
		now <= SKATE_OWN_GRENADES_UNTIL * TICKS_PER_SECOND && now % TICKS_PER_SECOND == 0 &&
		skate_shoot_target(watcher_index, TRUE) == watcher_index)
	{
		real_vector3d direction;
		long other_index = skate_shoot_target(watcher_index, FALSE);

		/* (no other skater near, to be hurt too or ride into them while the
		push is measured) */
		if ((other_index != NONE && distance_squared3d(&object_get(other_index)->object.position,
			&watcher->object.position) < SKATE_OWN_GRENADE_ALONE * SKATE_OWN_GRENADE_ALONE) ||
			!skate_shoot_free(watcher) ||
			!skate_shoot_clear(watcher_index, NONE, SKATE_THROW_DISTANCE, SKATE_PUSH_ROOM, &direction))
		{
			return;
		}
		/* (healed, as by a health pack, and a full shield: the frag hurts
		them, and does not kill them) */
		object_restore_body(watcher_index);
		if (watcher->object.shield_vitality < 1.f)
		{
			watcher->object.shield_vitality = 1.f;
			watcher->object.shield_stun_ticks = 0;
		}
		skate_shoot.bullet_time = NONE;
		skate_shoot_grenade(watcher_index, watcher_index, TRUE, &direction, now);
		platform_log("skate test: shooting a skater (this machine's) %.2f world units away, a burst (none) and a "
			"grenade%s at tick %ld", 0.f, skate_shoot.grenade_index != NONE ? "" : " (none)", now);
	}
}


/* debug.network_test_skate_teleport: that many seconds into a game a host
skating itself (debug.network_test_skate_duel) puts its skater on the first
teleporter's entrance, where Halo's own teleporter
(game_engine_update_teleporter) takes them to its exit that tick, turned by
the exit's facing less the entrance's, and the skate engine's skater follows
(skate.c's skate_follow_teleport); the other machines see them there as the
host has them */
static void skate_network_test_teleport(void)
{
	static real start = -1.f;
	struct scenario *scenario = global_scenario_get();
	long unit_index = player_control_get_unit_index(0);
	short index;

	if (start < 0.f)
		start = (real)config_real("debug.network_test_skate_teleport");
	if (!(start > 0.f) || !game_in_progress() || game_connection() == _game_connection_network_client || !scenario ||
		game_time_get() != (long)(start * TICKS_PER_SECOND) || unit_index == NONE || !skate_unit_is_skating(unit_index))
	{
		return;
	}
	for (index = 0; index < scenario->netgame_flags.count; index++)
	{
		struct scenario_netgame_flag *flag = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, index,
			struct scenario_netgame_flag);
		struct object_datum *object = object_get(unit_index);

		if (flag->type != _netgame_flag_teleporter_source)
			continue;
		platform_log("skate test: this machine's skater put on a teleporter's entrance %.2f world units away, at "
			"(%.2f %.2f %.2f)", (real)sqrt(distance_squared3d(&object->object.position, &flag->position)),
			flag->position.x, flag->position.y, flag->position.z);
		object_set_position(unit_index, &flag->position, &object->object.forward, NULL);
		return;
	}
}

void skate_network_update_after_objects(void)
{
	boolean log = skate_network_globals.log && game_time_get() % TICKS_PER_SECOND == 0 &&
		game_time_get() != skate_network_globals.logged_time;
	short index;

	skate_network_test_shoot();
	skate_network_test_teleport();
	/* (the host's verdicts on skaters' hits: skate_authority.c) */
	skate_authority_update();
	if (log)
		skate_network_globals.logged_time = game_time_get();
	for (index = 0; index < SKATE_NETWORK_PLAYERS; index++)
	{
		struct skate_network_player *player = &skate_network_players[index];
		struct skate_network_skeleton *skeleton;
		struct skate_state state;
		struct object_datum *object;
		real_matrix4x3 *node_matrices;
		long unit_index;
		byte player_index;
		boolean posed = FALSE;
		short count;

		if (!skate_network_player_skating(index) || (unit_index = skate_network_player_unit(index)) == NONE ||
			!skate_state_read(player->entry, player->entry + player->size, &player_index, &state))
		{
			player->has_latest = FALSE;
			player->bailing = FALSE;
			continue;
		}
		/* (a bail seen here, as their machine says: the network test's log) */
		if (TEST_FLAG(state.flags, _skate_state_bailing_bit) != player->bailing)
		{
			player->bailing = TEST_FLAG(state.flags, _skate_state_bailing_bit);
			if (player->bailing && skate_network_globals.log)
				platform_log("skate network: player %d bails on another machine, at tick %ld", index, game_time_get());
		}
		object = object_get(unit_index);
		node_matrices = (real_matrix4x3 *)object_header_block_get(unit_index, &object->object.node_matrices);
		count = (short)(object->object.node_matrices.size / (int)sizeof(real_matrix4x3));
		skeleton = skate_network_skeleton(unit_index);
		if (skeleton && skeleton->count == count &&
			skate_pose_rebuild(&state, skeleton->defaults, skeleton->parents, count, &object->object.position,
			node_matrices))
		{
			posed = TRUE;
		}
		/* (posed or not, no weapon in hand on a board) */
		skate_holster_weapon(unit_index, node_matrices, count);
		if (TEST_FLAG(state.flags, _skate_state_board_bit))
		{
			real placement[7];

			memcpy(placement, state.board_rotation, 4 * sizeof(real));
			placement[4] = object->object.position.x + state.board_offset.i;
			placement[5] = object->object.position.y + state.board_offset.j;
			placement[6] = object->object.position.z + state.board_offset.k;
			skate_network_board_note(player, unit_index, placement);
		}
		else
		{
			player->has_latest = FALSE;
		}
		if (log)
		{
			real_vector3d const *velocity = &object->object.translational_velocity;
			long taken = 0, refused = 0, refused_on_foot = 0;

			if (game_connection() == _game_connection_network_server)
				network_distributed_skate_predictions(&taken, &refused, &refused_on_foot);
			platform_log("skate network: player %d skates on another machine at (%.2f %.2f %.2f) %.1f m/s%s: "
				"%s %d nodes, %s, state %ld ticks old | host took %ld of its places, refused %ld (on foot's "
				"bounds would refuse %ld)", index, object->object.position.x, object->object.position.y,
				object->object.position.z, sqrt(velocity->i * velocity->i + velocity->j * velocity->j +
				velocity->k * velocity->k) * TICKS_PER_SECOND * 3.048f,
				TEST_FLAG(state.flags, _skate_state_off_board_bit) ? " off the board" : "",
				posed ? "posed" : "not posed,", state.node_count,
				player->has_latest ? "the board drawn" : "no board", game_time_get() - player->time,
				taken, refused, refused_on_foot);
		}
	}
}

boolean skate_network_board(short player_index, real fraction, long *unit_index, real *placement)
{
	struct skate_network_player const *player;
	real const *latest;
	real const *previous;
	real dot = 0.f;
	real length = 0.f;
	short index;

	if (player_index < 0 || player_index >= SKATE_NETWORK_PLAYERS)
		return FALSE;
	player = &skate_network_players[player_index];
	if (!player->has_latest || player->local)
		return FALSE;
	*unit_index = player->board_unit_index;
	latest = player->boards[player->latest];
	memcpy(placement, latest, 7 * sizeof(real));
	if (!player->has_previous || !(fraction < 1.f))
		return TRUE;
	/* (between the last two: the turn the shorter way, made one long) */
	previous = player->boards[player->latest ^ 1];
	for (index = 0; index < 4; index++)
		dot += previous[index] * latest[index];
	for (index = 0; index < 4; index++)
	{
		real from = dot < 0.f ? -previous[index] : previous[index];

		placement[index] = from + (latest[index] - from) * fraction;
		length += placement[index] * placement[index];
	}
	length = (real)sqrt(length);
	for (index = 0; index < 4; index++)
		placement[index] = length > 0.f ? placement[index] / length : latest[index];
	for (index = 4; index < 7; index++)
		placement[index] = previous[index] + (latest[index] - previous[index]) * fraction;
	return TRUE;
}

/* whether the network test may put its watcher at spot near the skater
skater_index: nothing of the level's between them (not into a rock, a wall
or a hill) and ground under it (not off a cliff), so that the watcher is
never put where it falls out of the level and dies, missing its turns */
static boolean skate_network_open_spot(long skater_index, real_point3d const *spot)
{
	struct collision_result collision;
	real_point3d from = object_get(skater_index)->object.position;
	real_vector3d along, down;

	from.z += 0.3f;
	along.i = spot->x - from.x;
	along.j = spot->y - from.y;
	along.k = spot->z - from.z;
	down.i = 0.f;
	down.j = 0.f;
	down.k = -SKATE_NETWORK_SPOT_GROUND;
	return !collision_test_vector(_collision_test_environment_flags, &from, &along, skater_index, &collision) &&
		collision_test_vector(_collision_test_environment_flags, spot, &down, skater_index, &collision);
}

boolean skate_local_player_watching(short local_player_index, real *yaw, real *pitch)
{
	long unit_index = player_control_get_unit_index(local_player_index);
	struct object_datum *object = unit_index != NONE ? object_try_and_get(unit_index) : NULL;
	real nearest = 0.f;
	boolean found = FALSE;
	short index;

	long nearest_index = NONE;

	if (!skate_network_globals.watch || !object)
		return FALSE;
	for (index = 0; index < SKATE_NETWORK_PLAYERS; index++)
	{
		long skater_index = skate_network_player_skating(index) ? skate_network_player_unit(index) : NONE;
		real_point3d const *at;
		real dx, dy, dz, distance;

		if (skater_index == NONE)
			continue;
		at = &object_get(skater_index)->object.position;
		dx = at->x - object->object.position.x;
		dy = at->y - object->object.position.y;
		dz = at->z - object->object.position.z;
		distance = dx * dx + dy * dy + dz * dz;
		if (!found || distance < nearest)
		{
			found = TRUE;
			nearest = distance;
			nearest_index = skater_index;
			*yaw = (real)atan2(dy, dx);
			if (*yaw < 0.f)
				*yaw += 2.f * _pi;
			*pitch = (real)atan2(dz, sqrt(dx * dx + dy * dy));
		}
	}
	/* (further than it is seen well from: stood nearer, a way off it toward
where the watcher was, a little above the ground to fall onto it) */
	if (found && nearest > SKATE_NETWORK_WATCH_DISTANCE * SKATE_NETWORK_WATCH_DISTANCE &&
		game_connection() != _game_connection_network_client)
	{
		real_point3d const *at = &object_get(nearest_index)->object.position;
		real_point3d position;

		position.x = at->x - (real)cos(*yaw) * SKATE_NETWORK_WATCH_NEAR;
		position.y = at->y - (real)sin(*yaw) * SKATE_NETWORK_WATCH_NEAR;
		position.z = at->z + 0.3f;
		if (skate_network_open_spot(nearest_index, &position))
		{
			object_set_position(unit_index, &position, &object->object.forward, &object->object.up);
			*pitch = (real)atan2(-0.3f, SKATE_NETWORK_WATCH_NEAR);
		}
	}
	/* debug.network_test_skate_block: now and then the watcher steps into the
	skater's way (skate_network_block_spot) */
	if (found)
	{
		real_point3d position;

		if (skate_network_block_spot(unit_index, FALSE, &position, NULL))
			object_set_position(unit_index, &position, &object->object.forward, &object->object.up);
	}
	return found && *yaw >= 0.f && *yaw < 2.f * _pi;
}

boolean skate_network_block_spot(long unit_index, boolean on_board, real_point3d *spot, real *yaw)
{
	struct object_datum *object = unit_index != NONE ? object_try_and_get(unit_index) : NULL;
	struct object_datum *skater = NULL;
	long nearest_index = NONE;
	real nearest = 0.f;
	real_vector3d const *velocity;
	real speed;
	short index;

	if (!skate_network_globals.block || !object || game_connection() == _game_connection_network_client ||
		(skate_network_globals.blocked_time != NONE &&
		game_time_get() - skate_network_globals.blocked_time < SKATE_NETWORK_BLOCK_TICKS))
	{
		return FALSE;
	}
	/* (the host's own skater not moved while the test times what moves them:
	near its teleport, which putting them elsewhere that tick would undo, nor
	while a grenade thrown at them is about or its push is measured) */
	if (on_board)
	{
		real teleport = (real)config_real("debug.network_test_skate_teleport");
		long since = game_time_get() - (long)(teleport * TICKS_PER_SECOND);

		if ((teleport > 0.f && since >= -SKATE_NETWORK_BLOCK_CLEAR && since <= SKATE_NETWORK_BLOCK_CLEAR) ||
			(skate_shoot.local && (skate_shoot.grenade_index != NONE || skate_shoot.detonated_time != NONE)))
		{
			return FALSE;
		}
	}
	for (index = 0; index < SKATE_NETWORK_PLAYERS; index++)
	{
		long skater_index = skate_network_player_skating(index) ? skate_network_player_unit(index) : NONE;
		real_point3d const *at;
		real dx, dy, dz, distance;

		if (skater_index == NONE || skater_index == unit_index)
			continue;
		at = &object_get(skater_index)->object.position;
		dx = at->x - object->object.position.x;
		dy = at->y - object->object.position.y;
		dz = at->z - object->object.position.z;
		distance = dx * dx + dy * dy + dz * dz;
		if (!skater || distance < nearest)
		{
			skater = object_get(skater_index);
			nearest_index = skater_index;
			nearest = distance;
		}
	}
	if (!skater)
		return FALSE;
	velocity = &skater->object.translational_velocity;
	speed = (real)sqrt(velocity->i * velocity->i + velocity->j * velocity->j);
	if (!(speed * TICKS_PER_SECOND > SKATE_NETWORK_BLOCK_SPEED))
		return FALSE;
	spot->x = skater->object.position.x + velocity->i / speed * SKATE_NETWORK_BLOCK_AHEAD;
	spot->y = skater->object.position.y + velocity->j / speed * SKATE_NETWORK_BLOCK_AHEAD;
	spot->z = skater->object.position.z + 0.3f;
	/* (not into the level or off it: tried again next tick) */
	if (!skate_network_open_spot(nearest_index, spot))
		return FALSE;
	/* (facing the skater, who comes along their velocity) */
	if (yaw)
	{
		*yaw = (real)atan2(-velocity->j, -velocity->i);
		if (*yaw < 0.f)
			*yaw += 2.f * _pi;
	}
	skate_network_globals.blocked_time = game_time_get();
	platform_log("skate network: the watcher steps into the skater's way%s at (%.2f %.2f %.2f), "
		"the skater going %.1f m/s", on_board ? " on a board" : "", spot->x, spot->y, spot->z,
		speed * TICKS_PER_SECOND * 3.048f);
	return TRUE;
}
