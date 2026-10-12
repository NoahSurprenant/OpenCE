/*
SKATE_STATE.C (test)

The real skate state of port/linux/game/skate_network.c, as Skate 3 mode
sends it in a network game: a skater's pose as each node's turn from its
bind pose, written to its bytes and read back, and rebuilt as rig.rs poses
it, within a hair of where it was; and what another machine sends that is
not a state refused. test_skate_state.py takes the sizes, flags and the
state (config.inc) and the code (under_test.inc).
*/

#include "harness.h"

#include <math.h>

typedef unsigned short word;
typedef struct real_matrix4x3
{
	real scale;
	real_vector3d forward;
	real_vector3d left;
	real_vector3d up;
	real_point3d position;
} real_matrix4x3;

#include "config.inc"
#include "under_test.inc"

#define NODES 19

static unsigned long random_state = 12345;

static real random_real(real low, real high)
{
	random_state = random_state * 1103515245UL + 12345UL;
	return low + (high - low) * (real)((random_state >> 8) & 0xFFFFFF) / (real)0xFFFFFF;
}

static void random_quaternion(real *q)
{
	real length = 0.f;
	short index;

	for (index = 0; index < 4; index++)
	{
		q[index] = random_real(-1.f, 1.f);
		length += q[index] * q[index];
	}
	length = (real)sqrt(length);
	for (index = 0; index < 4; index++)
		q[index] /= length;
}

/* a quaternion's turn of a vector, written out as a matrix (independently
of the code under test) */
static void turn(real const *q, real_vector3d const *v, real_vector3d *out)
{
	real x = q[0], y = q[1], z = q[2], w = q[3];

	out->i = (1 - 2 * (y * y + z * z)) * v->i + 2 * (x * y - z * w) * v->j + 2 * (x * z + y * w) * v->k;
	out->j = 2 * (x * y + z * w) * v->i + (1 - 2 * (x * x + z * z)) * v->j + 2 * (y * z - x * w) * v->k;
	out->k = 2 * (x * z - y * w) * v->i + 2 * (y * z + x * w) * v->j + (1 - 2 * (x * x + y * y)) * v->k;
}

static void axes(real const *q, real_matrix4x3 *m)
{
	real_vector3d x = { 1, 0, 0 }, y = { 0, 1, 0 }, z = { 0, 0, 1 };

	turn(q, &x, &m->forward);
	turn(q, &y, &m->left);
	turn(q, &z, &m->up);
}

/* a biped's skeleton: each node's parent before it, its bind pose turned and
placed at random about its parent's */
static void skeleton(short *parents, real_matrix4x3 *defaults)
{
	short index;

	for (index = 0; index < NODES; index++)
	{
		real q[4];

		parents[index] = index ? (short)(random_real(0.f, (real)index - 0.01f)) : NONE;
		random_quaternion(q);
		axes(q, &defaults[index]);
		defaults[index].scale = 1.f;
		if (index)
		{
			defaults[index].position.x = defaults[parents[index]].position.x + random_real(-0.2f, 0.2f);
			defaults[index].position.y = defaults[parents[index]].position.y + random_real(-0.2f, 0.2f);
			defaults[index].position.z = defaults[parents[index]].position.z + random_real(-0.2f, 0.2f);
		}
		else
		{
			defaults[0].position.x = 0.f;
			defaults[0].position.y = 0.f;
			defaults[0].position.z = 0.3f;
		}
	}
}

/* the skeleton posed as rig.rs poses it: each node's turn times its bind
pose, at its parent's place plus its parent's turn of their offset */
static void pose(short const *parents, real_matrix4x3 const *defaults, real turns[][4], real_point3d const *root,
	real_matrix4x3 *nodes)
{
	short index;

	for (index = 0; index < NODES; index++)
	{
		nodes[index].scale = defaults[index].scale;
		turn(turns[index], &defaults[index].forward, &nodes[index].forward);
		turn(turns[index], &defaults[index].left, &nodes[index].left);
		turn(turns[index], &defaults[index].up, &nodes[index].up);
		if (!index)
		{
			nodes[0].position = *root;
		}
		else
		{
			short parent = parents[index];
			real_vector3d offset, turned;

			offset.i = defaults[index].position.x - defaults[parent].position.x;
			offset.j = defaults[index].position.y - defaults[parent].position.y;
			offset.k = defaults[index].position.z - defaults[parent].position.z;
			turn(turns[parent], &offset, &turned);
			nodes[index].position.x = nodes[parent].position.x + turned.i;
			nodes[index].position.y = nodes[parent].position.y + turned.j;
			nodes[index].position.z = nodes[parent].position.z + turned.k;
		}
	}
}

static real dot(real_vector3d const *a, real_vector3d const *b)
{
	return a->i * b->i + a->j * b->j + a->k * b->k;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	short parents[NODES];
	real_matrix4x3 defaults[NODES], nodes[NODES], rebuilt[NODES];
	real turns[NODES][4];
	struct skate_state state, read_back;
	byte bytes[SKATE_STATE_MAXIMUM_SIZE];
	byte player_index;
	short size;
	short trial;
	real_point3d elsewhere_origin = { 1.f, 2.f, 3.f };
	short index;

	/* a pose sent and rebuilt on another machine, around another place for
	the unit, is the same pose there: within a millimetre and a third of a
	degree at every node */
	CASE("round-trip")
	{
		real worst_distance = 0.f, worst_cosine = 1.f;

		for (trial = 0; trial < 200; trial++)
		{
			real_point3d origin = { random_real(-100, 100), random_real(-100, 100), random_real(-10, 10) };
			real_point3d elsewhere = { origin.x + 3.f, origin.y - 2.f, origin.z + 0.5f };
			real_point3d root = { origin.x + random_real(-0.3f, 0.3f), origin.y + random_real(-0.3f, 0.3f),
				origin.z + random_real(-0.1f, 0.4f) };

			skeleton(parents, defaults);
			for (index = 0; index < NODES; index++)
				random_quaternion(turns[index]);
			pose(parents, defaults, turns, &root, nodes);
			memset(&state, 0, sizeof(state));
			state.flags = FLAG(_skate_state_skating_bit) | FLAG(_skate_state_board_bit);
			state.board_offset.i = 0.1f;
			random_quaternion(state.board_rotation);
			skate_pose_turns(nodes, defaults, NODES, &origin, &state);
			size = skate_state_write(7, &state, bytes);
			CHECK(size == SKATE_STATE_HEADER_SIZE + 4 * NODES, "a state of %d nodes is %d bytes", NODES, size);
			CHECK(skate_state_read(bytes, bytes + size, &player_index, &read_back) == size, "the state not read back");
			CHECK(player_index == 7 && read_back.flags == state.flags && read_back.node_count == NODES,
				"the state's player, flags or nodes changed");
			CHECK(skate_pose_rebuild(&read_back, defaults, parents, NODES, &elsewhere, rebuilt), "not rebuilt");
			for (index = 0; index < NODES; index++)
			{
				real dx = rebuilt[index].position.x - (nodes[index].position.x + elsewhere.x - origin.x);
				real dy = rebuilt[index].position.y - (nodes[index].position.y + elsewhere.y - origin.y);
				real dz = rebuilt[index].position.z - (nodes[index].position.z + elsewhere.z - origin.z);
				real distance = (real)sqrt(dx * dx + dy * dy + dz * dz);
				real cosine = dot(&rebuilt[index].forward, &nodes[index].forward);

				if (dot(&rebuilt[index].left, &nodes[index].left) < cosine)
					cosine = dot(&rebuilt[index].left, &nodes[index].left);
				if (dot(&rebuilt[index].up, &nodes[index].up) < cosine)
					cosine = dot(&rebuilt[index].up, &nodes[index].up);
				if (distance > worst_distance)
					worst_distance = distance;
				if (cosine < worst_cosine)
					worst_cosine = cosine;
				CHECK(rebuilt[index].scale == defaults[index].scale, "node %d's scale changed", index);
			}
			/* the board's turn, the same turn (q or -q) */
			{
				real d = 0.f;

				for (index = 0; index < 4; index++)
					d += read_back.board_rotation[index] * state.board_rotation[index];
				CHECK(fabs(d) > 0.99999f, "the board's turn changed (%f)", d);
			}
		}
		printf("worst node %.5f world units, %.3f degrees off\n", worst_distance,
			acos(worst_cosine > 1.f ? 1.f : worst_cosine) * 180.0 / 3.14159265);
		CHECK(worst_distance < 0.003f, "a node %.5f world units off", worst_distance);
		CHECK(worst_cosine > 0.99998f, "a node %.3f degrees off", acos(worst_cosine) * 180.0 / 3.14159265);
		return 0;
	}

	/* an offset too far (or not a number) to send leaves out what it places:
	the pose, or the board; another skeleton is not posed by the state */
	CASE("offsets")
	{
		memset(&state, 0, sizeof(state));
		state.flags = FLAG(_skate_state_skating_bit) | FLAG(_skate_state_board_bit);
		state.node_count = NODES;
		state.board_rotation[3] = 1.f;
		for (index = 0; index < NODES; index++)
			state.turns[index][3] = 1.f;
		state.root_offset.i = 40.f;
		size = skate_state_write(1, &state, bytes);
		CHECK(size == SKATE_STATE_HEADER_SIZE && bytes[2] == 0, "a root 40 world units off was sent");
		state.root_offset.i = 0.f;
		state.board_offset.k = NAN;
		size = skate_state_write(1, &state, bytes);
		CHECK(size == SKATE_STATE_HEADER_SIZE + 4 * NODES, "the pose left out with the board");
		CHECK(skate_state_read(bytes, bytes + size, &player_index, &read_back) == size &&
			!TEST_FLAG(read_back.flags, _skate_state_board_bit), "a board at NaN was sent");
		skeleton(parents, defaults);
		CHECK(!skate_pose_rebuild(&read_back, defaults, parents, NODES - 1, &elsewhere_origin, rebuilt),
			"a state of %d nodes posed a skeleton of %d", NODES, NODES - 1);
		parents[3] = 5;
		CHECK(!skate_pose_rebuild(&read_back, defaults, parents, NODES, &elsewhere_origin, rebuilt),
			"a skeleton with a child before its parent was posed");
		return 0;
	}

	/* what is no state is refused, and nothing read is past the end or not
	a number */
	CASE("malformed")
	{
		memset(&state, 0, sizeof(state));
		state.flags = FLAG(_skate_state_skating_bit) | FLAG(_skate_state_board_bit);
		state.node_count = NODES;
		state.board_rotation[3] = 1.f;
		for (index = 0; index < NODES; index++)
			random_quaternion(state.turns[index]);
		size = skate_state_write(2, &state, bytes);
		CHECK(skate_state_read(bytes, bytes + size - 1, &player_index, &read_back) == 0, "a state cut short was read");
		CHECK(skate_state_read(bytes, bytes + SKATE_STATE_HEADER_SIZE - 1, &player_index, &read_back) == 0,
			"a header cut short was read");
		bytes[3] = SKATE_STATE_FORMAT + 1;
		CHECK(skate_state_read(bytes, bytes + size, &player_index, &read_back) == 0, "another format was read");
		bytes[3] = SKATE_STATE_FORMAT;
		bytes[2] = SKATE_STATE_MAXIMUM_NODES + 1;
		CHECK(skate_state_read(bytes, bytes + sizeof(bytes), &player_index, &read_back) == 0, "too many nodes read");
		bytes[2] = NODES;
		/* three parts at their most: no unit quaternion's */
		bytes[SKATE_STATE_HEADER_SIZE] = 0xFF;
		bytes[SKATE_STATE_HEADER_SIZE + 1] = 0xFF;
		bytes[SKATE_STATE_HEADER_SIZE + 2] = 0xFF;
		bytes[SKATE_STATE_HEADER_SIZE + 3] = 0x3F;
		CHECK(skate_state_read(bytes, bytes + size, &player_index, &read_back) == 0, "a turn that is none was read");
		/* bytes at random: refused or read as a state of their length, every
		part a number */
		for (trial = 0; trial < 20000; trial++)
		{
			short length = (short)random_real(0.f, (real)sizeof(bytes));

			for (index = 0; index < length; index++)
				bytes[index] = (byte)random_real(0.f, 255.99f);
			if (trial & 1)
				bytes[3] = SKATE_STATE_FORMAT;
			size = skate_state_read(bytes, bytes + length, &player_index, &read_back);
			CHECK(size >= 0 && size <= length, "%d bytes read of %d", size, length);
			if (!size)
				continue;
			CHECK(size == SKATE_STATE_HEADER_SIZE + 4 * read_back.node_count, "a state's size is not its nodes'");
			for (index = 0; index < read_back.node_count; index++)
			{
				real length2 = read_back.turns[index][0] * read_back.turns[index][0] + read_back.turns[index][1] *
					read_back.turns[index][1] + read_back.turns[index][2] * read_back.turns[index][2] +
					read_back.turns[index][3] * read_back.turns[index][3];

				CHECK(fabs(length2 - 1.f) < 0.001f, "a turn read is %f long", length2);
			}
			CHECK(fabs(read_back.root_offset.i) <= 32.f && fabs(read_back.board_offset.k) <= 32.f,
				"an offset read past 32 world units");
		}
		return 0;
	}

	fprintf(stderr, "unknown case %s\n", case_name);
	return 2;
}
