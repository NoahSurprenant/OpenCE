/* skate_hitbox.c: a skater's hit shapes, where their body is drawn (skate.h)

A biped is hit where its collision model is: each node's collision BSP
(collision_models.c) placed by the node's matrix as the object has it when
a projectile, a bullet's line or a melee is tested (collisions.c), during
objects_update. A skater's body is drawn as the skate engine poses it:
this machine's skater from the engine (skate.c), another machine's from
their state (skate_network.c). Those nodes are written after
objects_update, for drawing; and Halo's own placing of the biped's nodes
(object_compute_node_matrices: object_set_position, as skate.c puts the
biped where the board is, and each object's update) would put its own
animation back in them during the tick, with its bounding sphere, which
collision passes over far objects by. So object_compute_node_matrices
asks here: a skater's nodes are the skate pose there too
(skate_pose_object_nodes), and Halo's touches after it (a unit's aim and
weapon IK, object_postprocess_node_matrices) are skipped
(skate_object_posed), so that what is hit is what is drawn.

debug.skate_hitbox_test checks it in a game, before and after
objects_update (skate_hitbox_test): for each skater, how far each node
the collision tests use is from the skate pose, and lines a bullet takes
at each part of the drawn body (each collision node's middle as the pose
places it, from the side and the front), what they hit as the body is
drawn and as collision has it, and lines just past the drawn body (over
the head, either side), which must hit nothing of the skater
(tools/test_linux_port.py's test_skate_hitbox). debug.skate_hitbox_dump
writes each state's first such check to a file, the shapes' corners too,
for pictures. */

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "physics/collision_bsp_definitions.h"
#include "physics/collision_model_definitions.h"
#include "physics/collision_models.h"
#include "physics/collisions.h"
#include "units/units.h"

#include "skate.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* the platform layer's (port_config.c, the port's log) */
int config_boolean(const char *name);
const char *config_string(const char *name);
void platform_log(const char *format, ...);
/* objects.c's copy of the bounding spheres collision reads (object_bounds_cache.c) */
void object_bounds_cache_update(long object_index, real_point3d const *center, real radius);

/* the most nodes a skater's pose has */
#define SKATE_HITBOX_MAXIMUM_NODES 64

/* ---------- the skate pose in the biped's nodes */

/* the skate pose of a skater's biped, this machine's or another's, into
nodes (count of them); FALSE when the object is no skater, or their pose is
not had */
static boolean skate_object_pose(long object_index, real_matrix4x3 *nodes, short count, char const **state)
{
	struct object_datum *object = object_try_and_get(object_index);

	if (state)
		*state = "";
	if (!object || object->object.type != _object_type_biped || count <= 0 || count > SKATE_HITBOX_MAXIMUM_NODES)
		return FALSE;
	return skate_local_pose(object_index, nodes, count, state) || skate_network_pose(object_index, nodes, count);
}

boolean skate_object_posed(long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);

	return object && object->object.type == _object_type_biped &&
		(skate_host_skater(object_index) || skate_network_unit_skating(object_index));
}

boolean skate_pose_object_nodes(long object_index)
{
	struct object_datum *object = object_get(object_index);
	real_matrix4x3 posed[SKATE_HITBOX_MAXIMUM_NODES];
	short count;

	if (object->object.type != _object_type_biped)
		return FALSE;
	count = (short)(object->object.node_matrices.size / (int)sizeof(real_matrix4x3));
	if (skate_object_pose(object_index, posed, count, NULL))
	{
		memcpy(object_header_block_get(object_index, &object->object.node_matrices), posed,
			sizeof(real_matrix4x3) * count);
		return TRUE;
	}
	return FALSE;
}

/* ---------- the check (debug.skate_hitbox_test)

The skater's body is drawn from the pose written after objects_update; the
nodes collision has at a check are compared with the pose the engine (or
their state) gives then. */

/* how often a skater is checked, ticks */
#define SKATE_HITBOX_TICKS 5
/* how far off the drawn body a line starts, and how long it is, world units */
#define SKATE_HITBOX_REACH 2.f
/* how far past the drawn body's shapes the lines that must miss go, world
units (18 cm; the shapes are the collision model's, near the drawn
model's skin) */
#define SKATE_HITBOX_CLEARANCE 0.06f
/* the most nodes with shapes, and corners of one node's shapes, a check
takes */
#define SKATE_HITBOX_MAXIMUM_SHAPES 32
#define SKATE_HITBOX_MAXIMUM_CORNERS 256
/* the most checks written to debug.skate_hitbox_dump, the states told
apart for it, and how often a check is written besides each state's first */
#define SKATE_HITBOX_MAXIMUM_DUMPS 48
#define SKATE_HITBOX_MAXIMUM_STATES 48
#define SKATE_HITBOX_DUMP_EVERY 40
/* the skaters whose last drawn nodes are kept */
#define SKATE_HITBOX_MAXIMUM_DRAWN 16

/* each skater's nodes as the last tick left them to be drawn */
static struct skate_hitbox_drawn
{
	long unit_index;
	long time;
	short count;
	real_matrix4x3 nodes[SKATE_HITBOX_MAXIMUM_NODES];
} skate_hitbox_drawn[SKATE_HITBOX_MAXIMUM_DRAWN];

static struct skate_hitbox_drawn *skate_hitbox_find_drawn(long unit_index, boolean make)
{
	struct skate_hitbox_drawn *oldest = &skate_hitbox_drawn[0];
	short index;

	for (index = 0; index < SKATE_HITBOX_MAXIMUM_DRAWN; index++)
	{
		if (skate_hitbox_drawn[index].unit_index == unit_index && skate_hitbox_drawn[index].count)
			return &skate_hitbox_drawn[index];
		if (skate_hitbox_drawn[index].time < oldest->time)
			oldest = &skate_hitbox_drawn[index];
	}
	if (!make)
		return NULL;
	oldest->unit_index = unit_index;
	oldest->count = 0;
	return oldest;
}

static void skate_hitbox_keep_drawn(long unit_index)
{
	struct object_datum *object = object_get(unit_index);
	short count = (short)(object->object.node_matrices.size / (int)sizeof(real_matrix4x3));
	struct skate_hitbox_drawn *drawn;

	if (count <= 0 || count > SKATE_HITBOX_MAXIMUM_NODES)
		return;
	drawn = skate_hitbox_find_drawn(unit_index, TRUE);
	drawn->time = game_time_get();
	drawn->count = count;
	memcpy(drawn->nodes, object_header_block_get(unit_index, &object->object.node_matrices),
		sizeof(real_matrix4x3) * count);
}

enum
{
	_skate_hitbox_head,
	_skate_hitbox_torso,
	_skate_hitbox_arms,
	_skate_hitbox_legs,
	NUMBER_OF_SKATE_HITBOX_PARTS
};

static char const *const skate_hitbox_part_names[NUMBER_OF_SKATE_HITBOX_PARTS] = { "head", "torso", "arms", "legs" };

/* a collision node's part of the body, by its name */
static short skate_hitbox_part(char const *name)
{
	char lower[32];
	short index;

	for (index = 0; index < 31 && name[index]; index++)
		lower[index] = (char)(name[index] >= 'A' && name[index] <= 'Z' ? name[index] - 'A' + 'a' : name[index]);
	lower[index] = 0;
	if (strstr(lower, "head") || strstr(lower, "neck"))
		return _skate_hitbox_head;
	if (strstr(lower, "thigh") || strstr(lower, "calf") || strstr(lower, "foot") || strstr(lower, "toe") ||
		strstr(lower, "leg"))
	{
		return _skate_hitbox_legs;
	}
	if (strstr(lower, "clavicle") || strstr(lower, "arm") || strstr(lower, "hand") || strstr(lower, "finger"))
		return _skate_hitbox_arms;
	return _skate_hitbox_torso;
}

/* a node's collision BSP, the permutation the object shows (skate.c's
skate_node_bsp) */
static struct collision_bsp *skate_hitbox_bsp(struct collision_model_instance const *instance, short node_index)
{
	struct collision_node const *node = TAG_BLOCK_GET_ELEMENT(&instance->model->nodes, node_index,
		struct collision_node);
	short permutation;

	if (node->region_index < 0 || node->region_index >= MAXIMUM_REGIONS_PER_OBJECT || node->bsps.count <= 0)
		return NULL;
	permutation = instance->region_permutation_indices[node->region_index];
	if (permutation == NONE)
		return NULL;
	permutation = PIN(permutation, 0, (short)(node->bsps.count - 1));
	return TAG_BLOCK_GET_ELEMENT(&node->bsps, permutation, struct collision_bsp);
}

/* what a line hits of the skater: the collision node, NONE for nothing of
them (another object first counts as nothing: the check's lines go through
no level) */
static short skate_hitbox_line(long unit_index, real_point3d const *start, real_vector3d const *vector,
	real_point3d *point)
{
	struct collision_result collision;
	unsigned long flags = _collision_test_for_projectiles_flags &
		~(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_media_bit));

	if (collision_test_vector(flags, start, vector, NONE, &collision) && collision.type == _collision_result_object &&
		collision.object_index == unit_index)
	{
		if (point)
			*point = collision.point;
		return collision.node_index;
	}
	return NONE;
}

/* the unit's nodes and bounding sphere put to nodes (count of them), and back
(saved: the nodes and the sphere they had) */
static void skate_hitbox_swap(long unit_index, real_matrix4x3 *nodes, short count, real_matrix4x3 *saved,
	real_point3d *saved_center, boolean back)
{
	struct object_datum *object = object_get(unit_index);
	struct object_definition *definition = object_definition_get(object->definition_index);
	real_matrix4x3 *node_matrices = (real_matrix4x3 *)object_header_block_get(unit_index, &object->object.node_matrices);

	if (back)
	{
		memcpy(node_matrices, saved, sizeof(real_matrix4x3) * count);
		object->object.bounding_sphere_center = *saved_center;
	}
	else
	{
		memcpy(saved, node_matrices, sizeof(real_matrix4x3) * count);
		*saved_center = object->object.bounding_sphere_center;
		memcpy(node_matrices, nodes, sizeof(real_matrix4x3) * count);
		matrix4x3_transform_point(&nodes[0], &definition->object.bounding_offset,
			&object->object.bounding_sphere_center);
	}
	object_bounds_cache_update(unit_index, &object->object.bounding_sphere_center,
		object->object.bounding_sphere_radius);
}

struct skate_hitbox_shape
{
	short node_index;
	short part;
	short corner_count;
	real_point3d middle;
	real_point3d drawn[SKATE_HITBOX_MAXIMUM_CORNERS];
	real_point3d hit[SKATE_HITBOX_MAXIMUM_CORNERS];
};

static struct
{
	boolean read;
	boolean enabled;
	FILE *dump;
	short dumps;
	char states[SKATE_HITBOX_MAXIMUM_STATES][40];
	short state_count;
	long checks;
} skate_hitbox_globals;

/* whether this check is the first of its state, or one of every few, to be
written (debug.skate_hitbox_dump) */
static boolean skate_hitbox_dump_wanted(char const *who, char const *state)
{
	char key[40];
	short index;

	if (!skate_hitbox_globals.dump || skate_hitbox_globals.dumps >= SKATE_HITBOX_MAXIMUM_DUMPS)
		return FALSE;
	if (++skate_hitbox_globals.checks % SKATE_HITBOX_DUMP_EVERY == 0)
	{
		skate_hitbox_globals.dumps++;
		return TRUE;
	}
	snprintf(key, sizeof(key), "%s %s", who, state);
	for (index = 0; index < skate_hitbox_globals.state_count; index++)
	{
		if (!strcmp(skate_hitbox_globals.states[index], key))
			return FALSE;
	}
	if (skate_hitbox_globals.state_count >= SKATE_HITBOX_MAXIMUM_STATES)
		return FALSE;
	strcpy(skate_hitbox_globals.states[skate_hitbox_globals.state_count++], key);
	skate_hitbox_globals.dumps++;
	return TRUE;
}

static void skate_hitbox_dump_point(FILE *file, real_point3d const *point)
{
	fprintf(file, "[%.4f,%.4f,%.4f]", point->x, point->y, point->z);
}

static void skate_hitbox_check(long unit_index, char const *who, boolean after_objects)
{
	static struct skate_hitbox_shape shapes[SKATE_HITBOX_MAXIMUM_SHAPES];
	struct object_datum *object = object_get(unit_index);
	real_matrix4x3 *node_matrices = (real_matrix4x3 *)object_header_block_get(unit_index, &object->object.node_matrices);
	struct object_definition *definition = object_definition_get(object->definition_index);
	real_matrix4x3 drawn[SKATE_HITBOX_MAXIMUM_NODES];
	real_matrix4x3 saved[SKATE_HITBOX_MAXIMUM_NODES];
	real_point3d saved_center, drawn_center;
	struct collision_model_instance instance;
	short count = (short)(object->object.node_matrices.size / (int)sizeof(real_matrix4x3));
	short shape_count = 0;
	char const *state = "";
	struct skate_hitbox_drawn const *last = skate_hitbox_find_drawn(unit_index, FALSE);
	real lag = -1.f;
	real most = 0.f, parts_off[NUMBER_OF_SKATE_HITBOX_PARTS];
	/* lines at each part: as many, hitting the part drawn, and as collision
	has it: the same node, another of the skater's, nothing */
	short lines[NUMBER_OF_SKATE_HITBOX_PARTS], on_part[NUMBER_OF_SKATE_HITBOX_PARTS];
	short same[NUMBER_OF_SKATE_HITBOX_PARTS], other[NUMBER_OF_SKATE_HITBOX_PARTS], missed[NUMBER_OF_SKATE_HITBOX_PARTS];
	short outside = 0, outside_drawn = 0, outside_hit = 0;
	/* how far the drawn shapes reach past the bounding sphere the drawn pose gives,
	which collision passes over objects by */
	real unbounded = 0.f;
	real_point3d lowest, highest, body;
	real_vector3d axes[2];
	short index, part, axis;
	boolean dump;
	FILE *file = skate_hitbox_globals.dump;

	if (count <= 0 || count > SKATE_HITBOX_MAXIMUM_NODES || !skate_object_pose(unit_index, drawn, count, &state) ||
		!collision_model_instance_new(&instance, unit_index) || !instance.matrices)
	{
		return;
	}
	memset(lines, 0, sizeof(lines));
	memset(on_part, 0, sizeof(on_part));
	memset(same, 0, sizeof(same));
	memset(other, 0, sizeof(other));
	memset(missed, 0, sizeof(missed));
	memset(parts_off, 0, sizeof(parts_off));
	/* each node's shapes, drawn and as collision has them */
	lowest = highest = drawn[0].position;
	for (index = 0; index < instance.node_count && shape_count < SKATE_HITBOX_MAXIMUM_SHAPES; index++)
	{
		struct collision_bsp *bsp = skate_hitbox_bsp(&instance, index);
		struct skate_hitbox_shape *shape = &shapes[shape_count];
		real_point3d sum = { 0.f, 0.f, 0.f };
		long corner;

		if (!bsp || bsp->vertices.count <= 0)
			continue;
		shape->node_index = index;
		shape->part = skate_hitbox_part(TAG_BLOCK_GET_ELEMENT(&instance.model->nodes, index, struct collision_node)->name);
		shape->corner_count = (short)MIN(bsp->vertices.count, SKATE_HITBOX_MAXIMUM_CORNERS);
		for (corner = 0; corner < shape->corner_count; corner++)
		{
			real_point3d const *at = &TAG_BLOCK_GET_ELEMENT(&bsp->vertices, corner, struct collision_vertex)->point;
			real_point3d *p = &shape->drawn[corner];
			real off;

			matrix4x3_transform_point(&drawn[index], at, p);
			matrix4x3_transform_point(&node_matrices[index], at, &shape->hit[corner]);
			sum.x += p->x; sum.y += p->y; sum.z += p->z;
			lowest.x = MIN(lowest.x, p->x); lowest.y = MIN(lowest.y, p->y); lowest.z = MIN(lowest.z, p->z);
			highest.x = MAX(highest.x, p->x); highest.y = MAX(highest.y, p->y); highest.z = MAX(highest.z, p->z);
			off = (real)sqrt(distance_squared3d(p, &shape->hit[corner]));
			parts_off[shape->part] = MAX(parts_off[shape->part], off);
			most = MAX(most, off);
			if (last && last->count == count && game_time_get() - last->time == 1)
			{
				real_point3d was;

				matrix4x3_transform_point(&last->nodes[index], at, &was);
				lag = MAX(lag, (real)sqrt(distance_squared3d(&was, &shape->hit[corner])));
			}
		}
		shape->middle.x = sum.x / shape->corner_count;
		shape->middle.y = sum.y / shape->corner_count;
		shape->middle.z = sum.z / shape->corner_count;
		shape_count++;
	}
	if (!shape_count)
		return;
	matrix4x3_transform_point(&drawn[0], &definition->object.bounding_offset, &drawn_center);
	for (index = 0; index < shape_count; index++)
	{
		short corner;

		for (corner = 0; corner < shapes[index].corner_count; corner++)
		{
			unbounded = MAX(unbounded, (real)sqrt(distance_squared3d(&shapes[index].drawn[corner], &drawn_center)) -
				object->object.bounding_sphere_radius);
		}
	}
	body.x = (lowest.x + highest.x) / 2.f;
	body.y = (lowest.y + highest.y) / 2.f;
	body.z = (lowest.z + highest.z) / 2.f;
	/* (level: the biped's forward, the board's heading, and its left) */
	axes[0].i = object->object.forward.i; axes[0].j = object->object.forward.j; axes[0].k = 0.f;
	if (normalize3d(&axes[0]) == 0.f)
	{
		axes[0].i = 1.f; axes[0].j = 0.f;
	}
	axes[1].i = -axes[0].j; axes[1].j = axes[0].i; axes[1].k = 0.f;

	dump = skate_hitbox_dump_wanted(who, state);
	if (dump)
	{
		fprintf(file, "{\"who\":\"%s\",\"when\":\"%s\",\"state\":\"%s\",\"tick\":%ld,\"position\":", who,
			after_objects ? "after" : "before", state, game_time_get());
		skate_hitbox_dump_point(file, &object->object.position);
		fprintf(file, ",\"forward\":[%.4f,%.4f],\"nodes\":[", axes[0].i, axes[0].j);
		{
			struct model *model = definition->object.model.index != NONE ?
				model_definition_get(definition->object.model.index) : NULL;

			for (index = 0; index < count; index++)
			{
				short parent = model && index < model->nodes.count ?
					TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->parent_node_index : NONE;

				fprintf(file, "%s{\"parent\":%d,\"drawn\":", index ? "," : "", parent);
				skate_hitbox_dump_point(file, &drawn[index].position);
				fprintf(file, ",\"hit\":");
				skate_hitbox_dump_point(file, &node_matrices[index].position);
				fprintf(file, "}");
			}
		}
		fprintf(file, "],\"shapes\":[");
		for (index = 0; index < shape_count; index++)
		{
			struct skate_hitbox_shape const *shape = &shapes[index];
			short corner;

			fprintf(file, "%s{\"node\":%d,\"part\":\"%s\",\"drawn\":[", index ? "," : "", shape->node_index,
				skate_hitbox_part_names[shape->part]);
			for (corner = 0; corner < shape->corner_count; corner++)
			{
				fprintf(file, corner ? "," : "");
				skate_hitbox_dump_point(file, &shape->drawn[corner]);
			}
			fprintf(file, "],\"hit\":[");
			for (corner = 0; corner < shape->corner_count; corner++)
			{
				fprintf(file, corner ? "," : "");
				skate_hitbox_dump_point(file, &shape->hit[corner]);
			}
			fprintf(file, "]}");
		}
		fprintf(file, "],\"lines\":[");
	}

	/* lines at each part's middle, from either way level */
	for (index = 0; index < shape_count; index++)
	{
		struct skate_hitbox_shape const *shape = &shapes[index];

		for (axis = 0; axis < 2; axis++)
		{
			real_point3d start, drawn_point, hit_point;
			real_vector3d vector;
			short drawn_node, hit_node;

			start.x = shape->middle.x - axes[axis].i * SKATE_HITBOX_REACH;
			start.y = shape->middle.y - axes[axis].j * SKATE_HITBOX_REACH;
			start.z = shape->middle.z;
			vector.i = axes[axis].i * 2.f * SKATE_HITBOX_REACH;
			vector.j = axes[axis].j * 2.f * SKATE_HITBOX_REACH;
			vector.k = 0.f;
			skate_hitbox_swap(unit_index, drawn, count, saved, &saved_center, FALSE);
			drawn_node = skate_hitbox_line(unit_index, &start, &vector, &drawn_point);
			skate_hitbox_swap(unit_index, drawn, count, saved, &saved_center, TRUE);
			hit_node = skate_hitbox_line(unit_index, &start, &vector, &hit_point);
			if (drawn_node == NONE)
				continue;
			part = shape->part;
			lines[part]++;
			if (drawn_node == shape->node_index)
				on_part[part]++;
			if (hit_node == drawn_node)
				same[part]++;
			else if (hit_node != NONE)
				other[part]++;
			else
				missed[part]++;
			if (dump)
			{
				fprintf(file, "%s{\"part\":\"%s\",\"aim\":%d,\"start\":", index || axis ? "," : "",
					skate_hitbox_part_names[part], shape->node_index);
				skate_hitbox_dump_point(file, &start);
				fprintf(file, ",\"vector\":[%.4f,%.4f,%.4f],\"drawn\":%d,\"hit\":%d}", vector.i, vector.j, vector.k,
					drawn_node, hit_node);
			}
		}
	}
	/* and lines just past the drawn body: over its head, and either side of
	it, from either way level */
	for (axis = 0; axis < 2; axis++)
	{
		real_vector3d const *along = &axes[axis];
		real_vector3d const *across = &axes[axis ^ 1];
		real side_most = -1.e9f, side_least = 1.e9f;
		short which;

		for (index = 0; index < shape_count; index++)
		{
			short corner;

			for (corner = 0; corner < shapes[index].corner_count; corner++)
			{
				real_point3d const *p = &shapes[index].drawn[corner];
				real side = (p->x - body.x) * across->i + (p->y - body.y) * across->j;

				side_most = MAX(side_most, side);
				side_least = MIN(side_least, side);
			}
		}
		for (which = 0; which < 3; which++)
		{
			real_point3d start;
			real_vector3d vector;
			real side = which == 1 ? side_most + SKATE_HITBOX_CLEARANCE :
				which == 2 ? side_least - SKATE_HITBOX_CLEARANCE : 0.f;
			short drawn_node, hit_node;

			start.x = body.x + across->i * side - along->i * SKATE_HITBOX_REACH;
			start.y = body.y + across->j * side - along->j * SKATE_HITBOX_REACH;
			start.z = which ? body.z : highest.z + SKATE_HITBOX_CLEARANCE;
			vector.i = along->i * 2.f * SKATE_HITBOX_REACH;
			vector.j = along->j * 2.f * SKATE_HITBOX_REACH;
			vector.k = 0.f;
			skate_hitbox_swap(unit_index, drawn, count, saved, &saved_center, FALSE);
			drawn_node = skate_hitbox_line(unit_index, &start, &vector, NULL);
			skate_hitbox_swap(unit_index, drawn, count, saved, &saved_center, TRUE);
			hit_node = skate_hitbox_line(unit_index, &start, &vector, NULL);
			outside++;
			outside_drawn += drawn_node != NONE;
			outside_hit += hit_node != NONE;
			if (dump)
			{
				fprintf(file, ",{\"part\":\"outside\",\"aim\":-1,\"start\":");
				skate_hitbox_dump_point(file, &start);
				fprintf(file, ",\"vector\":[%.4f,%.4f,%.4f],\"drawn\":%d,\"hit\":%d}", vector.i, vector.j, vector.k,
					drawn_node, hit_node);
			}
		}
	}
	if (dump)
	{
		fprintf(file, "]}\n");
		fflush(file);
	}
	platform_log("skate hitbox: %s skater, %s objects, %s: shapes off by %.3f world units at most (head %.3f, torso "
		"%.3f, arms %.3f, legs %.3f), the drawn body %.3f high, the bounding sphere %.3f off, the last drawn %.3f off; lines at the head "
		"%d (%d on it drawn): %d alike, %d elsewhere, %d missed; torso %d (%d): %d, %d, %d; arms %d (%d): %d, %d, "
		"%d; legs %d (%d): %d, %d, %d; past the body %d: %d hit drawn, %d hit; the drawn shapes %.3f past the bounding sphere", who,
		after_objects ? "after" : "before", state[0] ? state : "-", most, parts_off[0], parts_off[1], parts_off[2],
		parts_off[3], highest.z - lowest.z, (real)sqrt(distance_squared3d(&drawn_center,
		&object->object.bounding_sphere_center)), lag,
		lines[0], on_part[0], same[0], other[0], missed[0], lines[1], on_part[1], same[1], other[1], missed[1],
		lines[2], on_part[2], same[2], other[2], missed[2], lines[3], on_part[3], same[3], other[3], missed[3],
		outside, outside_drawn, outside_hit, unbounded);
}

void skate_hitbox_test(short when)
{
	struct object_iterator iterator;
	struct object_datum *object;

	if (!skate_hitbox_globals.read)
	{
		char const *path = config_string("debug.skate_hitbox_dump");

		skate_hitbox_globals.read = TRUE;
		skate_hitbox_globals.enabled = config_boolean("debug.skate_hitbox_test") != 0;
		if (skate_hitbox_globals.enabled && path && path[0])
		{
			skate_hitbox_globals.dump = fopen(path, "w");
			if (!skate_hitbox_globals.dump)
				platform_log("skate hitbox: cannot write %s", path);
		}
	}
	if (!skate_hitbox_globals.enabled || !game_in_progress() ||
		(when != _skate_hitbox_drawn && game_time_get() % SKATE_HITBOX_TICKS != 0))
	{
		return;
	}
	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		char const *who = iterator.index == skate_local_skater_unit() ? "this machine's" :
			skate_host_skater(iterator.index) ? "this machine's bot's" :
			skate_network_unit_skating(iterator.index) ? "another machine's" : NULL;

		if (!who)
			continue;
		if (when == _skate_hitbox_drawn)
			skate_hitbox_keep_drawn(iterator.index);
		else
			skate_hitbox_check(iterator.index, who, when == _skate_hitbox_after_objects);
	}
}
