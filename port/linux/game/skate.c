/* skate.c: Skate 3 mode (skate.h, port/skate/README.md)

Loading happens before the player wants to skate: the skate session (the
skater, its animation banks, state graphs and physics, which can take a long
while) is made in the background at startup, and each structure BSP's
collision goes to the engine as triangles when it loads, to be built in the
background too. Getting on a board is then immediate; one pressed for while
something is still loading gets on when it is ready. While skating, a
tick sends the pad to the engine and takes back the skater: the biped is put
where the board is, its own movement is skipped (bipeds.c), and its nodes are
posed as the skater's bones; the board, skinned to them, is drawn with the
objects (render.c). */

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "main/console.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "objects/object_lights_rendering.h"
#include "physics/bsp3d.h"
#include "physics/collision_bsp.h"
#include "physics/collision_bsp_definitions.h"
#include "physics/collision_model_definitions.h"
#include "physics/collision_models.h"
#include "physics/collisions.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "units/biped_definitions.h"
#include "units/bipeds.h"
#include "units/units.h"

#include "skate.h"
#include "skate_sound.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef HALO_SKATE

#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"

#include <float.h>
#include <xtl.h>

#include "../../skate/include/halo_skate.h"

/* the platform layer's (sdl_platform.c, xinput_sdl.c, d3d8_gl.c) */
int halo_skate_platform_toggle_pressed(void);
void halo_skate_platform_pad(struct halo_skate_pad *pad);
void halo_skate_platform_board_texture(int slot, int width, int height, const unsigned char *rgba);
void halo_skate_platform_board_draw(const float *vertices, int vertex_count, const unsigned int *indices,
	int index_count, const struct halo_skate_board_surface *surfaces, int surface_count, const float *lights);
/* the platform layer's log (halo.log on Windows, stderr on Linux) */
void platform_log(const char *format, ...);

#define SKATE_TWO_SIDED_FLAG 0x01
#define SKATE_MAXIMUM_NODES 64
/* how far below the biped its spawn looks for the ground */
#define SKATE_GROUND_PROBE 1.f
#define SKATE_THUMB_CLICKS 0x00C0
/* world units the board may move in one tick before it is drawn there at
once: past any speed on a board (90 m/s) */
#define SKATE_BOARD_SNAP_DISTANCE 1.f

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
	/* the heading the following camera keeps: the way the board travels, or
	off the board the way the skater walks (camera.rs) */
	struct halo_skate_follow camera;
	struct halo_skate_frame frame;
	/* the skeleton was described again on this board (skate_update_after_objects) */
	boolean skeleton_described_again;
	/* a structure bsp loaded: its collision goes to the engine on the next
	tick, with the scenery placed in it by then (skate_load_map) */
	boolean map_pending;
} skate_globals ={ FALSE, FALSE, FALSE, NONE, NONE, NONE, NONE, 0, 0.f, { 0.f, 0.f } };

/* the board grown about its middle: Master Chief (about 2.1 m) is larger
than the skater it was made for (about 1.8 m) */
#define SKATE_DEFAULT_BOARD_SCALE 1.05f
#define SKATE_MAXIMUM_BOARD_SCALE 10.f
/* the furthest skate_feet_offset raises or lowers the biped, world units */
#define SKATE_MAXIMUM_FEET_OFFSET 1.f
/* the camera turns to the way the skater travels when faster than this
along the ground (world units a second: 1 m/s), and holds its heading when
slower, so that it neither spins at a standstill nor wobbles at a crawl */
#define SKATE_DEFAULT_CAMERA_SPEED (1.f / 3.048f)
#define SKATE_MAXIMUM_CAMERA_SPEED 10.f
/* the part of the way to that heading the camera turns each tick: rolling
back off a wall it comes round in about a third of a second */
#define SKATE_CAMERA_TURN 0.2f
/* running into a person or a vehicle faster than this (world units a second
along the contact: 6 m/s) knocks the skater over; slower is a bump */
#define SKATE_DEFAULT_OBSTACLE_BAIL_SPEED (6.f / 3.048f)
#define SKATE_MAXIMUM_OBSTACLE_BAIL_SPEED 100.f
/* how far around the foot nodes, across the ground, a vertex of the bind
pose counts as the foot's (world units) */
#define SKATE_FOOT_REGION 0.15f
/* an ankle height outside these (world units, 1.5 cm to 30 cm) was not
measured right */
#define SKATE_MINIMUM_ANKLE_HEIGHT 0.005f
#define SKATE_MAXIMUM_ANKLE_HEIGHT 0.1f

/* the values tuned at the console (skate_console_setting), for the session */
static struct
{
	real board_scale;
	real feet_offset;
	real camera_speed;
	/* the biped's ankles above its soles (skate_describe_skeleton), 0 for
	unknown */
	real ankle_height;
	/* how fast running into a person or vehicle knocks the skater over */
	real obstacle_bail_speed;
} skate_settings = { SKATE_DEFAULT_BOARD_SCALE, 0.f, SKATE_DEFAULT_CAMERA_SPEED, 0.f, SKATE_DEFAULT_OBSTACLE_BAIL_SPEED };

static boolean skate_text_has(char const *text, char const *word, boolean whole);

/* the board as the engine last skinned it (skate_board_capture) */
static struct
{
	/* the board taken, 0 for none */
	unsigned int generation;
	unsigned int uploaded_generation;
	int vertex_count;
	int index_count;
	int surface_count;
	int texture_count;
	unsigned int *indices;
	struct halo_skate_board_surface *surfaces;
	/* the last two ticks' vertices, the latest of them, and a frame's
	between them */
	float *vertices[2];
	short latest;
	boolean has_latest;
	boolean has_previous;
	float *drawn;
	/* the ambient light, then each distant light's direction and color */
	float lights[5][4];
} skate_board;

/* ... and at each other machine's skater (skate_network.c) */
static float skate_remote_lights[SKATE_NETWORK_PLAYERS][5][4];

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

/* the BSP of a node of an object's collision model, the permutation the
object shows; NULL for none */
static struct collision_bsp *skate_node_bsp(struct collision_model_instance const *instance, short node_index)
{
	struct collision_node const *node = TAG_BLOCK_GET_ELEMENT(&instance->model->nodes, node_index,
		struct collision_node);
	short permutation;

	if (node->region_index < 0 || node->region_index >= MAXIMUM_REGIONS_PER_OBJECT || node->bsps.count <= 0)
		return NULL;
	permutation = instance->region_permutation_indices[node->region_index];
	permutation = PIN(permutation, 0, (short)(node->bsps.count - 1));
	return TAG_BLOCK_GET_ELEMENT(&node->bsps, permutation, struct collision_bsp);
}

/* ---------- static scenery, part of the level

Scenery does not move, so the level's collision goes to the engine with the
scenery placed in it then: its collision models' triangles, as each node of
them is placed, join the structure BSP's (skate_load_map). The board and the
skater meet a rock's slopes and a crate's top as they are, at any size, the
skater walking off the board stops at them, and their edges are grind rails
(the rail finder takes them with the level's). Those pieces are then no
obstacles (skate_send_obstacles). Scenery made later (by a script) is an
obstacle each tick, as a box; a piece destroyed or moved later stays where it
was to the engine until the next structure BSP. */

#define SKATE_MAXIMUM_BAKED_SCENERY 1024

/* the scenery sent with the level's collision, sorted */
static long skate_baked_scenery[SKATE_MAXIMUM_BAKED_SCENERY];
static int skate_baked_scenery_count;

static int skate_compare_object_indices(void const *a, void const *b)
{
	long x = *(long const *)a, y = *(long const *)b;

	return x < y ? -1 : x > y;
}

static boolean skate_scenery_baked(long object_index)
{
	return skate_baked_scenery_count > 0 && bsearch(&object_index, skate_baked_scenery,
		(size_t)skate_baked_scenery_count, sizeof(long), skate_compare_object_indices) != NULL;
}

/* triangles (count of them) with the static scenery's added, each node's BSP
as skate_collision_triangles makes it, placed by the node; the scenery taken
is noted (skate_baked_scenery). The array grown, or NULL (and triangles
freed) out of memory */
static float *skate_add_scenery(float *triangles, long *count, long *pieces, long *added)
{
	struct object_iterator iterator;
	struct object_datum *scenery;

	*pieces = 0;
	*added = 0;
	skate_baked_scenery_count = 0;
	object_iterator_new(&iterator, _object_mask_scenery, 0);
	while ((scenery = (struct object_datum *)object_iterator_next(&iterator)) != NULL &&
		skate_baked_scenery_count < SKATE_MAXIMUM_BAKED_SCENERY)
	{
		struct collision_model_instance instance;
		long object_index = iterator.index;
		long before = *count;
		short node_index;

		if (scenery->object.parent_object_index != NONE ||
			!collision_model_instance_new(&instance, object_index) || !instance.matrices)
		{
			continue;
		}
		for (node_index = 0; node_index < instance.node_count; node_index++)
		{
			struct collision_bsp *bsp = skate_node_bsp(&instance, node_index);
			real_matrix4x3 const *matrix = &instance.matrices[node_index];
			float *node_triangles;
			float *grown;
			long node_count;
			long index;

			if (!bsp || !(node_triangles = skate_collision_triangles(bsp, &node_count)))
				continue;
			grown = node_count ? (float *)realloc(triangles, (size_t)(*count + node_count) * 9 * sizeof(float)) :
				triangles;
			if (!grown)
			{
				free(node_triangles);
				free(triangles);
				return NULL;
			}
			triangles = grown;
			for (index = 0; index < node_count * 3; index++)
			{
				real_point3d point, placed;

				point.x = node_triangles[index * 3];
				point.y = node_triangles[index * 3 + 1];
				point.z = node_triangles[index * 3 + 2];
				matrix4x3_transform_point(matrix, &point, &placed);
				triangles[*count * 9 + index * 3] = placed.x;
				triangles[*count * 9 + index * 3 + 1] = placed.y;
				triangles[*count * 9 + index * 3 + 2] = placed.z;
			}
			*count += node_count;
			free(node_triangles);
		}
		if (*count > before)
		{
			skate_baked_scenery[skate_baked_scenery_count++] = object_index;
			(*pieces)++;
			*added += *count - before;
		}
	}
	qsort(skate_baked_scenery, (size_t)skate_baked_scenery_count, sizeof(long), skate_compare_object_indices);
	return triangles;
}

/* sends the BSP's collision, with the static scenery in it, to be built in
the background; the engine copies the triangles, so this never waits for the
build */
static boolean skate_load_map(void)
{
	struct collision_bsp *bsp = global_collision_bsp_get();
	long count;
	long pieces = 0;
	long added = 0;
	float *triangles;
	boolean sent = FALSE;

	skate_globals.map_pending = FALSE;
	skate_baked_scenery_count = 0;
	if (!bsp)
		return FALSE;
	triangles = skate_collision_triangles(bsp, &count);
	if (triangles)
		triangles = skate_add_scenery(triangles, &count, &pieces, &added);
	if (!triangles)
	{
		skate_baked_scenery_count = 0;
		return FALSE;
	}
	platform_log("skate: the level's collision: %ld triangles, %ld of them %ld pieces of scenery", count, added,
		pieces);
	if (halo_skate_load(skate_assets(), triangles, (int)count) == 0)
	{
		skate_globals.loaded_scenario_index = global_scenario_index;
		skate_globals.loaded_structure_bsp_index = global_structure_bsp_index;
		sent = TRUE;
	}
	else
		skate_baked_scenery_count = 0;
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
	halo_skate_set_board_scale(skate_settings.board_scale);
	halo_skate_set_feet(skate_settings.ankle_height, skate_settings.feet_offset);
	halo_skate_set_obstacle_bail_speed(skate_settings.obstacle_bail_speed);
	/* (the engine says when there is no assets folder; nothing more happens
	until J) */
	skate_globals.preloaded = halo_skate_preload(skate_assets()) == 0;
	/* the skater's sounds (skate_sound.c) */
	skate_sound_initialize(skate_assets());
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
	skate_globals.map_pending = FALSE;
	skate_baked_scenery_count = 0;
	/* nobody skates in the main menu; a failed preload is retried on J */
	if (!skate_globals.preloaded || !scenario || scenario->type == _scenario_type_main_menu ||
		halo_skate_state() == -1)
	{
		return;
	}
	/* (sent on the next tick, skate_update_before_objects: a new map's
	scenery is placed after its first bsp loads) */
	skate_globals.map_pending = TRUE;
	skate_globals.activate_when_ready = waiting;
}

/* a model's geometry and part, as models.c and powerup_render_bounds.c each
define them for themselves (no header declares them) */
struct model_geometry
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct model_geometry_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	short centroid_secondary_node_index;
	real centroid_primary_node_weight;
	real centroid_secondary_node_weight;
	real_point3d centroid;
	struct tag_block uncompressed_vertices;
	struct tag_block compressed_vertices;
	struct tag_block triangles;
	struct triangle_buffer triangle_buffer;
	struct vertex_buffer vertex_buffer;
};

typedef char verify_skate_model_geometry_part_size[sizeof(struct model_geometry_part) == 0x68 ? 1 : -1];

#define SKATE_MODEL_GEOMETRY_PART_STRIPPED_BIT 0

/* the lowest vertex, in the bind pose, of the model's geometry within
SKATE_FOOT_REGION of a foot across the ground: the soles. A model's vertices
are in the bind pose's model space (models.c skins them by each node's
matrix times its default inverse). FALSE when there is none to read */
static boolean skate_model_lowest_foot_vertex(struct model *model, real_point3d const *feet, real *lowest,
	long *vertex_count)
{
	long geometry_index;

	*lowest = FLT_MAX;
	*vertex_count = 0;
	if (!model->geometries.address || model->geometries.count <= 0 ||
		model->geometries.count > MAXIMUM_GEOMETRIES_PER_MODEL)
	{
		return FALSE;
	}
	for (geometry_index = 0; geometry_index < model->geometries.count; geometry_index++)
	{
		struct model_geometry *geometry = TAG_BLOCK_GET_ELEMENT(&model->geometries, geometry_index,
			struct model_geometry);
		long part_index;

		if (geometry->parts.count <= 0 || geometry->parts.count > MAXIMUM_PARTS_PER_MODEL_GEOMETRY ||
			!geometry->parts.address)
		{
			continue;
		}
		for (part_index = 0; part_index < geometry->parts.count; part_index++)
		{
			struct model_geometry_part *part = TAG_BLOCK_GET_ELEMENT(&geometry->parts, part_index,
				struct model_geometry_part);
			struct vertex_buffer *buffer = &part->vertex_buffer;
			byte *vertices = NULL;
			long stride;
			long vertex_index;

			/* (as powerup_render_bounds.c reads them) */
			if (TEST_FLAG(part->flags, SKATE_MODEL_GEOMETRY_PART_STRIPPED_BIT) || !buffer->hardware_format ||
				buffer->offset || buffer->count <= 0 || buffer->count > MAXIMUM_VERTICES_PER_MODEL_GEOMETRY_PART ||
				(buffer->type != _rasterizer_vertex_type_model_compressed &&
				buffer->type != _rasterizer_vertex_type_model_uncompressed))
			{
				continue;
			}
			stride = rasterizer_geometry_get_vertex_size(buffer->type);
			IDirect3DVertexBuffer8_Lock(buffer->hardware_format, 0, 0, &vertices, D3DLOCK_READONLY);
			if (!vertices)
				continue;
			for (vertex_index = 0; vertex_index < buffer->count; vertex_index++)
			{
				real_point3d const *point = (real_point3d const *)(vertices + vertex_index * stride);
				short foot;

				for (foot = 0; foot < 2; foot++)
				{
					real dx = point->x - feet[foot].x;
					real dy = point->y - feet[foot].y;

					/* (a number, near the foot) */
					if (dx * dx + dy * dy <= SKATE_FOOT_REGION * SKATE_FOOT_REGION && point->z > -FLT_MAX &&
						point->z < FLT_MAX)
					{
						if (point->z < *lowest)
							*lowest = point->z;
						(*vertex_count)++;
						break;
					}
				}
			}
			IDirect3DVertexBuffer8_Unlock(buffer->hardware_format);
		}
	}
	return *vertex_count > 0;
}

/* how far the biped's ankles (its foot nodes) are above its soles, world
units, measured on its model's bind pose: from the lowest vertex around its
feet, else from the model's origin (where a biped's feet stand), else 0 for
unknown. Logged, with how it was found */
static real skate_model_ankle_height(struct model *model)
{
	real_point3d feet[2];
	short found = 0;
	short index;
	real ankle;
	real lowest;
	long vertex_count;

	for (index = 0; index < model->nodes.count && found < 2; index++)
	{
		struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);
		real_matrix4x3 bind;

		if (!skate_text_has(node->name, found ? "bip01 r foot" : "bip01 l foot", TRUE))
			continue;
		matrix4x3_inverse(&node->runtime_default_inverse_matrix, &bind);
		feet[found++] = bind.position;
		index = -1; /* (the right foot may come before the left) */
	}
	if (found < 2)
	{
		platform_log("skate: the biped has no bip01 l foot and r foot: its ankles stay on the skater's");
		return 0.f;
	}
	ankle = (feet[0].z + feet[1].z) * 0.5f;
	if (skate_model_lowest_foot_vertex(model, feet, &lowest, &vertex_count) &&
		ankle - lowest >= SKATE_MINIMUM_ANKLE_HEIGHT && ankle - lowest <= SKATE_MAXIMUM_ANKLE_HEIGHT)
	{
		platform_log("skate: the biped's ankles are %.4f world units (%.1f cm) above its soles, the lowest of %ld "
			"vertices around its feet (ankles %.4f up in the bind pose, soles %.4f)",
			ankle - lowest, (ankle - lowest) * 304.8f, vertex_count, ankle, lowest);
		return ankle - lowest;
	}
	if (ankle >= SKATE_MINIMUM_ANKLE_HEIGHT && ankle <= SKATE_MAXIMUM_ANKLE_HEIGHT)
	{
		platform_log("skate: the biped's ankles are %.4f world units (%.1f cm) above its model's origin, taken as "
			"its soles (%ld vertices around its feet, the lowest %.4f)", ankle, ankle * 304.8f, vertex_count,
			vertex_count ? lowest : 0.f);
		return ankle;
	}
	platform_log("skate: the biped's ankle height is unknown (ankles %.4f up in the bind pose, %ld vertices "
		"around its feet): its ankles stay on the skater's", ankle, vertex_count);
	return 0.f;
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
	/* riding, its soles rest on the deck (rig.rs) */
	skate_settings.ankle_height = skate_model_ankle_height(model);
	halo_skate_set_feet(skate_settings.ankle_height, skate_settings.feet_offset);
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

/* starting: getting on the board, when the camera takes the board's heading */
static void skate_apply_frame(long unit_index, boolean starting)
{
	struct object_datum *object = object_get(unit_index);
	real_point3d position;
	real_vector3d forward, up;
	float const *velocity;

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
	/* the camera turns to the way the board travels, not the way the body
	faces: off a wall the skater rolls back fakie, still facing it. Off the
	board it follows the skater: the way it walks, or, stood still a while,
	round behind the way it faces, wherever the board went */
	if (starting)
	{
		skate_globals.camera.heading = (real)atan2(forward.j, forward.i);
		skate_globals.camera.still = 0.f;
	}
	else
	{
		halo_skate_follow(&skate_globals.camera, &skate_globals.frame, skate_settings.camera_speed,
			SKATE_CAMERA_TURN, 1.f / TICKS_PER_SECOND);
	}
	/* (the player's desired yaw must be within 0 to 2 pi: player_control.c) */
	if (!(skate_globals.camera.heading >= 0.f && skate_globals.camera.heading < 2.f * _pi))
		skate_globals.camera.heading = (real)fmod(skate_globals.camera.heading, 2.f * _pi);
	if (skate_globals.camera.heading < 0.f)
		skate_globals.camera.heading += 2.f * _pi;
	if (!(skate_globals.camera.heading >= 0.f && skate_globals.camera.heading < 2.f * _pi))
		skate_globals.camera.heading = 0.f;

	object_set_position(unit_index, &position, &forward, &up);
	/* the biped moves with the skater: off the board, not with the board */
	velocity = skate_globals.frame.off_board ? skate_globals.frame.skater_velocity : skate_globals.frame.velocity;
	object->object.translational_velocity.i = velocity[0] / TICKS_PER_SECOND;
	object->object.translational_velocity.j = velocity[1] / TICKS_PER_SECOND;
	object->object.translational_velocity.k = velocity[2] / TICKS_PER_SECOND;
}

/* ---------- the board

The skate engine skins the board each tick (halo_skate_board_vertices); a
frame draws it between the last two ticks, as the biped's nodes are
(render_interpolation.c), so that it stays under the skater's feet. */

/* (the game's free is debug_free, which takes no NULL: it reads the header
before the pointer) */
static void skate_free(void *pointer)
{
	if (pointer)
		free(pointer);
}

static void skate_board_free(void)
{
	skate_free(skate_board.indices);
	skate_free(skate_board.surfaces);
	skate_free(skate_board.vertices[0]);
	skate_free(skate_board.vertices[1]);
	skate_free(skate_board.drawn);
	memset(&skate_board, 0, sizeof(skate_board));
}

/* takes another board's mesh; FALSE if there is none to draw */
static boolean skate_board_take(struct halo_skate_board_info const *info)
{
	size_t vertex_bytes = (size_t)info->vertex_count * HALO_SKATE_BOARD_VERTEX_FLOATS * sizeof(float);

	skate_board_free();
	skate_board.generation = info->generation;
	if (info->vertex_count <= 0 || info->index_count <= 0 || info->surface_count <= 0)
		return FALSE;
	skate_board.indices = (unsigned int *)malloc((size_t)info->index_count * sizeof(unsigned int));
	skate_board.surfaces = (struct halo_skate_board_surface *)malloc(
		(size_t)info->surface_count * sizeof(struct halo_skate_board_surface));
	skate_board.vertices[0] = (float *)malloc(vertex_bytes);
	skate_board.vertices[1] = (float *)malloc(vertex_bytes);
	skate_board.drawn = (float *)malloc(vertex_bytes);
	if (!skate_board.indices || !skate_board.surfaces || !skate_board.vertices[0] || !skate_board.vertices[1] ||
		!skate_board.drawn || halo_skate_board_mesh(skate_board.indices, info->index_count,
		skate_board.surfaces, info->surface_count) != 0)
	{
		skate_board_free();
		skate_board.generation = info->generation;
		return FALSE;
	}
	skate_board.vertex_count = info->vertex_count;
	skate_board.index_count = info->index_count;
	skate_board.surface_count = info->surface_count;
	skate_board.texture_count = info->texture_count < HALO_SKATE_BOARD_TEXTURES ?
		info->texture_count : HALO_SKATE_BOARD_TEXTURES;
	return TRUE;
}

/* the lights of the skater's place in the level (lights_prepare_for_object_static),
as the shader of halo_skate_platform_board_draw takes them */
static void skate_board_light(long unit_index, float lights[5][4])
{
	struct render_lighting lighting;
	short light_index;
	real total;

	memset(&lighting, 0, sizeof(lighting));
	lights_prepare_for_object_static(unit_index, &lighting);
	memset(lights, 0, 5 * sizeof(lights[0]));
	lights[0][0] = lighting.ambient_color.red;
	lights[0][1] = lighting.ambient_color.green;
	lights[0][2] = lighting.ambient_color.blue;
	total = lighting.ambient_color.red + lighting.ambient_color.green + lighting.ambient_color.blue;
	for (light_index = 0; light_index < lighting.distant_light_count && light_index < MAXIMUM_RENDERED_DISTANT_LIGHTS;
		light_index++)
	{
		struct render_distant_light const *light = &lighting.distant_lights[light_index];

		lights[1 + 2 * light_index][0] = light->direction.i;
		lights[1 + 2 * light_index][1] = light->direction.j;
		lights[1 + 2 * light_index][2] = light->direction.k;
		lights[2 + 2 * light_index][0] = light->color.red;
		lights[2 + 2 * light_index][1] = light->color.green;
		lights[2 + 2 * light_index][2] = light->color.blue;
		total += light->color.red + light->color.green + light->color.blue;
	}
	/* (no light found, or none that is a number: grey, lit from above,
	rather than black) */
	if (!(total > 0.f))
	{
		memset(lights, 0, 5 * sizeof(lights[0]));
		lights[0][0] = lights[0][1] = lights[0][2] = 0.4f;
		lights[1][2] = -1.f;
		lights[2][0] = lights[2][1] = lights[2][2] = 0.6f;
	}
}

/* nothing to draw from until the next tick on a board */
static void skate_board_forget(void)
{
	skate_board.has_latest = FALSE;
	skate_board.has_previous = FALSE;
}

/* the tick's board, skinned by the pose the engine just gave */
static void skate_board_capture(long unit_index)
{
	struct halo_skate_board_info info;
	short next;
	float const *previous;
	float const *latest;

	if (halo_skate_board_info(&info) != 0)
	{
		skate_board_forget();
		return;
	}
	if (info.generation != skate_board.generation && !skate_board_take(&info))
		return;
	if (!skate_board.vertex_count)
		return;
	next = skate_board.has_latest ? skate_board.latest ^ 1 : skate_board.latest;
	if (halo_skate_board_vertices(skate_board.vertices[next], skate_board.vertex_count) != skate_board.vertex_count)
	{
		skate_board_forget();
		return;
	}
	skate_board.has_previous = skate_board.has_latest;
	skate_board.has_latest = TRUE;
	skate_board.latest = next;
	/* further than a tick of skating moves it (the engine put the skater
	somewhere else): drawn there at once, not swept across the level */
	if (skate_board.has_previous)
	{
		real dx, dy, dz;

		previous = skate_board.vertices[next ^ 1];
		latest = skate_board.vertices[next];
		dx = latest[0] - previous[0];
		dy = latest[1] - previous[1];
		dz = latest[2] - previous[2];
		if (!(dx * dx + dy * dy + dz * dz <= SKATE_BOARD_SNAP_DISTANCE * SKATE_BOARD_SNAP_DISTANCE))
			skate_board.has_previous = FALSE;
	}
	skate_board_light(unit_index, skate_board.lights);
}

/* the board's mesh taken (another's when the engine loaded another), for
another machine's skater too; FALSE with none */
static boolean skate_board_ready(void)
{
	struct halo_skate_board_info info;

	if (halo_skate_board_info(&info) != 0)
		return FALSE;
	if (info.generation != skate_board.generation && !skate_board_take(&info))
		return FALSE;
	return skate_board.vertex_count > 0;
}

/* the board's textures, the first time a board is drawn */
static void skate_board_upload_textures(void)
{
	int texture_index;

	for (texture_index = 0; texture_index < skate_board.texture_count; texture_index++)
	{
		int width, height;
		unsigned char *rgba;

		if (halo_skate_board_texture(texture_index, &width, &height, NULL, 0) != 0 || width <= 0 || height <= 0)
			continue;
		rgba = (unsigned char *)malloc((size_t)width * height * 4);
		if (!rgba)
			continue;
		if (halo_skate_board_texture(texture_index, &width, &height, rgba, width * height * 4) == 0)
			halo_skate_platform_board_texture(texture_index, width, height, rgba);
		free(rgba);
	}
	skate_board.uploaded_generation = skate_board.generation;
}

void skate_render_board(void)
{
	float const *previous;
	float const *latest;
	float const *drawn;
	real fraction;
	long index;

	short player_index;

	/* other machines' skaters' boards, placed whole where their states have
	them, between their last two ticks (skate_network.c) */
	fraction = render_interpolation_fraction();
	for (player_index = 0; player_index < SKATE_NETWORK_PLAYERS; player_index++)
	{
		real placement[7];
		long unit_index;

		if (!skate_network_board(player_index, fraction, &unit_index, placement) || !skate_board.vertex_count ||
			!skate_board.drawn)
		{
			continue;
		}
		if (skate_board.uploaded_generation != skate_board.generation)
			skate_board_upload_textures();
		if (halo_skate_board_vertices_at(placement, skate_board.drawn, skate_board.vertex_count) !=
			skate_board.vertex_count)
		{
			continue;
		}
		halo_skate_platform_board_draw(skate_board.drawn, skate_board.vertex_count, skate_board.indices,
			skate_board.index_count, skate_board.surfaces, skate_board.surface_count,
			&skate_remote_lights[player_index][0][0]);
	}
	if (!skate_globals.skating || !skate_board.has_latest || !skate_board.vertex_count)
		return;
	if (skate_board.uploaded_generation != skate_board.generation)
		skate_board_upload_textures();
	latest = skate_board.vertices[skate_board.latest];
	drawn = latest;
	if (skate_board.has_previous && fraction < 1.f)
	{
		previous = skate_board.vertices[skate_board.latest ^ 1];
		/* positions and normals blended (the shader normalises them), the
		texture coordinates the same in both */
		for (index = 0; index < skate_board.vertex_count * HALO_SKATE_BOARD_VERTEX_FLOATS; index++)
			skate_board.drawn[index] = previous[index] + (latest[index] - previous[index]) * fraction;
		drawn = skate_board.drawn;
	}
	halo_skate_platform_board_draw(drawn, skate_board.vertex_count, skate_board.indices, skate_board.index_count,
		skate_board.surfaces, skate_board.surface_count, &skate_board.lights[0][0]);
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
	skate_board_forget();
}

/* the engine failed (halo_skate_state -1): off the board as by J, so that the
biped walks, holds its weapon and is watched and aimed as on foot again (all
of which follow skate_globals.skating), and everything the engine was told
is told afresh: the next J restarts it with the map and the skeleton */
static void skate_engine_failed(void)
{
	if (skate_globals.skating || skate_globals.activate_when_ready)
		console_printf(FALSE, "skate: off (%s)", halo_skate_error());
	platform_log("skate: off, the skate engine failed: %s", halo_skate_error());
	skate_stop(NULL);
	skate_globals.loaded_scenario_index = NONE;
	skate_globals.loaded_structure_bsp_index = NONE;
	skate_globals.skeleton_definition_index = NONE;
}

/* the ground under from (from itself when none is near) */
static real_point3d skate_ground_under(long unit_index, real_point3d const *from)
{
	struct collision_result collision;
	real_point3d ground = *from;
	real_vector3d down;

	down.i = 0.f;
	down.j = 0.f;
	down.k = -SKATE_GROUND_PROBE;
	if (collision_test_vector(_collision_test_environment_flags, from, &down, unit_index,
		&collision))
	{
		ground = collision.point;
	}
	return ground;
}

/* the skater put on the board again at spot (on the ground under it), facing
yaw: the network test's duel (debug.network_test_skate_duel) */
static void skate_put(long unit_index, real_point3d const *spot, real yaw)
{
	real_point3d ground = skate_ground_under(unit_index, spot);

	if (halo_skate_activate(&ground.x, yaw, &skate_globals.frame) != 0)
		return;
	skate_board_forget();
	skate_apply_frame(unit_index, TRUE);
}

static void skate_start(long unit_index)
{
	struct object_datum *object = object_get(unit_index);
	real_point3d ground = skate_ground_under(unit_index, &object->object.position);

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
	skate_globals.skeleton_described_again = FALSE;
	skate_board_forget();
	skate_apply_frame(unit_index, TRUE);
	console_printf(FALSE, "skate: on");
}

/* ---------- the network test's skater
debug.network_test_skate: that many seconds into a game this machine's
player gets on a board and rides a scripted pad (port/linux/NETCODE.md,
"Testing"): pushes, a slow carve and an ollie every four seconds, and Y
to step back on whenever a fall left them walking; off the board
SKATE_TEST_OFF_SECONDS on, and on again five seconds later. */
#define SKATE_TEST_CYCLE_TICKS 120
#define SKATE_TEST_OFF_SECONDS 40
#define SKATE_TEST_BACK_SECONDS 45
#define SKATE_TEST_CARVE 8000.f

double config_real(const char *name);

/* debug.network_test_skate_duel: that many seconds into a game this
machine's player gets on a board and stands on it, Y stepping back on after
a fall; with debug.network_test_skate_block on a host it is put on the board
in another machine's skater's way every five seconds, facing them
(skate_update_before_objects), for them to ride into, skater into skater.
TRUE while it has the pad */
static boolean skate_test_duel_pad(struct halo_skate_pad *pad, boolean *toggle)
{
	static real start = -1.f;
	long tick;

	if (start < 0.f)
		start = (real)config_real("debug.network_test_skate_duel");
	if (!(start > 0.f) || !game_in_progress())
		return FALSE;
	tick = game_time_get() - (long)(start * TICKS_PER_SECOND);
	if (tick == 0 && !skate_globals.skating && !skate_globals.activate_when_ready)
	{
		platform_log("skate test: on the board, to stand in the skater's way");
		*toggle = TRUE;
	}
	if (tick >= 0 && skate_globals.skating)
	{
		memset(pad, 0, sizeof(*pad));
		if (skate_globals.frame.off_board && tick % 30 < 4)
			pad->buttons = 0x8000;
	}
	return TRUE;
}

static void skate_test_pad(struct halo_skate_pad *pad, boolean *toggle)
{
	static real start = -1.f;
	long tick;
	long cycle;

	if (skate_test_duel_pad(pad, toggle))
		return;
	if (start < 0.f)
		start = (real)config_real("debug.network_test_skate");
	if (!(start > 0.f) || !game_in_progress())
		return;
	tick = game_time_get() - (long)(start * TICKS_PER_SECOND);
	if (tick < 0)
		return;
	if ((tick == 0 || tick == SKATE_TEST_BACK_SECONDS * TICKS_PER_SECOND) &&
		!skate_globals.skating && !skate_globals.activate_when_ready)
	{
		platform_log("skate test: on the board");
		*toggle = TRUE;
	}
	if (tick == SKATE_TEST_OFF_SECONDS * TICKS_PER_SECOND && skate_globals.skating)
	{
		platform_log("skate test: off the board");
		*toggle = TRUE;
	}
	if (!skate_globals.skating)
		return;
	/* (pushes, A held half a second and let go as long, then the right
	stick pulled down and flicked up: real_assets.rs's push and ollie) */
	cycle = tick % SKATE_TEST_CYCLE_TICKS;
	memset(pad, 0, sizeof(*pad));
	if (skate_globals.frame.off_board)
	{
		if (cycle % 30 < 4)
			pad->buttons = 0x8000;
		return;
	}
	if (cycle < 90 && cycle % 30 < 15)
		pad->buttons = 0x1000;
	pad->left[0] = (short)(SKATE_TEST_CARVE * sin(tick * 2.f * _pi / (2 * SKATE_TEST_CYCLE_TICKS)));
	if (cycle >= 90 && cycle < 98)
		pad->right[1] = -32767;
	else if (cycle >= 98 && cycle < 100)
		pad->right[1] = 32767;
}

/* ---------- obstacles

Each tick the objects near the skater go to the engine as obstacles the
board and the skater collide with (halo_skate_set_obstacles; the engine's
side is skate-core's kinematic_obstacle.rs): other bipeds, players and AI,
yours and other machines' alike, as their collision pills, and vehicles,
scenery and devices as a box for each node of their collision model, the
box around that node's BSP as the object is now (a turret, a door, a lift
moves with its node). Each carries on at its velocity until the next tick.
Nothing is pushed back on Halo's side: the skater bumps or bails off them. */

/* how far from the skater objects are looked for (world units, 24 m: a
tick at any board speed is under 3 m) */
#define SKATE_OBSTACLE_RADIUS 8.f
/* scenery and devices larger than this (half a side, world units: 6 m) are
left out: a box would fill a building's doors and arches, or a bridge's
underside. Vehicles are taken up to the engine's most (50 m) */
#define SKATE_MAXIMUM_SCENERY_HALF_EXTENT 2.f
#define SKATE_OBSTACLE_SEARCH 128
/* a biped's velocity is taken up to this (world units a second: 10 m/s,
past any run): another machine's unit jumps where the host says after a
respawn, a teleporter or a correction, and its velocity briefly says so */
#define SKATE_MAXIMUM_BIPED_OBSTACLE_SPEED (10.f / 3.048f)
/* another machine's skater: their body's round (world units, 27 cm: a
skater's shoulders, slimmer than Halo's pill), and their velocity taken up to
20 m/s, past any push or carve */
#define SKATE_SKATER_RADIUS 0.09f
#define SKATE_MAXIMUM_SKATER_OBSTACLE_SPEED (20.f / 3.048f)

/* the box around a collision BSP's vertices in its node's space: FALSE for
none */
static boolean skate_bsp_bounds(struct collision_bsp *bsp, real_point3d *minimum, real_point3d *maximum)
{
	long index;

	if (bsp->vertices.count <= 0)
		return FALSE;
	for (index = 0; index < bsp->vertices.count; index++)
	{
		struct collision_vertex *vertex = TAG_BLOCK_GET_ELEMENT(&bsp->vertices, index, struct collision_vertex);

		if (index == 0)
		{
			*minimum = vertex->point;
			*maximum = vertex->point;
			continue;
		}
		minimum->x = MIN(minimum->x, vertex->point.x);
		minimum->y = MIN(minimum->y, vertex->point.y);
		minimum->z = MIN(minimum->z, vertex->point.z);
		maximum->x = MAX(maximum->x, vertex->point.x);
		maximum->y = MAX(maximum->y, vertex->point.y);
		maximum->z = MAX(maximum->z, vertex->point.z);
	}
	return TRUE;
}

/* the obstacles kept so far, nearest first, and how far each is */
struct skate_obstacles
{
	struct halo_skate_obstacle obstacles[HALO_SKATE_MAXIMUM_OBSTACLES];
	real distances[HALO_SKATE_MAXIMUM_OBSTACLES];
	int count;
};

/* keeps obstacle, distance world units away, among the nearest */
static void skate_obstacle_keep(struct skate_obstacles *kept, struct halo_skate_obstacle const *obstacle, real distance)
{
	int slot = kept->count;

	if (slot == HALO_SKATE_MAXIMUM_OBSTACLES)
	{
		if (distance >= kept->distances[slot - 1])
			return;
		slot--;
	}
	else
		kept->count++;
	while (slot > 0 && kept->distances[slot - 1] > distance)
	{
		kept->obstacles[slot] = kept->obstacles[slot - 1];
		kept->distances[slot] = kept->distances[slot - 1];
		slot--;
	}
	kept->obstacles[slot] = *obstacle;
	kept->distances[slot] = distance;
}

static void skate_obstacle_velocity(struct object_datum const *object, struct halo_skate_obstacle *obstacle)
{
	obstacle->velocity[0] = object->object.translational_velocity.i * TICKS_PER_SECOND;
	obstacle->velocity[1] = object->object.translational_velocity.j * TICKS_PER_SECOND;
	obstacle->velocity[2] = object->object.translational_velocity.k * TICKS_PER_SECOND;
}

static real skate_distance(real_point3d const *a, float const *b)
{
	real dx = a->x - b[0];
	real dy = a->y - b[1];
	real dz = a->z - b[2];

	return (real)sqrt(dx * dx + dy * dy + dz * dz);
}

/* a biped as its collision pill (bipeds.c's biped_get_physics_pill, but as
tall as it stands for AI too, whose pill Halo keeps a sphere) */
static void skate_biped_obstacle(long object_index, real_point3d const *skater, struct skate_obstacles *kept)
{
	struct biped_datum *biped = biped_get(object_index);
	struct biped_definition *definition = biped_definition_get(biped->definition_index);
	struct halo_skate_obstacle obstacle;
	real radius = definition->biped.collision_radius;
	real height = definition->biped.collision_height_standing +
		(definition->biped.collision_height_crouching - definition->biped.collision_height_standing) *
		biped->biped.crouch;
	real segment;
	real_point3d base;

	if (!(radius > 0.f))
		return;
	segment = TEST_FLAG(definition->biped.flags, _biped_spherical_bit) ? 0.f : MAX(height - 2.f * radius, 0.f);
	object_get_origin(object_index, &base);
	if (!TEST_FLAG(definition->biped.flags, _biped_pill_centered_at_origin_bit))
		base.z += radius;
	memset(&obstacle, 0, sizeof(obstacle));
	obstacle.kind = HALO_SKATE_OBSTACLE_BIPED;
	obstacle.shape = HALO_SKATE_OBSTACLE_CAPSULE;
	obstacle.center[0] = base.x;
	obstacle.center[1] = base.y;
	obstacle.center[2] = base.z + segment / 2.f;
	obstacle.forward[0] = 1.f;
	obstacle.left[1] = 1.f;
	obstacle.up[2] = 1.f;
	obstacle.half_extents[0] = radius;
	obstacle.half_extents[2] = segment / 2.f + radius;
	skate_obstacle_velocity(&biped->object, &obstacle);
	{
		real speed = (real)sqrt(obstacle.velocity[0] * obstacle.velocity[0] +
			obstacle.velocity[1] * obstacle.velocity[1] + obstacle.velocity[2] * obstacle.velocity[2]);

		if (speed > SKATE_MAXIMUM_BIPED_OBSTACLE_SPEED)
		{
			obstacle.velocity[0] *= SKATE_MAXIMUM_BIPED_OBSTACLE_SPEED / speed;
			obstacle.velocity[1] *= SKATE_MAXIMUM_BIPED_OBSTACLE_SPEED / speed;
			obstacle.velocity[2] *= SKATE_MAXIMUM_BIPED_OBSTACLE_SPEED / speed;
		}
	}
	skate_obstacle_keep(kept, &obstacle, skate_distance(skater, obstacle.center) - obstacle.half_extents[2]);
}

/* an object as a box for each node of its collision model that has a BSP */
static void skate_box_obstacles(long object_index, real_point3d const *skater, struct skate_obstacles *kept)
{
	struct object_datum *object = object_get(object_index);
	struct collision_model_instance instance;
	int kind = object->object.type == _object_type_vehicle ? HALO_SKATE_OBSTACLE_VEHICLE : HALO_SKATE_OBSTACLE_SCENERY;
	short node_index;

	if (!collision_model_instance_new(&instance, object_index) || !instance.matrices)
		return;
	for (node_index = 0; node_index < instance.node_count; node_index++)
	{
		real_matrix4x3 const *matrix = &instance.matrices[node_index];
		struct collision_bsp *bsp = skate_node_bsp(&instance, node_index);
		struct halo_skate_obstacle obstacle;
		real_point3d minimum, maximum, middle, center;
		real largest;

		if (!bsp || !skate_bsp_bounds(bsp, &minimum, &maximum))
			continue;
		memset(&obstacle, 0, sizeof(obstacle));
		obstacle.kind = kind;
		obstacle.shape = HALO_SKATE_OBSTACLE_BOX;
		obstacle.half_extents[0] = (maximum.x - minimum.x) / 2.f * matrix->scale;
		obstacle.half_extents[1] = (maximum.y - minimum.y) / 2.f * matrix->scale;
		obstacle.half_extents[2] = (maximum.z - minimum.z) / 2.f * matrix->scale;
		largest = MAX(obstacle.half_extents[0], MAX(obstacle.half_extents[1], obstacle.half_extents[2]));
		if (kind != HALO_SKATE_OBSTACLE_VEHICLE && largest > SKATE_MAXIMUM_SCENERY_HALF_EXTENT)
			continue;
		middle.x = (minimum.x + maximum.x) / 2.f;
		middle.y = (minimum.y + maximum.y) / 2.f;
		middle.z = (minimum.z + maximum.z) / 2.f;
		matrix4x3_transform_point(matrix, &middle, &center);
		obstacle.center[0] = center.x;
		obstacle.center[1] = center.y;
		obstacle.center[2] = center.z;
		obstacle.forward[0] = matrix->forward.i;
		obstacle.forward[1] = matrix->forward.j;
		obstacle.forward[2] = matrix->forward.k;
		obstacle.left[0] = matrix->left.i;
		obstacle.left[1] = matrix->left.j;
		obstacle.left[2] = matrix->left.k;
		obstacle.up[0] = matrix->up.i;
		obstacle.up[1] = matrix->up.j;
		obstacle.up[2] = matrix->up.k;
		skate_obstacle_velocity(object, &obstacle);
		skate_obstacle_keep(kept, &obstacle, skate_distance(skater, obstacle.center) - largest);
	}
}

/* the velocity of an obstacle taken up to maximum (world units a second) */
static void skate_obstacle_speed_limit(struct halo_skate_obstacle *obstacle, real maximum)
{
	real speed = (real)sqrt(obstacle->velocity[0] * obstacle->velocity[0] +
		obstacle->velocity[1] * obstacle->velocity[1] + obstacle->velocity[2] * obstacle->velocity[2]);
	int index;

	if (speed > maximum)
	{
		for (index = 0; index < 3; index++)
			obstacle->velocity[index] *= maximum / speed;
	}
}

/* the nodes of another skater's body (the cyborg's, as rig.rs expects them):
NONE for a model without them */
static void skate_skater_nodes(long definition_index, short nodes[3])
{
	static char const *const names[3] = { "bip01 head", "bip01 l foot", "bip01 r foot" };
	static long cached_definition_index = NONE;
	static short cached[3] = { NONE, NONE, NONE };
	struct object_definition *definition;
	short which;

	if (definition_index != cached_definition_index)
	{
		cached_definition_index = definition_index;
		definition = object_definition_get(definition_index);
		for (which = 0; which < 3; which++)
		{
			struct model *model = definition->object.model.index != NONE ?
				model_definition_get(definition->object.model.index) : NULL;
			short index;

			cached[which] = NONE;
			for (index = 0; model && index < model->nodes.count && cached[which] == NONE; index++)
			{
				if (skate_text_has(TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name,
					names[which], TRUE))
				{
					cached[which] = index;
				}
			}
		}
	}
	memcpy(nodes, cached, sizeof(cached));
}

/* another machine's skater (skate_network.c poses them from their state): their
body as a capsule from between their feet to their head, as posed, and their
board as a box where their state has it, both moving at their unit's
velocity. FALSE when their biped's nodes are not the cyborg's (their pill is
taken instead) */
static boolean skate_skater_obstacles(long object_index, real_point3d const *skater, struct skate_obstacles *kept)
{
	struct object_datum *object = object_get(object_index);
	long player_index = ((struct unit_datum *)object)->unit.player_index;
	real_matrix4x3 const *node_matrices = (real_matrix4x3 const *)object_header_block_get(object_index,
		&object->object.node_matrices);
	int node_count = object->object.node_matrices.size / (int)sizeof(real_matrix4x3);
	struct halo_skate_obstacle obstacle;
	real_point3d feet, head;
	real_vector3d along;
	real length;
	short nodes[3];
	real placement[7];
	long board_unit_index;

	skate_skater_nodes(object->object.definition_index, nodes);
	if (nodes[0] == NONE || nodes[1] == NONE || nodes[2] == NONE ||
		MAX(nodes[0], MAX(nodes[1], nodes[2])) >= node_count)
	{
		return FALSE;
	}
	head = node_matrices[nodes[0]].position;
	feet.x = (node_matrices[nodes[1]].position.x + node_matrices[nodes[2]].position.x) / 2.f;
	feet.y = (node_matrices[nodes[1]].position.y + node_matrices[nodes[2]].position.y) / 2.f;
	feet.z = (node_matrices[nodes[1]].position.z + node_matrices[nodes[2]].position.z) / 2.f;
	along.i = head.x - feet.x;
	along.j = head.y - feet.y;
	along.k = head.z - feet.z;
	length = (real)sqrt(along.i * along.i + along.j * along.j + along.k * along.k);
	if (!(length > 0.01f && length < 2.f))
		return FALSE;
	memset(&obstacle, 0, sizeof(obstacle));
	obstacle.kind = HALO_SKATE_OBSTACLE_SKATER;
	obstacle.shape = HALO_SKATE_OBSTACLE_CAPSULE;
	obstacle.center[0] = (head.x + feet.x) / 2.f;
	obstacle.center[1] = (head.y + feet.y) / 2.f;
	obstacle.center[2] = (head.z + feet.z) / 2.f;
	obstacle.forward[0] = 1.f;
	obstacle.left[1] = 1.f;
	obstacle.up[0] = along.i / length;
	obstacle.up[1] = along.j / length;
	obstacle.up[2] = along.k / length;
	obstacle.half_extents[0] = SKATE_SKATER_RADIUS;
	obstacle.half_extents[2] = length / 2.f + SKATE_SKATER_RADIUS;
	skate_obstacle_velocity(object, &obstacle);
	skate_obstacle_speed_limit(&obstacle, SKATE_MAXIMUM_SKATER_OBSTACLE_SPEED);
	skate_obstacle_keep(kept, &obstacle, skate_distance(skater, obstacle.center) - obstacle.half_extents[2]);
	/* (the board drawn last tick: halo_skate_board_obstacle sizes it) */
	if (player_index != NONE &&
		skate_network_board((short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), 1.f, &board_unit_index, placement) &&
		board_unit_index == object_index && halo_skate_board_obstacle(placement, &obstacle) == 0)
	{
		real largest = MAX(obstacle.half_extents[0], MAX(obstacle.half_extents[1], obstacle.half_extents[2]));

		skate_obstacle_velocity(object, &obstacle);
		skate_obstacle_speed_limit(&obstacle, SKATE_MAXIMUM_SKATER_OBSTACLE_SPEED);
		skate_obstacle_keep(kept, &obstacle, skate_distance(skater, obstacle.center) - largest);
	}
	return TRUE;
}

/* the obstacles around the skater's biped unit_index, to the engine */
static void skate_send_obstacles(long unit_index)
{
	struct object_datum *unit = object_get(unit_index);
	long object_indices[SKATE_OBSTACLE_SEARCH];
	struct skate_obstacles kept;
	real_point3d skater;
	short count;
	short index;

	kept.count = 0;
	object_get_origin(unit_index, &skater);
	/* (collideable objects only: class bit 0) */
	count = objects_in_sphere(FLAG(0),
		_object_mask_biped | _object_mask_vehicle | _object_mask_scenery | _object_mask_machine | _object_mask_control,
		&unit->object.location, &skater, SKATE_OBSTACLE_RADIUS, object_indices, NUMBEROF(object_indices));
	for (index = 0; index < count; index++)
	{
		long object_index = object_indices[index];
		struct object_datum *object = object_get(object_index);

		/* (not the skater, nor what rides or is carried: a vehicle's riders
		are inside its boxes) */
		if (object_index == unit_index || object->object.parent_object_index != NONE)
			continue;
		if (object->object.type == _object_type_biped)
		{
			/* (another machine's skater as their body and board, else as
			Halo's pill) */
			if (!(object->object.damage_flags & FLAG(_object_dead_bit)) &&
				!(skate_network_unit_skating(object_index) && skate_skater_obstacles(object_index, &skater, &kept)))
			{
				skate_biped_obstacle(object_index, &skater, &kept);
			}
		}
		/* (static scenery is part of the level: skate_add_scenery) */
		else if (!(object->object.type == _object_type_scenery && skate_scenery_baked(object_index)))
			skate_box_obstacles(object_index, &skater, &kept);
	}
	halo_skate_set_obstacles(kept.obstacles, kept.count);
	/* (the network tests, debug.network_test_skate and _duel, tell what is
	near once a second; the engine tells of each hit: "halo-skate: hit a biped
	at ...") */
	if (game_time_get() % TICKS_PER_SECOND == 0 &&
		(config_real("debug.network_test_skate") > 0. || config_real("debug.network_test_skate_duel") > 0.))
	{
		static char const *const names[4] = { "biped", "vehicle", "scenery", "skater" };
		int kinds[4] = { 0, 0, 0, 0 };

		for (index = 0; index < kept.count; index++)
			kinds[kept.obstacles[index].kind]++;
		if (kept.count)
		{
			platform_log("skate test: %d obstacles near (%d bipeds, %d of skaters, %d vehicle boxes, %d scenery "
				"boxes), the nearest a %s %.2f world units away", kept.count, kinds[0], kinds[3], kinds[1], kinds[2],
				names[kept.obstacles[0].kind], kept.distances[0]);
		}
		else
			platform_log("skate test: no obstacles near");
	}
}

void skate_update_before_objects(void)
{
	long unit_index = player_control_get_unit_index(0);
	struct halo_skate_pad pad;
	boolean toggle;
	int state;
	int stepped;

	memset(&pad, 0, sizeof(pad));
	halo_skate_platform_pad(&pad);
	toggle = halo_skate_platform_toggle_pressed() ||
		((pad.buttons & SKATE_THUMB_CLICKS) == SKATE_THUMB_CLICKS &&
		(skate_globals.previous_buttons & SKATE_THUMB_CLICKS) != SKATE_THUMB_CLICKS);
	skate_globals.previous_buttons = pad.buttons;
	skate_test_pad(&pad, &toggle);
	/* (other machines' skaters carried on: skate_network.c) */
	skate_network_update_before_objects();
	/* the structure bsp loaded since the last tick, with its scenery placed */
	if (skate_globals.map_pending && !skate_load_map())
		skate_globals.activate_when_ready = FALSE;

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
		skate_engine_failed();
		return;
	}
	if (skate_globals.activate_when_ready && state == 2)
	{
		skate_globals.activate_when_ready = FALSE;
		if (skate_unit_can_skate(unit_index))
			skate_start(unit_index);
		return;
	}
	if (!skate_globals.skating)
		return;
	/* (the network test's duel: on the board in another skater's way) */
	{
		real_point3d spot;
		real yaw;

		if (skate_network_block_spot(skate_globals.unit_index, TRUE, &spot, &yaw))
			skate_put(skate_globals.unit_index, &spot, yaw);
	}
	skate_send_obstacles(skate_globals.unit_index);
	stepped =halo_skate_step(&pad, 1.f / TICKS_PER_SECOND, &skate_globals.frame);
	if (stepped == 2)
	{
		/* a step failed (a bail the engine could not follow): the engine put
		the skater back on the board where it last was, as J would; the
		camera turns from where it was rather than snapping round */
		console_printf(FALSE, "skate: thrown, back on the board");
		skate_board_forget();
		skate_apply_frame(skate_globals.unit_index, FALSE);
	}
	else if (stepped >= 0)
		skate_apply_frame(skate_globals.unit_index, FALSE);
	else if (halo_skate_state() == -1)
		skate_engine_failed();
}

#endif

/* ---------- the skater's weapon, holstered
(Also another machine's skater's, posed from their state: skate_network.c,
in a build without the mode too.)

The weapon hangs from the biped's hand node, placed during objects_update by
Halo's own animation, before the skater's pose replaces the biped's nodes.
Once the pose is in it is placed again, each tick: a rifle (and anything
two-handed or large) across the back, on bip01 spine1; a pistol down the
right thigh, on bip01 r thigh; and what has no place there (a flag, a ball,
anything huge) is drawn at next to no size. A spot is given in the model's
bind pose (x forward, y left, z up, world units), and goes with the posed
node as the node's skin does: the posed node times its default inverse
times the spot, whatever the axes of the model's nodes. The weapon's own
position and vectors (its hold in the hand) are set for
object_compute_node_matrices and put back right after, so that Halo finds it
in the hand again when the skater gets off. */

#define SKATE_BACK_NODE_NAME "bip01 spine1"
#define SKATE_THIGH_NODE_NAME "bip01 r thigh"
/* the weapon's middle from the node, in the bind pose: behind the back and
a little lower */
#define SKATE_BACK_OFFSET_FORWARD (-0.085f)
#define SKATE_BACK_OFFSET_UP (-0.02f)
/* the barrel tilted from upright toward the right shoulder, radians */
#define SKATE_BACK_TILT 0.52f
/* outside the right thigh, partway down it */
#define SKATE_THIGH_OFFSET_LEFT (-0.05f)
#define SKATE_THIGH_OFFSET_UP (-0.08f)
/* a bounding radius (world units) larger than any holster takes */
#define SKATE_HOLSTER_MAXIMUM_RADIUS 1.f
/* the scale a weapon with no holster is drawn at: too small to see, not 0,
which the matrices' inverses would divide by */
#define SKATE_HIDDEN_SCALE 0.001f

enum skate_holster
{
	_skate_holster_back = 0,
	_skate_holster_thigh,
	_skate_holster_hidden
};

/* whether text has word in it (equals it, if whole), case aside */
static boolean skate_text_has(char const *text, char const *word, boolean whole)
{
	size_t length = strlen(word);

	if (!text || !length)
		return FALSE;
	for (; *text; text++)
	{
		size_t index = 0;

		while (index < length && text[index] &&
			tolower((unsigned char)text[index]) == tolower((unsigned char)word[index]))
		{
			index++;
		}
		if (index == length && (!whole || text[index] == 0))
			return TRUE;
		if (whole)
			break;
	}
	return FALSE;
}

/* a pistol is what Halo holds as one: its animation label (the weapon class
the biped's animations hold it by) or its tag's name says so. A flag or a
ball, by label or name, or a weapon larger than a holster, has none */
static short skate_weapon_holster(long weapon_index)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	struct weapon_definition *definition = weapon_definition_get(weapon->definition_index);
	char const *name = tag_get_name(weapon->definition_index);
	char label[sizeof(definition->weapon.label) + 1];

	memcpy(label, definition->weapon.label, sizeof(definition->weapon.label));
	label[sizeof(label) - 1] = 0;
	if (definition->object.bounding_radius > SKATE_HOLSTER_MAXIMUM_RADIUS ||
		skate_text_has(label, "flag", FALSE) || skate_text_has(label, "ball", FALSE) ||
		skate_text_has(name, "flag", FALSE) || skate_text_has(name, "ball", FALSE))
	{
		return _skate_holster_hidden;
	}
	if (skate_text_has(label, "pistol", FALSE) || skate_text_has(name, "pistol", FALSE))
		return _skate_holster_thigh;
	return _skate_holster_back;
}

/* the holster's node and its spot in the model's bind pose; FALSE if the
model has no such node */
static boolean skate_holster_spot(struct model *model, short holster, short *node_index,
	real_matrix4x3 *spot)
{
	char const *node_name = holster == _skate_holster_thigh ? SKATE_THIGH_NODE_NAME : SKATE_BACK_NODE_NAME;
	real_matrix4x3 bind;
	real_point3d position;
	real_vector3d forward, up;
	short index;

	*node_index = NONE;
	for (index = 0; index < model->nodes.count && *node_index == NONE; index++)
	{
		if (skate_text_has(TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name, node_name, TRUE))
			*node_index = index;
	}
	if (*node_index == NONE)
		return FALSE;
	matrix4x3_inverse(&TAG_BLOCK_GET_ELEMENT(&model->nodes, *node_index,
		struct model_node)->runtime_default_inverse_matrix, &bind);
	position = bind.position;
	if (holster == _skate_holster_thigh)
	{
		/* the barrel down, the grip back, flat against the leg */
		position.y += SKATE_THIGH_OFFSET_LEFT;
		position.z += SKATE_THIGH_OFFSET_UP;
		forward.i = 0.f; forward.j = 0.f; forward.k = -1.f;
		up.i = 1.f; up.j = 0.f; up.k = 0.f;
	}
	else
	{
		/* the barrel up over the right shoulder, the sights facing out */
		position.x += SKATE_BACK_OFFSET_FORWARD;
		position.z += SKATE_BACK_OFFSET_UP;
		forward.i = 0.f; forward.j = -(real)sin(SKATE_BACK_TILT); forward.k = (real)cos(SKATE_BACK_TILT);
		up.i = -1.f; up.j = 0.f; up.k = 0.f;
	}
	matrix4x3_from_point_and_vectors(spot, &position, &forward, &up);
	return TRUE;
}

/* an object and its children drawn at next to no size */
static void skate_shrink_object(long object_index)
{
	struct object_datum *object = object_get(object_index);
	real_matrix4x3 *node_matrices = (real_matrix4x3 *)object_header_block_get(object_index,
		&object->object.node_matrices);
	int node_count = object->object.node_matrices.size / (int)sizeof(real_matrix4x3);
	long child_index;
	int index;

	for (index = 0; index < node_count; index++)
		node_matrices[index].scale = SKATE_HIDDEN_SCALE;
	for (child_index = object->object.first_child_object_index; child_index != NONE;
		child_index = object_get(child_index)->object.next_object_index)
	{
		skate_shrink_object(child_index);
	}
}

/* the unit's weapon holstered on its posed nodes (skate_update_after_objects) */
void skate_holster_weapon(long unit_index, real_matrix4x3 const *node_matrices, int node_count)
{
	struct unit_datum *unit = unit_get(unit_index);
	long weapon_index = unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index);
	struct object_definition *unit_definition = object_definition_get(unit->definition_index);
	struct weapon_datum *weapon;
	struct model *model;
	real_matrix4x3 spot, skin, target, parent, inverse_parent, local;
	real_point3d hold_position;
	real_vector3d hold_forward, hold_up, offset;
	real parent_scale;
	short holster;
	short node_index = NONE;

	weapon = weapon_index != NONE ? weapon_try_and_get(weapon_index) : NULL;
	if (!weapon || weapon->object.parent_object_index != unit_index ||
		TEST_FLAG(weapon->object.flags, _object_invisible_bit) ||
		weapon->object.parent_node_index < 0 || weapon->object.parent_node_index >= node_count ||
		unit_definition->object.model.index == NONE)
	{
		return;
	}
	model = model_definition_get(unit_definition->object.model.index);
	holster = skate_weapon_holster(weapon_index);
	if (holster != _skate_holster_hidden &&
		(!skate_holster_spot(model, holster, &node_index, &spot) || node_index >= node_count))
	{
		holster = _skate_holster_hidden;
	}
	/* the hand it hangs from, unscaled, as object_compute_node_matrices takes it */
	parent = node_matrices[weapon->object.parent_node_index];
	parent_scale = parent.scale;
	if (holster == _skate_holster_hidden || !(parent_scale > 0.f))
	{
		skate_shrink_object(weapon_index);
		return;
	}
	parent.scale = 1.f;

	/* where the weapon's middle goes, in the world */
	matrix4x3_multiply(&node_matrices[node_index], &TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index,
		struct model_node)->runtime_default_inverse_matrix, &skin);
	matrix4x3_multiply(&skin, &spot, &target);
	/* and in the hand's frame */
	matrix4x3_inverse(&parent, &inverse_parent);
	matrix4x3_multiply(&inverse_parent, &target, &local);

	hold_position = weapon->object.position;
	hold_forward = weapon->object.forward;
	hold_up = weapon->object.up;
	weapon->object.position.x = local.position.x / parent_scale;
	weapon->object.position.y = local.position.y / parent_scale;
	weapon->object.position.z = local.position.z / parent_scale;
	weapon->object.forward = local.forward;
	weapon->object.up = local.up;
	normalize3d(&weapon->object.forward);
	normalize3d(&weapon->object.up);
	object_compute_node_matrices(weapon_index);
	/* moved so that its bounding sphere's middle, not its origin, is on the
	spot, then placed again with its children */
	offset.i = target.position.x - weapon->object.bounding_sphere_center.x;
	offset.j = target.position.y - weapon->object.bounding_sphere_center.y;
	offset.k = target.position.z - weapon->object.bounding_sphere_center.z;
	matrix4x3_transform_normal(&inverse_parent, &offset, &offset);
	weapon->object.position.x += offset.i / parent_scale;
	weapon->object.position.y += offset.j / parent_scale;
	weapon->object.position.z += offset.k / parent_scale;
	object_compute_node_matrices_recursive(weapon_index);
	weapon->object.position = hold_position;
	weapon->object.forward = hold_forward;
	weapon->object.up = hold_up;
}

#ifdef HALO_SKATE

void skate_update_after_objects(void)
{
	real_matrix4x3 matrices[SKATE_MAXIMUM_NODES];
	struct object_datum *object;
	real_matrix4x3 *node_matrices;
	int node_count;
	int written;

	short player_index;

	skate_sound_update(skate_globals.skating && skate_unit_can_skate(skate_globals.unit_index), skate_globals.unit_index,
		skate_globals.frame.position);
	/* other machines' skaters posed, and their boards lit (skate_network.c) */
	skate_network_update_after_objects();
	if (skate_board_ready())
	{
		for (player_index = 0; player_index < SKATE_NETWORK_PLAYERS; player_index++)
		{
			real placement[7];
			long unit_index;

			if (skate_network_board(player_index, 1.f, &unit_index, placement))
				skate_board_light(unit_index, skate_remote_lights[player_index]);
		}
	}
	if (!skate_globals.skating || !skate_unit_can_skate(skate_globals.unit_index))
	{
		skate_network_note_local(local_player_get_player_index(0), NONE, FALSE, NULL);
		skate_board_forget();
		return;
	}
	skate_board_capture(skate_globals.unit_index);
	object = object_get(skate_globals.unit_index);
	node_count = object->object.node_matrices.size / (int)sizeof(real_matrix4x3);
	if (node_count <= 0 || node_count > SKATE_MAXIMUM_NODES)
		return;
	node_matrices = (real_matrix4x3 *)object_header_block_get(skate_globals.unit_index,
		&object->object.node_matrices);
	written = halo_skate_pose_nodes(&matrices[0].scale, node_count);
	if (written != node_count && !skate_globals.skeleton_described_again)
	{
		/* the engine lost the skeleton (it never should: lib.rs keeps it
		across restarts): told again, rather than the biped left standing on
		the board in Halo's own pose */
		platform_log("skate: the skate engine posed %d of %d nodes: describing the skeleton again", written,
			node_count);
		skate_globals.skeleton_definition_index = NONE;
		skate_globals.skeleton_described_again = TRUE;
		skate_describe_skeleton(skate_globals.unit_index);
		written = halo_skate_pose_nodes(&matrices[0].scale, node_count);
	}
	if (written == node_count)
		memcpy(node_matrices, matrices, sizeof(real_matrix4x3) * node_count);
	/* on a board the weapon is never in hand, posed or not */
	skate_holster_weapon(skate_globals.unit_index, node_matrices, node_count);
	/* and the others told, in a network game (skate_network.c) */
	{
		float placement[7];

		skate_network_note_local(local_player_get_player_index(0), skate_globals.unit_index,
			(boolean)(skate_globals.frame.off_board != 0),
			halo_skate_board_placement(placement) == 0 ? placement : NULL);
	}
}

boolean skate_unit_is_skating(long unit_index)
{
	return (skate_globals.skating && unit_index != NONE && unit_index == skate_globals.unit_index) ||
		skate_network_unit_skating(unit_index);
}

boolean skate_local_player_skating(short local_player_index, real *yaw)
{
	if (local_player_index != 0 || !skate_globals.skating)
		return FALSE;
	if (yaw)
		*yaw = skate_globals.camera.heading;
	return TRUE;
}

/* ---------- tuning at the console

"skate_board_scale 1.1" sets a value for the session; the word alone prints
it (hs.c hands both over before the script compiler sees them). */

/* the number in what follows a command's word (spaces, then the number, then
spaces or a closing parenthesis): TRUE with *value; *given says whether
anything but those was there at all */
static boolean skate_console_number(char const *text, real *value, boolean *given)
{
	char *end;
	double number;

	while (*text == ' ' || *text == '\t')
		text++;
	*given = *text != 0 && *text != ')';
	if (!*given)
		return FALSE;
	number = strtod(text, &end);
	if (end == text)
		return FALSE;
	while (*end == ' ' || *end == '\t' || *end == ')')
		end++;
	/* (and a number: not NaN, nor infinite) */
	if (*end || !(number > -1e30 && number < 1e30))
		return FALSE;
	*value = (real)number;
	return TRUE;
}

boolean skate_console_setting(char const *word, char const *arguments)
{
	real value = 0.f;
	boolean given;
	boolean valid = skate_console_number(arguments, &value, &given);

	if (!strcmp(word, "skate_board_scale"))
	{
		if (given && !(valid && value > 0.f && value <= SKATE_MAXIMUM_BOARD_SCALE))
		{
			console_printf(FALSE, "skate_board_scale: a factor above 0, at most %.0f (skate_board_scale 1.05)",
				SKATE_MAXIMUM_BOARD_SCALE);
			return FALSE;
		}
		if (given)
			skate_settings.board_scale = halo_skate_set_board_scale(value);
		console_printf(FALSE, "skate_board_scale %.3f (default %.3f)", skate_settings.board_scale,
			SKATE_DEFAULT_BOARD_SCALE);
		return TRUE;
	}
	if (!strcmp(word, "skate_feet_offset"))
	{
		if (given && !(valid && fabs(value) <= SKATE_MAXIMUM_FEET_OFFSET))
		{
			console_printf(FALSE, "skate_feet_offset: world units from -%.0f to %.0f, up positive "
				"(skate_feet_offset 0.01)", SKATE_MAXIMUM_FEET_OFFSET, SKATE_MAXIMUM_FEET_OFFSET);
			return FALSE;
		}
		if (given)
		{
			skate_settings.feet_offset = value;
			halo_skate_set_feet(skate_settings.ankle_height, skate_settings.feet_offset);
		}
		console_printf(FALSE, "skate_feet_offset %.4f world units, %.1f cm (default 0; ankles %.4f above the soles)",
			skate_settings.feet_offset, skate_settings.feet_offset * 304.8f, skate_settings.ankle_height);
		return TRUE;
	}
	if (!strcmp(word, "skate_camera_speed"))
	{
		if (given && !(valid && value >= 0.f && value <= SKATE_MAXIMUM_CAMERA_SPEED))
		{
			console_printf(FALSE, "skate_camera_speed: world units a second from 0 to %.0f (skate_camera_speed 0.33)",
				SKATE_MAXIMUM_CAMERA_SPEED);
			return FALSE;
		}
		if (given)
			skate_settings.camera_speed = value;
		console_printf(FALSE, "skate_camera_speed %.3f world units a second, %.2f m/s (default %.3f)",
			skate_settings.camera_speed, skate_settings.camera_speed * 3.048f, SKATE_DEFAULT_CAMERA_SPEED);
		return TRUE;
	}
	if (!strcmp(word, "skate_obstacle_bail_speed"))
	{
		if (given && !(valid && value >= 0.f && value <= SKATE_MAXIMUM_OBSTACLE_BAIL_SPEED))
		{
			console_printf(FALSE, "skate_obstacle_bail_speed: world units a second from 0 to %.0f "
				"(skate_obstacle_bail_speed 2)", SKATE_MAXIMUM_OBSTACLE_BAIL_SPEED);
			return FALSE;
		}
		if (given)
			skate_settings.obstacle_bail_speed = halo_skate_set_obstacle_bail_speed(value);
		console_printf(FALSE, "skate_obstacle_bail_speed %.3f world units a second, %.2f m/s (default %.3f)",
			skate_settings.obstacle_bail_speed, skate_settings.obstacle_bail_speed * 3.048f,
			SKATE_DEFAULT_OBSTACLE_BAIL_SPEED);
		return TRUE;
	}
	/* skate_volume and skate_sound_log (skate_sound.c) */
	{
		int taken = skate_sound_console_setting(word, value, valid, given);

		if (taken >= 0)
			return (boolean)taken;
	}
	return FALSE;
}

#else

boolean skate_console_setting(char const *word, char const *arguments)
{
	console_printf(FALSE, "%s: this build has no Skate 3 mode (it was built without --skate)", word);
	return FALSE;
}

void skate_initialize(void)
{
}

void skate_structure_bsp_changed(void)
{
}

/* (another machine's skater is still posed, without the board) */
void skate_update_before_objects(void)
{
	skate_network_update_before_objects();
}

void skate_update_after_objects(void)
{
	skate_network_update_after_objects();
}

void skate_render_board(void)
{
}

boolean skate_unit_is_skating(long unit_index)
{
	return skate_network_unit_skating(unit_index);
}

boolean skate_local_player_skating(short local_player_index, real *yaw)
{
	return FALSE;
}

#endif
