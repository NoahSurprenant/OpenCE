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
#include "physics/collisions.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "skate.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef HALO_SKATE

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
	real yaw;
	struct halo_skate_frame frame;
} skate_globals = { FALSE, FALSE, FALSE, NONE, NONE, NONE, NONE, 0, 0.f, 0.f };

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

/* ---------- the board

The skate engine skins the board each tick (halo_skate_board_vertices); a
frame draws it between the last two ticks, as the biped's nodes are
(render_interpolation.c), so that it stays under the skater's feet. */

static void skate_board_free(void)
{
	free(skate_board.indices);
	free(skate_board.surfaces);
	free(skate_board.vertices[0]);
	free(skate_board.vertices[1]);
	free(skate_board.drawn);
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
static void skate_board_light(long unit_index)
{
	struct render_lighting lighting;
	short light_index;
	real total;

	memset(&lighting, 0, sizeof(lighting));
	lights_prepare_for_object_static(unit_index, &lighting);
	memset(skate_board.lights, 0, sizeof(skate_board.lights));
	skate_board.lights[0][0] = lighting.ambient_color.red;
	skate_board.lights[0][1] = lighting.ambient_color.green;
	skate_board.lights[0][2] = lighting.ambient_color.blue;
	total = lighting.ambient_color.red + lighting.ambient_color.green + lighting.ambient_color.blue;
	for (light_index = 0; light_index < lighting.distant_light_count && light_index < MAXIMUM_RENDERED_DISTANT_LIGHTS;
		light_index++)
	{
		struct render_distant_light const *light = &lighting.distant_lights[light_index];

		skate_board.lights[1 + 2 * light_index][0] = light->direction.i;
		skate_board.lights[1 + 2 * light_index][1] = light->direction.j;
		skate_board.lights[1 + 2 * light_index][2] = light->direction.k;
		skate_board.lights[2 + 2 * light_index][0] = light->color.red;
		skate_board.lights[2 + 2 * light_index][1] = light->color.green;
		skate_board.lights[2 + 2 * light_index][2] = light->color.blue;
		total += light->color.red + light->color.green + light->color.blue;
	}
	/* (no light found, or none that is a number: grey, lit from above,
	rather than black) */
	if (!(total > 0.f))
	{
		memset(skate_board.lights, 0, sizeof(skate_board.lights));
		skate_board.lights[0][0] = skate_board.lights[0][1] = skate_board.lights[0][2] = 0.4f;
		skate_board.lights[1][2] = -1.f;
		skate_board.lights[2][0] = skate_board.lights[2][1] = skate_board.lights[2][2] = 0.6f;
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
	skate_board_light(unit_index);
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

	if (!skate_globals.skating || !skate_board.has_latest || !skate_board.vertex_count)
		return;
	if (skate_board.uploaded_generation != skate_board.generation)
		skate_board_upload_textures();
	latest = skate_board.vertices[skate_board.latest];
	drawn = latest;
	fraction = render_interpolation_fraction();
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

/* ---------- the skater's weapon, holstered

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
static void skate_holster_weapon(long unit_index, real_matrix4x3 const *node_matrices, int node_count)
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

void skate_update_after_objects(void)
{
	real_matrix4x3 matrices[SKATE_MAXIMUM_NODES];
	struct object_datum *object;
	real_matrix4x3 *node_matrices;
	int node_count;
	int written;

	if (!skate_globals.skating || !skate_unit_can_skate(skate_globals.unit_index))
	{
		skate_board_forget();
		return;
	}
	skate_board_capture(skate_globals.unit_index);
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
	skate_holster_weapon(skate_globals.unit_index, node_matrices, node_count);
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

void skate_render_board(void)
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
