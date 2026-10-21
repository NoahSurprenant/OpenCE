/* bots_nav_game.c: the bots' walking graph of the map being played
(bots_nav.c builds it; bots_internal.h)

On the host, once a game is being played and it has bots (or
debug.bot_nav_check asks), the graph of its map: read from the cache, else
built from the level and written to the cache. The cache is in the save
root (z:\, OPFS in the browser), a file for each map and version of it, so
that a map's graph is built once per machine; a map's data is the players'
own, and nothing made from it is in the repository.

What the graph is built from:
- the structure BSP's collision surfaces as triangles, and the static
  scenery's collision models placed in the level (teleporter pads, crates,
  rocks: Wizard's and Prisoner's floors are partly scenery), as Skate 3 mode
  gathers the level's collision (skate.c's skate_collision_triangles and
  skate_add_scenery, written again here: the two modes stay apart);
- the spawns (the scenario's player starting locations), whose part of the
  level is kept;
- the teleporters (netgame flags 6, an entrance, and 7, an exit, paired by
  their team index), and a map's own extra links (bots_nav_overrides);
- the game's collision test, which a drop or a jump must pass.

debug.bot_nav_check logs, for each map, how much of it the graph reaches:
the spawns and the multiplayer weapons that are both reachable from the
spawns' part and reach it back. It then ends the game, so that a dedicated
server goes through its map list a map at a time
(tools/test_bots.py's test_bots_nav_on_every_map). */

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "main/main.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "objects/object_types.h"
#include "physics/bsp3d.h"
#include "physics/collision_bsp.h"
#include "physics/collision_bsp_definitions.h"
#include "physics/collision_model_definitions.h"
#include "physics/collision_models.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "cache/cache_files.h"
#include "cseries/cseries_windows.h"
#include "tag_files/tag_files.h"

#include "bots_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xtl.h>

/* the platform layer's (port_config.c, the port's log) */
int config_boolean(const char *name);
void platform_log(const char *format, ...);

/* a collision surface seen from both sides (collision_bsp_definitions.h's
surface flags; skate.c's SKATE_TWO_SIDED_FLAG) */
#define BOTS_NAV_TWO_SIDED_FLAG 0x01
/* ... and one a biped climbs (bipeds.c's _collision_surface_climbable_bit):
Boarding Action's ladders */
#define BOTS_NAV_CLIMBABLE_FLAG 0x04
#define BOTS_NAV_MAXIMUM_LADDERS 128
/* the netgame flags of a teleporter's entrance and exit */
#define BOTS_NAV_TELEPORTER_ENTRANCE 6
#define BOTS_NAV_TELEPORTER_EXIT 7
#define BOTS_NAV_MAXIMUM_LINKS 64

static struct
{
	/* the map the graph is of (none: no graph), and the graph */
	boolean built;
	long scenario_index;
	short structure_bsp_index;
	unsigned long key;
	char map[64];
	struct bots_nav nav;
	/* debug.bot_nav_check's game ended for this map */
	boolean checked;
} bots_nav_game;

/* the graph the map's vehicles drive (bots_nav_vehicle_parameters), made
with the walking graph when needed, on a map that places vehicles (none: no
graph) */
static struct
{
	boolean built;
	long scenario_index;
	short structure_bsp_index;
	struct bots_nav nav;
} bots_nav_drive;

/* ---------- the level's triangles */

struct bots_nav_triangles
{
	float *points;
	long count;
	long capacity;
};

static boolean bots_nav_triangles_reserve(
	struct bots_nav_triangles *triangles,
	long count)
{
	float *grown;
	long capacity;

	if (count <= triangles->capacity)
		return TRUE;
	capacity = triangles->capacity ? triangles->capacity : 4096;
	while (capacity < count)
		capacity *= 2;
	grown = (float *)realloc(triangles->points, (size_t)capacity * 9 * sizeof(float));
	if (!grown)
		return FALSE;
	triangles->points = grown;
	triangles->capacity = capacity;
	return TRUE;
}

/* a BSP's surfaces as triangles wound counterclockwise seen from in front
of their planes (two-sided ones both ways), each point placed by the
matrix (none: as they are) */
static boolean bots_nav_add_bsp(
	struct bots_nav_triangles *triangles,
	struct collision_bsp *bsp,
	real_matrix4x3 const *matrix)
{
	long surface_index;

	for (surface_index = 0; surface_index < bsp->surfaces.count; surface_index++)
	{
		struct collision_surface *surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, surface_index, struct collision_surface);
		real_point3d points[MAXIMUM_VERTICES_PER_COLLISION_SURFACE];
		real_plane3d plane;
		short point_count = collision_surface_polygon(bsp, surface_index, points);
		short index;

		if (point_count < 3 || point_count > MAXIMUM_VERTICES_PER_COLLISION_SURFACE)
			continue;
		bsp3d_get_plane_from_designator(&bsp->bsp3d, surface->plane_designator, &plane);
		if (!bots_nav_triangles_reserve(triangles, triangles->count + 2 * (point_count - 2)))
			return FALSE;
		for (index = 1; index + 1 < point_count; index++)
		{
			real_point3d const *corners[3];
			real_vector3d ab, ac;
			boolean flip;
			int side;

			corners[0] = &points[0];
			corners[1] = &points[index];
			corners[2] = &points[index + 1];
			ab.i = corners[1]->x - corners[0]->x; ab.j = corners[1]->y - corners[0]->y; ab.k = corners[1]->z - corners[0]->z;
			ac.i = corners[2]->x - corners[0]->x; ac.j = corners[2]->y - corners[0]->y; ac.k = corners[2]->z - corners[0]->z;
			flip = (ab.j * ac.k - ab.k * ac.j) * plane.n.i + (ab.k * ac.i - ab.i * ac.k) * plane.n.j +
				(ab.i * ac.j - ab.j * ac.i) * plane.n.k < 0.0f;
			for (side = 0; side < ((surface->flags & BOTS_NAV_TWO_SIDED_FLAG) ? 2 : 1); side++)
			{
				float *out = triangles->points + triangles->count * 9;
				int order[3];
				int corner;

				order[0] = 0;
				order[1] = flip != (side == 1) ? 2 : 1;
				order[2] = flip != (side == 1) ? 1 : 2;
				for (corner = 0; corner < 3; corner++)
				{
					real_point3d placed = *corners[order[corner]];

					if (matrix)
						matrix4x3_transform_point(matrix, corners[order[corner]], &placed);
					out[corner * 3] = placed.x;
					out[corner * 3 + 1] = placed.y;
					out[corner * 3 + 2] = placed.z;
				}
				triangles->count++;
			}
		}
	}
	return TRUE;
}

/* the static scenery's collision, each node's BSP (the permutation the
object shows) placed as the node is */
static boolean bots_nav_add_scenery(
	struct bots_nav_triangles *triangles,
	long *pieces)
{
	struct object_iterator iterator;
	struct object_datum *scenery;

	*pieces = 0;
	object_iterator_new(&iterator, _object_mask_scenery, 0);
	while ((scenery = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		struct collision_model_instance instance;
		short node_index;

		if (scenery->object.parent_object_index != NONE ||
			!collision_model_instance_new(&instance, iterator.index) || !instance.matrices)
		{
			continue;
		}
		for (node_index = 0; node_index < instance.node_count; node_index++)
		{
			struct collision_node const *node = TAG_BLOCK_GET_ELEMENT(&instance.model->nodes, node_index,
				struct collision_node);
			short permutation;

			if (node->region_index < 0 || node->region_index >= MAXIMUM_REGIONS_PER_OBJECT || node->bsps.count <= 0)
				continue;
			permutation = instance.region_permutation_indices[node->region_index];
			permutation = PIN(permutation, 0, (short)(node->bsps.count - 1));
			if (!bots_nav_add_bsp(triangles, TAG_BLOCK_GET_ELEMENT(&node->bsps, permutation, struct collision_bsp),
				&instance.matrices[node_index]))
			{
				return FALSE;
			}
		}
		(*pieces)++;
	}
	return TRUE;
}

/* the BSP's ladders: its upright climbable surfaces, each one's bottom and
top edges' middles and the way it faces; their count */
static long bots_nav_ladders(
	struct collision_bsp *bsp,
	struct bots_nav_ladder *ladders,
	long maximum)
{
	long count = 0;
	long surface_index;

	for (surface_index = 0; surface_index < bsp->surfaces.count && count < maximum; surface_index++)
	{
		struct collision_surface *surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, surface_index, struct collision_surface);
		real_point3d points[MAXIMUM_VERTICES_PER_COLLISION_SURFACE];
		real_plane3d plane;
		short point_count;
		real low_z = 1.0e30f, high_z = -1.0e30f, sum_x = 0.0f, sum_y = 0.0f;
		real length;
		short index;

		if (!(surface->flags & BOTS_NAV_CLIMBABLE_FLAG))
			continue;
		point_count = collision_surface_polygon(bsp, surface_index, points);
		if (point_count < 3 || point_count > MAXIMUM_VERTICES_PER_COLLISION_SURFACE)
			continue;
		bsp3d_get_plane_from_designator(&bsp->bsp3d, surface->plane_designator, &plane);
		length = (real)sqrt(plane.n.i * plane.n.i + plane.n.j * plane.n.j);
		/* (upright ones: a climbable floor is a floor) */
		if (fabs(plane.n.k) > 0.3f || length < 0.001f)
			continue;
		for (index = 0; index < point_count; index++)
		{
			if (points[index].z < low_z) low_z = points[index].z;
			if (points[index].z > high_z) high_z = points[index].z;
			sum_x += points[index].x;
			sum_y += points[index].y;
		}
		ladders[count].bottom.x = ladders[count].top.x = sum_x / (real)point_count;
		ladders[count].bottom.y = ladders[count].top.y = sum_y / (real)point_count;
		ladders[count].bottom.z = low_z;
		ladders[count].top.z = high_z;
		ladders[count].normal_x = plane.n.i / length;
		ladders[count].normal_y = plane.n.j / length;
		count++;
	}
	/* (a ladder is often several surfaces one above another: joined) */
	{
		boolean joined = TRUE;

		while (joined)
		{
			long first, second;

			joined = FALSE;
			for (first = 0; first < count && !joined; first++)
			{
				for (second = first + 1; second < count && !joined; second++)
				{
					struct bots_nav_ladder *a = &ladders[first];
					struct bots_nav_ladder const *b = &ladders[second];

					if (a->normal_x * b->normal_x + a->normal_y * b->normal_y > 0.99f &&
						fabs(a->bottom.x - b->bottom.x) < 0.3f && fabs(a->bottom.y - b->bottom.y) < 0.3f &&
						b->bottom.z <= a->top.z + 0.1f && a->bottom.z <= b->top.z + 0.1f)
					{
						a->bottom.z = MIN(a->bottom.z, b->bottom.z);
						a->top.z = MAX(a->top.z, b->top.z);
						ladders[second] = ladders[--count];
						joined = TRUE;
					}
				}
			}
		}
	}
	/* (and too short a one is a ledge's trim) */
	{
		long kept = 0, index;

		for (index = 0; index < count; index++)
		{
			if (ladders[index].top.z - ladders[index].bottom.z >= 0.8f)
				ladders[kept++] = ladders[index];
		}
		count = kept;
	}
	return count;
}

/* ---------- a map's own links */

/* links the generator does not find on a stock map, checked by hand
(debug.bot_nav_check): coordinates only, nothing of the map's data */
struct bots_nav_override
{
	char const *map;
	struct bots_nav_link link;
};

static struct bots_nav_override const bots_nav_overrides[] =
{
	/* Damnation: the upper floor (6.7) meets the main part's at a corner,
	across a little hole: a hop each way */
	{ "damnation", { { 4.45f, -7.05f, 6.70f }, { 5.05f, -6.55f, 6.70f }, _bots_nav_jump } },
	{ "damnation", { { 5.05f, -6.55f, 6.70f }, { 4.45f, -7.05f, 6.70f }, _bots_nav_jump } },
	/* Wizard: the middle's raised disc (-2.3), across the moat from the
	walkway's four corners (-2.75): a running jump */
	{ "wizard", { { 3.10f, -3.10f, -2.75f }, { 1.60f, -1.60f, -2.30f }, _bots_nav_jump } },
	{ "wizard", { { 3.10f, 3.10f, -2.75f }, { 1.60f, 1.60f, -2.30f }, _bots_nav_jump } },
	{ "wizard", { { -3.10f, 3.10f, -2.75f }, { -1.60f, 1.60f, -2.30f }, _bots_nav_jump } },
	{ "wizard", { { -3.10f, -3.10f, -2.75f }, { -1.60f, -1.60f, -2.30f }, _bots_nav_jump } },
	/* Beaver Creek: onto the rock by the blue base (the sniper rifle's) */
	{ "beavercreek", { { 14.40f, 20.45f, 2.90f }, { 14.94f, 18.85f, 3.63f }, _bots_nav_jump } },
};

/* ---------- the graph */

/* whether a biped moves straight between the points: nothing of the level
or its scenery in the way */
static int bots_nav_clear(
	void *context,
	struct bots_nav_point const *from,
	struct bots_nav_point const *to)
{
	struct collision_result collision;
	real_point3d point;
	real_vector3d vector;

	(void)context;
	point.x = from->x;
	point.y = from->y;
	point.z = from->z;
	vector.i = to->x - from->x;
	vector.j = to->y - from->y;
	vector.k = to->z - from->z;
	return !collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_back_facing_surfaces_bit) | FLAG(_collision_test_structure_bit) |
		FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) |
		FLAG(_collision_test_objects_machines_bit), &point, &vector, NONE, &collision);
}

static void bots_nav_point_from(
	struct bots_nav_point *point,
	real_point3d const *position)
{
	point->x = position->x;
	point->y = position->y;
	point->z = position->z;
}

/* the map's name (the last part of its path) */
static void bots_nav_map_name(
	char *name,
	int size)
{
	char const *path = main_get_multiplayer_map_name();
	char const *slash = path ? strrchr(path, '\\') : NULL;

	snprintf(name, (size_t)size, "%s", slash ? slash + 1 : path ? path : "");
}

/* the cache of a graph of the map: the walking graph's (kind "nav"), the
drive graph's ("drive") */
static void bots_nav_cache_path(
	char *path,
	int size,
	char const *kind)
{
	snprintf(path, (size_t)size, "z:\\bots_%s_%s_%08lx.nav", kind, bots_nav_game.map, bots_nav_game.key);
}

static boolean bots_nav_cache_read(
	struct bots_nav *nav,
	struct bots_nav_parameters const *parameters,
	char const *kind)
{
	char path[160];
	FILE *file;
	unsigned char *bytes = NULL;
	long size = 0;
	boolean loaded = FALSE;

	bots_nav_cache_path(path, (int)sizeof(path), kind);
	file = fopen(path, "rb");
	if (!file)
		return FALSE;
	if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0 && size < 64L * 1024L * 1024L &&
		fseek(file, 0, SEEK_SET) == 0 && (bytes = (unsigned char *)malloc((size_t)size)) != NULL &&
		fread(bytes, 1, (size_t)size, file) == (size_t)size)
	{
		loaded = bots_nav_load(nav, bytes, size, bots_nav_game.key, parameters) != 0;
	}
	fclose(file);
	free(bytes);
	return loaded;
}

static void bots_nav_cache_write(
	struct bots_nav const *nav,
	char const *kind)
{
	char path[160];
	unsigned char *bytes;
	long size;
	FILE *file;

	bots_nav_cache_path(path, (int)sizeof(path), kind);
	bytes = bots_nav_save(nav, bots_nav_game.key, &size);
	if (!bytes)
		return;
	file = fopen(path, "wb");
	if (file)
	{
		if (fwrite(bytes, 1, (size_t)size, file) != (size_t)size)
			platform_log("bots: could not write the graph's cache %s", path);
		fclose(file);
	}
	bots_nav_free_bytes(bytes);
}

/* how many of the points the graph serves: a spawn whose node reaches the
spawns' main part (a bot spawned there joins the game: a roof whose
teleporter is its only way down is one), a weapon whose node the main part
reaches (a bot goes and takes it); with debug.bot_nav_check, each it does
not, said */
static long bots_nav_reached(
	struct bots_nav const *nav,
	struct bots_nav_point const *points,
	long count,
	char const *kind,
	unsigned char needed)
{
	boolean check = config_boolean("debug.bot_nav_check") != 0;
	long reached = 0;
	long index;

	for (index = 0; index < count; index++)
	{
		long node = bots_nav_locate(nav, points[index].x, points[index].y, points[index].z);

		if (node >= 0 && (nav->nodes[node].reach & needed) == needed)
		{
			reached++;
		}
		else if (check)
		{
			struct bots_nav_point const *at = node >= 0 ? &nav->nodes[node].point : NULL;
			struct collision_result collision;
			real_point3d from;
			real_vector3d up = { 0.0f, 0.0f, 3.0f };
			boolean above;
			char what[96];

			/* (what is over it, within a biped's height and more: a low
			ceiling, a piece of scenery) */
			from.x = points[index].x;
			from.y = points[index].y;
			from.z = points[index].z + 0.05f;
			above = collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) |
				FLAG(_collision_test_back_facing_surfaces_bit) | FLAG(_collision_test_structure_bit) |
				FLAG(_collision_test_objects_bit) | _collision_test_objects_all_types_flags, &from, &up, NONE, &collision);
			snprintf(what, sizeof(what), "%s %.2f above %s", above ? "something" : "nothing",
				above ? collision.point.z - points[index].z : 0.0f,
				above && collision.type == _collision_result_object && object_try_and_get(collision.object_index) ?
				tag_get_name(object_get(collision.object_index)->definition_index) : above ? "(the level)" : "");
			platform_log("bots: nav check: %s %s %ld at (%.2f %.2f %.2f) not reached: %s (%.2f %.2f %.2f); %s",
				bots_nav_game.map, kind, index, points[index].x, points[index].y, points[index].z,
				!at ? "no node" : nav->nodes[node].reach == _bots_nav_reached_bit ? "its node is reached, not reaching back" :
				nav->nodes[node].reach == _bots_nav_reaches_bit ? "its node reaches the main part, not reached" : "node",
				at ? at->x : 0.0f, at ? at->y : 0.0f, at ? at->z : 0.0f, what);
		}
	}
	return reached;
}

/* the graph of the map being played, from the cache or built */
static void bots_nav_make(
	void)
{
	struct scenario *scenario = global_scenario_get();
	struct collision_bsp *bsp = global_collision_bsp_get();
	struct bots_nav_parameters parameters;
	struct bots_nav_input input;
	struct bots_nav_triangles triangles;
	struct bots_nav_point *seeds = NULL;
	struct bots_nav_point *weapons = NULL;
	struct bots_nav_link links[BOTS_NAV_MAXIMUM_LINKS];
	struct bots_nav_ladder ladders[BOTS_NAV_MAXIMUM_LADDERS];
	long ladder_count = 0;
	long seed_count = 0, weapon_count = 0, link_count = 0, pieces = 0;
	unsigned long started = system_milliseconds();
	boolean cached;
	long index;

	bots_nav_free(&bots_nav_game.nav);
	bots_nav_game.built = TRUE;
	bots_nav_game.scenario_index = global_scenario_index;
	bots_nav_game.structure_bsp_index = global_structure_bsp_index;
	bots_nav_map_name(bots_nav_game.map, (int)sizeof(bots_nav_game.map));
	bots_nav_game.key = cache_files_get_checksum() ^ ((unsigned long)global_structure_bsp_index * 0x9E3779B9UL);
	bots_nav_game.checked = FALSE;
	bots_nav_default_parameters(&parameters);
	if (!scenario || !bsp)
		return;

	/* (the spawns, the weapons, the teleporters and the map's own links) */
	seeds = (struct bots_nav_point *)malloc((size_t)(scenario->players.count + 1) * sizeof(*seeds));
	weapons = (struct bots_nav_point *)malloc((size_t)(scenario->netgame_equipment.count + 1) * sizeof(*weapons));
	if (!seeds || !weapons)
	{
		free(seeds);
		free(weapons);
		return;
	}
	for (index = 0; index < scenario->players.count; index++)
	{
		struct player_starting_location *start = TAG_BLOCK_GET_ELEMENT(&scenario->players, index,
			struct player_starting_location);

		bots_nav_point_from(&seeds[seed_count++], &start->position);
	}
	for (index = 0; index < scenario->netgame_equipment.count; index++)
	{
		struct scenario_netgame_equipment *equipment = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_equipment, index,
			struct scenario_netgame_equipment);

		bots_nav_point_from(&weapons[weapon_count++], &equipment->position);
	}
	for (index = 0; index < scenario->netgame_flags.count && link_count < BOTS_NAV_MAXIMUM_LINKS; index++)
	{
		struct scenario_netgame_flag *entrance = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, index,
			struct scenario_netgame_flag);
		long exit_index;

		if (entrance->type != BOTS_NAV_TELEPORTER_ENTRANCE)
			continue;
		/* (the first exit of its channel, as game_engine.c teleports to) */
		for (exit_index = 0; exit_index < scenario->netgame_flags.count; exit_index++)
		{
			struct scenario_netgame_flag *exit = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, exit_index,
				struct scenario_netgame_flag);

			if (exit->type == BOTS_NAV_TELEPORTER_EXIT && exit->team_index == entrance->team_index)
			{
				bots_nav_point_from(&links[link_count].from, &entrance->position);
				bots_nav_point_from(&links[link_count].to, &exit->position);
				links[link_count].type = _bots_nav_teleport;
				link_count++;
				break;
			}
		}
	}
	for (index = 0; index < (long)NUMBEROF(bots_nav_overrides) && link_count < BOTS_NAV_MAXIMUM_LINKS; index++)
	{
		if (bots_nav_overrides[index].map && !strcmp(bots_nav_overrides[index].map, bots_nav_game.map))
			links[link_count++] = bots_nav_overrides[index].link;
	}

	cached = bots_nav_cache_read(&bots_nav_game.nav, &parameters, "nav");
	if (!cached)
	{
		csmemset(&triangles, 0, sizeof(triangles));
		if (bots_nav_add_bsp(&triangles, bsp, NULL) && bots_nav_add_scenery(&triangles, &pieces))
		{
			csmemset(&input, 0, sizeof(input));
			input.triangles = triangles.points;
			input.triangle_count = triangles.count;
			input.seeds = seeds;
			input.seed_count = seed_count;
			input.links = links;
			input.link_count = link_count;
			ladder_count = bots_nav_ladders(bsp, ladders, BOTS_NAV_MAXIMUM_LADDERS);
			input.ladders = ladders;
			input.ladder_count = ladder_count;
			input.clear = bots_nav_clear;
			if (bots_nav_build(&input, &parameters, &bots_nav_game.nav))
				bots_nav_cache_write(&bots_nav_game.nav, "nav");
		}
		free(triangles.points);
	}
	if (!bots_nav_game.nav.nodes)
	{
		platform_log("bots: no walking graph for %s: the bots stand still", bots_nav_game.map);
	}
	else
	{
		struct bots_nav_statistics const *statistics = &bots_nav_game.nav.statistics;

		platform_log("bots: walking graph of %s %s in %lu ms: %ld nodes, %ld edges (%ld walks, %ld drops, %ld jumps, "
			"%ld teleporters, %ld ladders, %ld given; %ld refused), %ld triangles with %ld pieces of scenery; spawns %ld/%ld reach the main part, weapons "
			"%ld/%ld are reached from it",
			bots_nav_game.map, cached ? "read from the cache" : "built", system_milliseconds() - started,
			bots_nav_game.nav.node_count, bots_nav_game.nav.edge_count, statistics->walk_edges, statistics->drop_edges,
			statistics->jump_edges, statistics->teleport_edges, statistics->climb_edges, statistics->link_edges, statistics->rejected_links, statistics->triangles, pieces,
			bots_nav_reached(&bots_nav_game.nav, seeds, seed_count, "spawn", _bots_nav_reaches_bit), seed_count,
			bots_nav_reached(&bots_nav_game.nav, weapons, weapon_count, "weapon", _bots_nav_reached_bit),
			weapon_count);
		/* (each teleporter and given link, where its ends are on the graph) */
		if (config_boolean("debug.bot_nav_check"))
		{
			for (index = 0; index < link_count; index++)
			{
				long from = bots_nav_locate(&bots_nav_game.nav, links[index].from.x, links[index].from.y, links[index].from.z);
				long to = bots_nav_locate(&bots_nav_game.nav, links[index].to.x, links[index].to.y, links[index].to.z);

				platform_log("bots: nav check: %s link %ld (%s) from (%.2f %.2f %.2f) node %ld reach %d to (%.2f %.2f %.2f) node %ld reach %d",
					bots_nav_game.map, index, links[index].type == _bots_nav_teleport ? "teleporter" : "given",
					links[index].from.x, links[index].from.y, links[index].from.z, from,
					from >= 0 ? bots_nav_game.nav.nodes[from].reach : -1, links[index].to.x, links[index].to.y,
					links[index].to.z, to, to >= 0 ? bots_nav_game.nav.nodes[to].reach : -1);
			}
		}
		if (bots_nav_game.nav.main_node >= 0)
		{
			struct bots_nav_point const *main = &bots_nav_game.nav.nodes[bots_nav_game.nav.main_node].point;
			long both = 0;

			for (index = 0; index < bots_nav_game.nav.node_count; index++)
				both += bots_nav_game.nav.nodes[index].reach == (_bots_nav_reached_bit | _bots_nav_reaches_bit);
			platform_log("bots: walking graph of %s: from (%.2f %.2f %.2f), %ld of its nodes reached both ways (%ld before"
				" what the spawns' part neither reaches nor is reached from was dropped)", bots_nav_game.map, main->x,
				main->y, main->z, both, statistics->nodes_before_pruning);
		}
	}
	free(seeds);
	free(weapons);
}

/* the drive graph of the map being played, from the cache or built: its
seeds the level's vehicles (their placements, whichever the game type
leaves), whose part of the level is kept */
static void bots_nav_make_drive(
	void)
{
	struct scenario *scenario = global_scenario_get();
	struct collision_bsp *bsp = global_collision_bsp_get();
	struct tag_block *placements;
	struct bots_nav_parameters parameters;
	struct bots_nav_point *seeds;
	long element_size = 0;
	long seed_count = 0, pieces = 0;
	unsigned long started = system_milliseconds();
	boolean cached;
	long index;

	bots_nav_free(&bots_nav_drive.nav);
	bots_nav_drive.built = TRUE;
	bots_nav_drive.scenario_index = global_scenario_index;
	bots_nav_drive.structure_bsp_index = global_structure_bsp_index;
	bots_nav_vehicle_parameters(&parameters);
	if (!scenario || !bsp)
		return;
	placements = scenario_get_object_type_scenario_datums(scenario, _object_type_vehicle, &element_size);
	if (!placements || placements->count <= 0)
	{
		if (config_boolean("debug.bot_nav_check"))
			platform_log("bots: no drive graph for %s: it places no vehicles", bots_nav_game.map);
		return;
	}
	seeds = (struct bots_nav_point *)malloc((size_t)placements->count * sizeof(*seeds));
	if (!seeds)
		return;
	for (index = 0; index < placements->count; index++)
	{
		struct scenario_object_datum const *placement = (struct scenario_object_datum const *)
			tag_block_get_element_with_size(placements, index, element_size);

		bots_nav_point_from(&seeds[seed_count++], &placement->position);
	}
	cached = bots_nav_cache_read(&bots_nav_drive.nav, &parameters, "drive");
	if (!cached)
	{
		struct bots_nav_triangles triangles;

		csmemset(&triangles, 0, sizeof(triangles));
		if (bots_nav_add_bsp(&triangles, bsp, NULL) && bots_nav_add_scenery(&triangles, &pieces))
		{
			struct bots_nav_input input;

			/* (no teleporters, ladders or a map's links: a vehicle takes none) */
			csmemset(&input, 0, sizeof(input));
			input.triangles = triangles.points;
			input.triangle_count = triangles.count;
			input.seeds = seeds;
			input.seed_count = seed_count;
			input.clear = bots_nav_clear;
			if (bots_nav_build(&input, &parameters, &bots_nav_drive.nav))
				bots_nav_cache_write(&bots_nav_drive.nav, "drive");
		}
		free(triangles.points);
	}
	if (!bots_nav_drive.nav.nodes)
	{
		platform_log("bots: no drive graph for %s", bots_nav_game.map);
	}
	else
	{
		struct bots_nav_statistics const *statistics = &bots_nav_drive.nav.statistics;

		platform_log("bots: drive graph of %s %s in %lu ms: %ld nodes, %ld edges (%ld drives, %ld drops), %ld triangles; "
			"vehicles %ld/%ld on its main part",
			bots_nav_game.map, cached ? "read from the cache" : "built", system_milliseconds() - started,
			bots_nav_drive.nav.node_count, bots_nav_drive.nav.edge_count, statistics->walk_edges, statistics->drop_edges,
			statistics->triangles, bots_nav_reached(&bots_nav_drive.nav, seeds, seed_count, "vehicle",
			_bots_nav_reached_bit | _bots_nav_reaches_bit), seed_count);
	}
	free(seeds);
}

struct bots_nav *bots_nav_drive_current(
	void)
{
	return bots_nav_drive.built && bots_nav_drive.nav.nodes ? &bots_nav_drive.nav : NULL;
}

struct bots_nav *bots_nav_current(
	void)
{
	return bots_nav_game.built && bots_nav_game.nav.nodes ? &bots_nav_game.nav : NULL;
}

void bots_nav_update(
	boolean needed,
	boolean drive)
{
	boolean check = config_boolean("debug.bot_nav_check") != 0;

	/* (a multiplayer game: not the main menu's scene, nor co-op) */
	if (!game_in_progress() || !game_engine_running() || !global_scenario_get() || !global_collision_bsp_get())
	{
		if (bots_nav_game.built)
		{
			bots_nav_free(&bots_nav_game.nav);
			bots_nav_game.built = FALSE;
		}
		if (bots_nav_drive.built)
		{
			bots_nav_free(&bots_nav_drive.nav);
			bots_nav_drive.built = FALSE;
		}
		return;
	}
	if ((needed || check) && (!bots_nav_game.built || bots_nav_game.scenario_index != global_scenario_index ||
		bots_nav_game.structure_bsp_index != global_structure_bsp_index))
	{
		bots_nav_make();
	}
	/* (the drive graph, after the walking graph: its map name and key) */
	if (bots_nav_game.built && (drive || check) && (!bots_nav_drive.built ||
		bots_nav_drive.scenario_index != global_scenario_index ||
		bots_nav_drive.structure_bsp_index != global_structure_bsp_index))
	{
		bots_nav_make_drive();
	}
	/* (debug.bot_nav_check: on to the next map) */
	if (check && bots_nav_game.built && !bots_nav_game.checked && game_engine_running())
	{
		bots_nav_game.checked = TRUE;
		game_engine_end_game();
	}
}
