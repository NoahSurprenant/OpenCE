/* skate.c: Skate 3 mode (skate.h, port/skate/README.md)

Loading happens before the player wants to skate: the skate session (the
skater, its animation banks, state graphs and physics, which can take a long
while) is made in the background at startup, and each structure BSP's
collision goes to the engine as triangles when it loads, to be built in the
background too. Getting on a board is then immediate; one pressed for while
something is still loading gets on when it is ready. While skating, a
tick sends the pad to the engine and takes back the skater: the biped is put
where the board is, its own movement is skipped (bipeds.c), and its nodes are
posed as the skater's bones. */

#include "cseries.h"
#include "math/real_math.h"
#include "game/players.h"
#include "main/console.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "physics/bsp3d.h"
#include "physics/collision_bsp.h"
#include "physics/collision_bsp_definitions.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "units/units.h"

#include "skate.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef HALO_SKATE

#include "../../skate/include/halo_skate.h"

/* the platform layer's (sdl_platform.c, xinput_sdl.c) */
int halo_skate_platform_toggle_pressed(void);
void halo_skate_platform_pad(struct halo_skate_pad *pad);
/* the platform layer's log (halo.log on Windows, stderr on Linux) */
void platform_log(const char *format, ...);

#define SKATE_TWO_SIDED_FLAG 0x01
#define SKATE_MAXIMUM_NODES 64
/* how far below the biped its spawn looks for the ground */
#define SKATE_GROUND_PROBE 1.f
#define SKATE_THUMB_CLICKS 0x00C0

static struct
{
	/* the startup preload found the assets folder: maps are loaded as they
	come; without it nothing loads until J (which says what is missing) */
	boolean preloaded;
	boolean skating;
	boolean activate_when_ready;
	long unit_index;
	long loaded_scenario_index;
	short loaded_structure_bsp_index;
	long skeleton_definition_index;
	unsigned short previous_buttons;
	/* the biped's origin above the board's wheels */
	real height;
	real yaw;
	struct halo_skate_frame frame;
} skate_globals = { FALSE, FALSE, FALSE, NONE, NONE, NONE, NONE, 0, 0.f, 0.f };

static const char *skate_assets(void)
{
	const char *assets = getenv("HALO_SKATE_ASSETS");

	return assets && *assets ? assets : "skate-data/assets";
}

/* the BSP's surfaces as triangles wound counterclockwise seen from in front
of their planes (two-sided surfaces both ways), count in *count */
static float *skate_collision_triangles(struct collision_bsp *bsp, long *count)
{
	long capacity = 0;
	long surface_index;
	float *triangles;

	*count = 0;
	for (surface_index = 0; surface_index < bsp->surfaces.count; surface_index++)
		capacity += 2 * (MAXIMUM_VERTICES_PER_COLLISION_SURFACE - 2);
	triangles = (float *)malloc((size_t)(capacity ? capacity : 1) * 9 * sizeof(float));
	if (!triangles)
		return NULL;

	for (surface_index = 0; surface_index < bsp->surfaces.count; surface_index++)
	{
		struct collision_surface *surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, surface_index,
			struct collision_surface);
		real_point3d points[MAXIMUM_VERTICES_PER_COLLISION_SURFACE];
		real_plane3d plane;
		short point_count = collision_surface_polygon(bsp, surface_index, points);
		short index;

		if (point_count < 3 || point_count > MAXIMUM_VERTICES_PER_COLLISION_SURFACE)
			continue;
		bsp3d_get_plane_from_designator(&bsp->bsp3d, surface->plane_designator, &plane);
		for (index = 1; index + 1 < point_count; index++)
		{
			real_point3d const *a = &points[0];
			real_point3d const *b = &points[index];
			real_point3d const *c = &points[index + 1];
			real_vector3d ab, ac, normal;
			boolean flip;
			int side;

			ab.i = b->x - a->x; ab.j = b->y - a->y; ab.k = b->z - a->z;
			ac.i = c->x - a->x; ac.j = c->y - a->y; ac.k = c->z - a->z;
			normal.i = ab.j * ac.k - ab.k * ac.j;
			normal.j = ab.k * ac.i - ab.i * ac.k;
			normal.k = ab.i * ac.j - ab.j * ac.i;
			flip = normal.i * plane.n.i + normal.j * plane.n.j + normal.k * plane.n.k < 0.f;
			for (side = 0; side < ((surface->flags & SKATE_TWO_SIDED_FLAG) ? 2 : 1); side++)
			{
				float *out = triangles + *count * 9;
				real_point3d const *second = (flip != (side == 1)) ? c : b;
				real_point3d const *third = (flip != (side == 1)) ? b : c;

				out[0] = a->x; out[1] = a->y; out[2] = a->z;
				out[3] = second->x; out[4] = second->y; out[5] = second->z;
				out[6] = third->x; out[7] = third->y; out[8] = third->z;
				(*count)++;
			}
		}
	}
	return triangles;
}

static void skate_stop(const char *reason);

static boolean skate_map_loaded(void)
{
	return skate_globals.loaded_scenario_index == global_scenario_index &&
		skate_globals.loaded_structure_bsp_index == global_structure_bsp_index;
}

/* sends the BSP's collision to be built in the background; the engine copies
the triangles, so this never waits for the build */
static boolean skate_load_map(void)
{
	struct collision_bsp *bsp = global_collision_bsp_get();
	long count;
	float *triangles;
	boolean sent = FALSE;

	if (!bsp)
		return FALSE;
	triangles = skate_collision_triangles(bsp, &count);
	if (!triangles)
		return FALSE;
	if (halo_skate_load(skate_assets(), triangles, (int)count) == 0)
	{
		skate_globals.loaded_scenario_index = global_scenario_index;
		skate_globals.loaded_structure_bsp_index = global_structure_bsp_index;
		sent = TRUE;
	}
	free(triangles);
	return sent;
}

static void skate_log(const char *line)
{
	platform_log("%s", line);
}

void skate_initialize(void)
{
	/* the engine's lines (its load timings above all) go to the port's log:
	a Windows release build has no console for its stderr */
	halo_skate_set_log(skate_log);
	/* (the engine says when there is no assets folder; nothing more happens
	until J) */
	skate_globals.preloaded = halo_skate_preload(skate_assets()) == 0;
}

void skate_structure_bsp_changed(void)
{
	struct scenario *scenario = global_scenario_get();
	/* (a J still waiting for its load waits for this bsp's instead) */
	boolean waiting = skate_globals.activate_when_ready && !skate_globals.skating;

	/* the board is left in the old bsp's collision */
	skate_stop("the level changed");
	/* (and this bsp is not the one loaded, even where a new map's scenario
	tag has the old one's index) */
	skate_globals.loaded_scenario_index = NONE;
	skate_globals.loaded_structure_bsp_index = NONE;
	/* nobody skates in the main menu; a failed preload is retried on J */
	if (!skate_globals.preloaded || !scenario || scenario->type == _scenario_type_main_menu ||
		halo_skate_state() == -1)
	{
		return;
	}
	if (skate_load_map())
		skate_globals.activate_when_ready = waiting;
}

static void skate_describe_skeleton(long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct object_definition *definition = object_definition_get(unit->definition_index);
	struct model *model;
	char names[SKATE_MAXIMUM_NODES][32];
	short parents[SKATE_MAXIMUM_NODES];
	float inverses[SKATE_MAXIMUM_NODES][13];
	long count;
	long index;

	if (skate_globals.skeleton_definition_index == unit->definition_index ||
		definition->object.model.index == NONE)
	{
		return;
	}
	model = model_definition_get(definition->object.model.index);
	count = model->nodes.count < SKATE_MAXIMUM_NODES ? model->nodes.count : SKATE_MAXIMUM_NODES;
	for (index = 0; index < count; index++)
	{
		struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);
		real_matrix4x3 const *inverse = &node->runtime_default_inverse_matrix;

		memset(names[index], 0, sizeof(names[index]));
		strncpy(names[index], node->name, sizeof(names[index]) - 1);
		parents[index] = node->parent_node_index;
		inverses[index][0] = inverse->scale;
		memcpy(&inverses[index][1], inverse->n, sizeof(float) * 12);
	}
	console_printf(FALSE, "skate: %d of %ld nodes follow the skater",
		halo_skate_set_skeleton((int)count, &names[0][0], parents, &inverses[0][0]), count);
	skate_globals.skeleton_definition_index = unit->definition_index;
}

static boolean skate_unit_can_skate(long unit_index)
{
	struct object_datum *object = unit_index != NONE ? object_try_and_get(unit_index) : NULL;

	return object &&
		object->object.type == _object_type_biped &&
		object->object.parent_object_index == NONE &&
		!(object->object.damage_flags & FLAG(_object_dead_bit));
}

static void skate_apply_frame(long unit_index)
{
	struct object_datum *object = object_get(unit_index);
	real_point3d position;
	real_vector3d forward, up;

	position.x = skate_globals.frame.position[0];
	position.y = skate_globals.frame.position[1];
	position.z = skate_globals.frame.position[2] + skate_globals.height;
	/* the biped stands upright, facing the board's heading */
	forward.i = skate_globals.frame.forward[0];
	forward.j = skate_globals.frame.forward[1];
	forward.k = 0.f;
	if (normalize3d(&forward) == 0.f)
		forward = object->object.forward;
	up = *global_up3d;
	/* (the player's desired yaw must be within 0 to 2 pi: player_control.c) */
	skate_globals.yaw = (real)atan2(forward.j, forward.i);
	if (skate_globals.yaw < 0.f)
		skate_globals.yaw += 2.f * _pi;

	object_set_position(unit_index, &position, &forward, &up);
	object->object.translational_velocity.i = skate_globals.frame.velocity[0] / TICKS_PER_SECOND;
	object->object.translational_velocity.j = skate_globals.frame.velocity[1] / TICKS_PER_SECOND;
	object->object.translational_velocity.k = skate_globals.frame.velocity[2] / TICKS_PER_SECOND;
}

static void skate_stop(const char *reason)
{
	if (skate_globals.skating)
	{
		halo_skate_suspend();
		if (reason)
			console_printf(FALSE, "skate: off (%s)", reason);
	}
	skate_globals.skating = FALSE;
	skate_globals.activate_when_ready = FALSE;
	skate_globals.unit_index = NONE;
}

static void skate_start(long unit_index)
{
	struct object_datum *object = object_get(unit_index);
	struct collision_result collision;
	real_point3d ground = object->object.position;
	real_vector3d down;

	down.i = 0.f;
	down.j = 0.f;
	down.k = -SKATE_GROUND_PROBE;
	if (collision_test_vector(_collision_test_environment_flags, &object->object.position, &down,
		unit_index, &collision))
	{
		ground = collision.point;
	}
	skate_globals.height = object->object.position.z - ground.z;
	skate_describe_skeleton(unit_index);
	if (halo_skate_activate(&ground.x, (real)atan2(object->object.forward.j, object->object.forward.i),
		&skate_globals.frame) != 0)
	{
		console_printf(FALSE, "skate: could not get on the board: %s", halo_skate_error());
		return;
	}
	skate_globals.skating = TRUE;
	skate_globals.unit_index = unit_index;
	skate_apply_frame(unit_index);
	console_printf(FALSE, "skate: on");
}

void skate_update_before_objects(void)
{
	long unit_index = player_control_get_unit_index(0);
	struct halo_skate_pad pad;
	boolean toggle;
	int state;

	memset(&pad, 0, sizeof(pad));
	halo_skate_platform_pad(&pad);
	toggle = halo_skate_platform_toggle_pressed() ||
		((pad.buttons & SKATE_THUMB_CLICKS) == SKATE_THUMB_CLICKS &&
		(skate_globals.previous_buttons & SKATE_THUMB_CLICKS) != SKATE_THUMB_CLICKS);
	skate_globals.previous_buttons = pad.buttons;

	if (skate_globals.skating && (unit_index != skate_globals.unit_index ||
		!skate_unit_can_skate(unit_index) || !skate_map_loaded()))
	{
		skate_stop("the biped left the board");
	}
	if (toggle)
	{
		if (skate_globals.skating || skate_globals.activate_when_ready)
		{
			skate_stop("J");
		}
		else if (skate_unit_can_skate(unit_index))
		{
			/* (normally loaded already: at startup and as the bsp loaded.
			Without the preload, or after a failure, it loads now) */
			if (!skate_map_loaded() || halo_skate_state() == -1)
			{
				if (skate_load_map())
					console_printf(FALSE, "skate: loading");
			}
			skate_globals.activate_when_ready = TRUE;
			if (halo_skate_state() == 1)
			{
				console_printf(FALSE, halo_skate_preloading() ?
					"skate: still loading the skater, on the board when it is ready" :
					"skate: still loading the level, on the board when it is ready");
			}
		}
	}

	state = halo_skate_state();
	if (state == -1 && (skate_globals.skating || skate_globals.activate_when_ready))
	{
		console_printf(FALSE, "skate: %s", halo_skate_error());
		skate_globals.skating = FALSE;
		skate_globals.activate_when_ready = FALSE;
		skate_globals.loaded_scenario_index = NONE;
		return;
	}
	if (skate_globals.activate_when_ready && state == 2)
	{
		skate_globals.activate_when_ready = FALSE;
		if (skate_unit_can_skate(unit_index))
			skate_start(unit_index);
		return;
	}
	if (skate_globals.skating && halo_skate_step(&pad, 1.f / TICKS_PER_SECOND, &skate_globals.frame) >= 0)
		skate_apply_frame(skate_globals.unit_index);
}

void skate_update_after_objects(void)
{
	real_matrix4x3 matrices[SKATE_MAXIMUM_NODES];
	struct object_datum *object;
	real_matrix4x3 *node_matrices;
	int node_count;
	int written;

	if (!skate_globals.skating || !skate_unit_can_skate(skate_globals.unit_index))
		return;
	object = object_get(skate_globals.unit_index);
	node_count = object->object.node_matrices.size / (int)sizeof(real_matrix4x3);
	if (node_count <= 0 || node_count > SKATE_MAXIMUM_NODES)
		return;
	written = halo_skate_pose_nodes(&matrices[0].scale, node_count);
	if (written != node_count)
		return;
	node_matrices = (real_matrix4x3 *)object_header_block_get(skate_globals.unit_index,
		&object->object.node_matrices);
	memcpy(node_matrices, matrices, sizeof(real_matrix4x3) * node_count);
}

boolean skate_unit_is_skating(long unit_index)
{
	return skate_globals.skating && unit_index != NONE && unit_index == skate_globals.unit_index;
}

boolean skate_local_player_skating(short local_player_index, real *yaw)
{
	if (local_player_index != 0 || !skate_globals.skating)
		return FALSE;
	if (yaw)
		*yaw = skate_globals.yaw;
	return TRUE;
}

#else

void skate_initialize(void)
{
}

void skate_structure_bsp_changed(void)
{
}

void skate_update_before_objects(void)
{
}

void skate_update_after_objects(void)
{
}

boolean skate_unit_is_skating(long unit_index)
{
	return FALSE;
}

boolean skate_local_player_skating(short local_player_index, real *yaw)
{
	return FALSE;
}

#endif
