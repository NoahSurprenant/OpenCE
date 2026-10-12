/*
LONG_NOSE_CHIEF.C

The long-nose Master Chief (game.long_nose_chief, off unless set, or the
console's long_nose_chief): when a map's tags load (scenario_tags_load), the
cyborg's model (characters\cyborg\cyborg) gets a long gold nose out of its
visor and a toothy smile under it, and its visor turns golden orange. Only
what is drawn changes: the model's nodes, markers, collision and everything
played or sent between machines stay the map's, so it can be on for one
player and off for another in the same game.

The pieces are made by code (long_nose_chief_geometry.c), not read from any
file: each of the model's geometries (one for each level of detail) gets two
parts added after its own, the nose with the visor's shader and the smile
with the armor's, their vertices against that geometry's own helmet. Every
vertex is the head node's alone, so the pieces go wherever the head does:
in every animation, cutscene and pose (Skate 3 mode's too, which poses the
nodes), and on every player. The farthest level of detail, which has the
armor alone, gets a nose of fewer sides and no smile. The vertices are
compressed by the game's own rasterizer_geometry_compress_vertices and get
buffers from rasterizer_vertex_buffer_new and rasterizer_triangle_buffer_new,
as a Custom Edition map's are (custom_edition_geometry.c). The parts are no
transparent part's neighbours (no sorting links).

The geometry's parts are a copy with the new ones at its end, and the visor
shader's perpendicular and parallel tints are written over (its gold is its
reflection cube map times those). Both come back as they were, and the
copies and buffers are freed, when the map's tags go (before anything else
of the map's is let go), or when long_nose_chief false is typed. A model
that is not as expected (no head node, no visor or armor shader, a geometry
already holding the most parts there can be) is left as it is.
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "main/console.h"
#include "math/real_math.h"
#include "models/model_definitions.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "shaders/shader_definitions.h"
#include "tag_files/tag_groups.h"
#include "long_nose_chief_geometry.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <xtl.h>

/* ---------- constants */

#define LONG_NOSE_CHIEF_MODEL "characters\\cyborg\\cyborg"
#define LONG_NOSE_CHIEF_HEAD_NODE "bip01 head"

enum
{
	NUMBER_OF_DETAIL_LEVELS_PER_MODEL = 5,
	/* the farthest level of detail (models.c draws a geometry_indices[4]
	near and [0] far): a nose of fewer sides, and no smile */
	FAR_DETAIL_LEVEL = 0,
	_model_geometry_part_stripped_bit = 0,
	_shader_type_model = 4,
};

/* the visor golden orange: the design's (0.95, 0.63, 0.16), tuned in the game
(Blood Gulch, Hang 'Em High) to a lighter gold seen straight on and a deeper
orange at its edges, which keeps the visor's and the nose's shape */
static real_rgb_color const long_nose_visor_perpendicular_tint = { 1.f, 0.72f, 0.22f };
static real_rgb_color const long_nose_visor_parallel_tint = { 0.62f, 0.38f, 0.08f };

/* ---------- structures */

/* this build's model geometry, part and shader reference, as models.c
defines them for itself (no header declares them) */
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

struct model_shader_reference
{
	struct tag_reference shader;
	short permutation_index;
	word pad;
	long unused[3];
};

/* the reflection of a model shader (rasterizer_xbox_models.c) */
struct shader_model_reflection
{
	struct shader shader;
	byte reserved[0x144 - sizeof(struct shader)];
	real perpendicular_brightness;
	real_rgb_color perpendicular_tint_color;
	real parallel_brightness;
	real_rgb_color parallel_tint_color;
};

typedef char verify_long_nose_model_geometry_part_size[sizeof(struct model_geometry_part) == 0x68 ? 1 : -1];
typedef char verify_long_nose_model_shader_reference_size[sizeof(struct model_shader_reference) == 0x20 ? 1 : -1];
typedef char verify_long_nose_shader_model_perpendicular_tint_offset[
	offsetof(struct shader_model_reflection, perpendicular_tint_color) == 0x148 ? 1 : -1];
typedef char verify_long_nose_shader_model_parallel_tint_offset[
	offsetof(struct shader_model_reflection, parallel_tint_color) == 0x158 ? 1 : -1];

/* a part added: its vertices and strip, which its buffers keep pointing at */
struct long_nose_part
{
	struct model_vertex_compressed *vertices;
	word *strip;
};

/* a geometry given parts: its own parts block as it was, and the copy */
struct long_nose_geometry
{
	struct model_geometry *geometry;
	struct tag_block original_parts;
	struct model_geometry_part *parts;
	short added_count;
	struct long_nose_part added[NUMBER_OF_LONG_NOSE_PIECES];
};

/* a geometry's triangles, in the bind pose's model space, for finding the
helmet's front */
struct long_nose_surface
{
	long triangle_count;
	real_point3d (*triangles)[3];
};

/* ---------- globals */

static struct
{
	boolean map_loaded;
	/* the console's long_nose_chief, for the rest of the session: NONE
	until typed (config.toml's then) */
	short console_setting;
	boolean applied;
	struct shader_model_reflection *visor;
	real_rgb_color visor_perpendicular_tint;
	real_rgb_color visor_parallel_tint;
	short geometry_count;
	struct long_nose_geometry geometries[MAXIMUM_GEOMETRIES_PER_MODEL];
} long_nose_chief_globals = { FALSE, NONE };

/* (large, so not on the stack) */
static struct long_nose_mesh long_nose_mesh;
static struct model_vertex_uncompressed long_nose_uncompressed[LONG_NOSE_MAXIMUM_VERTICES];

/* ---------- prototypes */

int config_boolean(char const *name);
char *tag_get_name(long tag_index);

/* ---------- private code */

static boolean long_nose_chief_wanted(
	void)
{
	if (long_nose_chief_globals.console_setting != NONE)
		return (boolean)long_nose_chief_globals.console_setting;

	return config_boolean("game.long_nose_chief") != 0;
}

/* whether the part's buffers can be read: a vertex buffer of model
vertices from its start, and a strip or triangle list */
static boolean part_readable(
	struct model_geometry_part const *part)
{
	struct vertex_buffer const *vertices = &part->vertex_buffer;
	struct triangle_buffer const *triangles = &part->triangle_buffer;

	return !TEST_FLAG(part->flags, _model_geometry_part_stripped_bit) &&
		vertices->hardware_format && !vertices->offset &&
		vertices->count > 0 && vertices->count <= MAXIMUM_VERTICES_PER_MODEL_GEOMETRY_PART &&
		(vertices->type == _rasterizer_vertex_type_model_compressed ||
			vertices->type == _rasterizer_vertex_type_model_uncompressed) &&
		triangles->hardware_format && triangles->count > 0 &&
		triangles->count <= MAXIMUM_TRIANGLES_PER_MODEL_GEOMETRY_PART &&
		(triangles->type == _triangle_buffer_type_precompiled_strip ||
			triangles->type == _triangle_buffer_type_triangles);
}

/* the part's triangle's corners' indices (of a strip, in no particular
winding); FALSE for one of no area or past the part's vertices */
static boolean part_triangle(
	struct model_geometry_part const *part,
	word const *indices,
	long triangle_index,
	word *corners)
{
	if (part->triangle_buffer.type == _triangle_buffer_type_precompiled_strip)
		indices += triangle_index;
	else
		indices += 3 * triangle_index;
	corners[0] = indices[0];
	corners[1] = indices[1];
	corners[2] = indices[2];

	return corners[0] != corners[1] && corners[1] != corners[2] && corners[0] != corners[2] &&
		corners[0] < part->vertex_buffer.count && corners[1] < part->vertex_buffer.count &&
		corners[2] < part->vertex_buffer.count;
}

/* every triangle of the geometry's parts, into surface; FALSE when there
is none, or no memory */
static boolean surface_new(
	struct model_geometry const *geometry,
	struct long_nose_surface *surface)
{
	long count = 0;
	short pass;

	surface->triangle_count = 0;
	surface->triangles = NULL;
	/* (counted, then read) */
	for (pass = 0; pass < 2; pass++)
	{
		long part_index;

		if (pass)
		{
			if (!count || !(surface->triangles = malloc((size_t)count * sizeof(*surface->triangles))))
				return FALSE;
		}
		for (part_index = 0; part_index < geometry->parts.count; part_index++)
		{
			struct model_geometry_part const *part = TAG_BLOCK_GET_ELEMENT(&geometry->parts, part_index,
				struct model_geometry_part);
			byte *vertices = NULL;
			byte *indices = NULL;
			long stride;
			long triangle_index;

			if (!part_readable(part))
				continue;
			stride = rasterizer_geometry_get_vertex_size(part->vertex_buffer.type);
			IDirect3DVertexBuffer8_Lock(part->vertex_buffer.hardware_format, 0, 0, &vertices, D3DLOCK_READONLY);
			IDirect3DIndexBuffer8_Lock(part->triangle_buffer.hardware_format, 0, 0, &indices, D3DLOCK_READONLY);
			for (triangle_index = 0; vertices && indices && triangle_index < part->triangle_buffer.count;
				triangle_index++)
			{
				word corners[3];
				short corner;

				if (!part_triangle(part, (word const *)indices, triangle_index, corners))
					continue;
				if (pass && surface->triangle_count < count)
				{
					for (corner = 0; corner < 3; corner++)
					{
						surface->triangles[surface->triangle_count][corner] =
							*(real_point3d const *)(vertices + corners[corner] * stride);
					}
					surface->triangle_count++;
				}
				else if (!pass)
				{
					count++;
				}
			}
			if (indices)
				IDirect3DIndexBuffer8_Unlock(part->triangle_buffer.hardware_format);
			if (vertices)
				IDirect3DVertexBuffer8_Unlock(part->vertex_buffer.hardware_format);
		}
	}

	return surface->triangle_count > 0;
}

static void surface_dispose(
	struct long_nose_surface *surface)
{
	if (surface->triangles)
		free(surface->triangles);
	surface->triangles = NULL;
	surface->triangle_count = 0;
}

/* (long_nose_surface_proc) the front-most of the geometry's triangles
that a ray along -x through (y, z) passes */
static int surface_front(
	void *context,
	float y,
	float z,
	float *x)
{
	struct long_nose_surface const *surface = context;
	real front = -FLT_MAX;
	long triangle_index;

	for (triangle_index = 0; triangle_index < surface->triangle_count; triangle_index++)
	{
		real_point3d const *corner = surface->triangles[triangle_index];
		real ay = corner[0].y, az = corner[0].z;
		real by = corner[1].y - ay, bz = corner[1].z - az;
		real cy = corner[2].y - ay, cz = corner[2].z - az;
		real py = y - ay, pz = z - az;
		real area = by * cz - bz * cy;
		real u, v;

		/* (seen edge on from the front) */
		if (fabs(area) < 1e-12f)
			continue;
		u = (py * cz - pz * cy) / area;
		v = (by * pz - bz * py) / area;
		if (u >= 0.f && v >= 0.f && u + v <= 1.f)
		{
			real hit = corner[0].x + u * (corner[1].x - corner[0].x) + v * (corner[2].x - corner[0].x);

			if (hit > front)
				front = hit;
		}
	}
	if (front == -FLT_MAX)
		return 0;
	*x = (float)front;

	return 1;
}

/* the model's shader whose tag's name has `word` in it, if it is of a type
`type` (NONE for any) */
static short model_shader_named(
	struct model const *model,
	char const *word,
	short type)
{
	short shader_index;

	for (shader_index = 0; shader_index < model->shaders.count; shader_index++)
	{
		struct model_shader_reference const *reference = TAG_BLOCK_GET_ELEMENT(&model->shaders, shader_index,
			struct model_shader_reference);
		char const *name;

		if (reference->shader.index == NONE)
			continue;
		name = tag_get_name(reference->shader.index);
		if (name && strstr(name, word) &&
			(type == NONE || shader_definition_get(reference->shader.index)->base.type == type))
		{
			return shader_index;
		}
	}

	return NONE;
}

static short model_node_named(
	struct model const *model,
	char const *name)
{
	short node_index;

	for (node_index = 0; node_index < model->nodes.count; node_index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node);

		if (!strncmp(node->name, name, sizeof(node->name)))
			return node_index;
	}

	return NONE;
}

/* the mesh made a part of the geometry's: compressed, with buffers, all
the head node's; FALSE (and nothing kept) when it cannot be */
static boolean part_new(
	struct model_geometry_part *part,
	struct long_nose_part *added,
	struct long_nose_mesh const *mesh,
	short shader_index,
	short head_node_index,
	real_vector2d const *base_map_scale)
{
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_compressed);
	long uncompressed_vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_uncompressed);
	word strip[LONG_NOSE_MAXIMUM_STRIP_INDICES];
	long strip_index_count;
	real_point3d centroid = { 0.f, 0.f, 0.f };
	long vertex_index;

	memset(part, 0, sizeof(*part));
	memset(added, 0, sizeof(*added));
	/* (the game's front faces are clockwise seen from outside) */
	strip_index_count = long_nose_mesh_strip(mesh, TRUE, strip, NUMBEROF(strip));
	if (!mesh->vertex_count || strip_index_count < 3)
		return FALSE;

	for (vertex_index = 0; vertex_index < mesh->vertex_count; vertex_index++)
	{
		struct long_nose_vertex const *source = &mesh->vertices[vertex_index];
		struct model_vertex_uncompressed *vertex = &long_nose_uncompressed[vertex_index];
		real_vector3d across;

		vertex->position.x = source->position[0];
		vertex->position.y = source->position[1];
		vertex->position.z = source->position[2];
		vertex->normal.i = source->normal[0];
		vertex->normal.j = source->normal[1];
		vertex->normal.k = source->normal[2];
		/* any square pair across the normal: the pieces' textures are flat */
		set_real_vector3d(&across, 0.f, 0.f, 1.f);
		if (fabs(vertex->normal.k) > 0.9f)
			set_real_vector3d(&across, 0.f, 1.f, 0.f);
		cross_product3d(&vertex->normal, &across, &vertex->binormal);
		normalize3d(&vertex->binormal);
		cross_product3d(&vertex->binormal, &vertex->normal, &vertex->tangent);
		normalize3d(&vertex->tangent);
		/* (the model's base map scale multiplies what is stored) */
		vertex->texcoord.x = base_map_scale->i ? source->texcoord[0] / base_map_scale->i : source->texcoord[0];
		vertex->texcoord.y = base_map_scale->j ? source->texcoord[1] / base_map_scale->j : source->texcoord[1];
		vertex->nodes[0] = head_node_index;
		/* (as the map's own vertices of one node are) */
		vertex->nodes[1] = NONE;
		vertex->node_weights[0] = 1.f;
		vertex->node_weights[1] = 0.f;
		centroid.x += vertex->position.x / (real)mesh->vertex_count;
		centroid.y += vertex->position.y / (real)mesh->vertex_count;
		centroid.z += vertex->position.z / (real)mesh->vertex_count;
	}

	added->vertices = malloc((size_t)(mesh->vertex_count * vertex_size));
	added->strip = malloc((size_t)strip_index_count * sizeof(word));
	if (!added->vertices || !added->strip)
	{
		if (added->vertices)
			free(added->vertices);
		if (added->strip)
			free(added->strip);
		memset(added, 0, sizeof(*added));
		return FALSE;
	}
	rasterizer_geometry_compress_vertices(
		_rasterizer_vertex_type_model_uncompressed,
		mesh->vertex_count,
		added->vertices,
		mesh->vertex_count * vertex_size,
		long_nose_uncompressed,
		mesh->vertex_count * uncompressed_vertex_size);
	memcpy(added->strip, strip, (size_t)strip_index_count * sizeof(word));

	part->shader_index = shader_index;
	part->previous_part_index = NONE;
	part->next_part_index = NONE;
	part->centroid_primary_node_index = head_node_index;
	part->centroid_secondary_node_index = head_node_index;
	part->centroid_primary_node_weight = 1.f;
	part->centroid_secondary_node_weight = 0.f;
	part->centroid = centroid;
	if (!rasterizer_vertex_buffer_new(&part->vertex_buffer, _rasterizer_vertex_type_model_compressed,
			mesh->vertex_count, added->vertices, mesh->vertex_count * vertex_size) ||
		!rasterizer_triangle_buffer_new(&part->triangle_buffer, _triangle_buffer_type_precompiled_strip,
			strip_index_count - 2, added->strip))
	{
		rasterizer_vertex_buffer_delete(&part->vertex_buffer);
		free(added->vertices);
		free(added->strip);
		memset(added, 0, sizeof(*added));
		return FALSE;
	}

	return TRUE;
}

static void part_dispose(
	struct model_geometry_part *part,
	struct long_nose_part *added)
{
	rasterizer_triangle_buffer_delete(&part->triangle_buffer);
	rasterizer_vertex_buffer_delete(&part->vertex_buffer);
	if (added->vertices)
		free(added->vertices);
	if (added->strip)
		free(added->strip);
	memset(added, 0, sizeof(*added));
}

/* the geometry's own parts back, and what was added freed */
static void geometry_restore(
	struct long_nose_geometry *patch)
{
	short added_index;

	patch->geometry->parts = patch->original_parts;
	for (added_index = 0; added_index < patch->added_count; added_index++)
	{
		part_dispose(&patch->parts[patch->original_parts.count + added_index], &patch->added[added_index]);
	}
	free(patch->parts);
	memset(patch, 0, sizeof(*patch));
}

/* the pieces added to the geometry, at the detail level (the nearest that
draws it); FALSE when it is left as it was */
static boolean geometry_patch(
	struct long_nose_geometry *patch,
	struct model *model,
	struct model_geometry *geometry,
	short detail_level,
	real_point3d const *head,
	short head_node_index,
	short visor_shader_index,
	short armor_shader_index)
{
	short piece_count = detail_level == FAR_DETAIL_LEVEL ? 1 : NUMBER_OF_LONG_NOSE_PIECES;
	struct long_nose_surface surface;
	float head_position[3];
	short piece;

	memset(patch, 0, sizeof(*patch));
	if (geometry->parts.count < 0 || geometry->parts.count + piece_count > MAXIMUM_PARTS_PER_MODEL_GEOMETRY ||
		(geometry->parts.count && !geometry->parts.address))
	{
		return FALSE;
	}
	if (!surface_new(geometry, &surface))
		return FALSE;
	patch->parts = malloc((size_t)(geometry->parts.count + piece_count) * sizeof(struct model_geometry_part));
	if (!patch->parts)
	{
		surface_dispose(&surface);
		return FALSE;
	}
	patch->geometry = geometry;
	patch->original_parts = geometry->parts;
	if (geometry->parts.count)
	{
		memcpy(patch->parts, geometry->parts.address,
			(size_t)geometry->parts.count * sizeof(struct model_geometry_part));
	}

	head_position[0] = head->x;
	head_position[1] = head->y;
	head_position[2] = head->z;
	for (piece = 0; piece < piece_count; piece++)
	{
		struct model_geometry_part *part = &patch->parts[geometry->parts.count + patch->added_count];

		if (long_nose_mesh_build(&long_nose_mesh, piece,
				detail_level == FAR_DETAIL_LEVEL ? LONG_NOSE_FAR_NOSE_SIDES : LONG_NOSE_NOSE_SIDES,
				head_position, surface_front, &surface) &&
			part_new(part, &patch->added[patch->added_count], &long_nose_mesh,
				piece == _long_nose_piece_nose ? visor_shader_index : armor_shader_index,
				head_node_index, &model->base_map_scale))
		{
			patch->added_count++;
		}
	}
	surface_dispose(&surface);

	if (!patch->added_count)
	{
		free(patch->parts);
		memset(patch, 0, sizeof(*patch));
		return FALSE;
	}
	geometry->parts.address = patch->parts;
	geometry->parts.count = patch->original_parts.count + patch->added_count;

	return TRUE;
}

static void long_nose_chief_restore(
	void)
{
	short geometry_index;

	if (!long_nose_chief_globals.applied)
		return;
	for (geometry_index = 0; geometry_index < long_nose_chief_globals.geometry_count; geometry_index++)
		geometry_restore(&long_nose_chief_globals.geometries[geometry_index]);
	long_nose_chief_globals.geometry_count = 0;
	if (long_nose_chief_globals.visor)
	{
		long_nose_chief_globals.visor->perpendicular_tint_color = long_nose_chief_globals.visor_perpendicular_tint;
		long_nose_chief_globals.visor->parallel_tint_color = long_nose_chief_globals.visor_parallel_tint;
		long_nose_chief_globals.visor = NULL;
	}
	long_nose_chief_globals.applied = FALSE;
}

/* FALSE when the map has no cyborg model as expected (and nothing changes) */
static boolean long_nose_chief_apply(
	void)
{
	long model_index = tag_loaded(MODELS_GROUP_TAG, LONG_NOSE_CHIEF_MODEL);
	struct model *model;
	struct model_shader_reference const *visor_reference;
	real_matrix4x3 head_matrix;
	short head_node_index;
	short visor_shader_index;
	short armor_shader_index;
	short region_index;
	short level;

	if (long_nose_chief_globals.applied)
		return TRUE;
	if (model_index == NONE)
		return FALSE;
	model = model_definition_get(model_index);
	head_node_index = model_node_named(model, LONG_NOSE_CHIEF_HEAD_NODE);
	visor_shader_index = model_shader_named(model, "visor", _shader_type_model);
	armor_shader_index = model_shader_named(model, "armor", NONE);
	if (head_node_index == NONE || visor_shader_index == NONE || armor_shader_index == NONE ||
		model->geometries.count <= 0 || model->geometries.count > MAXIMUM_GEOMETRIES_PER_MODEL ||
		!model->geometries.address)
	{
		error(_error_silent, "long nose chief: the model '%s' has no %s node, or no visor or armor shader",
			LONG_NOSE_CHIEF_MODEL, LONG_NOSE_CHIEF_HEAD_NODE);
		return FALSE;
	}
	/* (the head node's origin in the bind pose) */
	matrix4x3_inverse(&TAG_BLOCK_GET_ELEMENT(&model->nodes, head_node_index, struct model_node)->runtime_default_inverse_matrix,
		&head_matrix);

	/* each geometry a region's permutation draws, once, at the nearest
	detail level it is drawn at */
	long_nose_chief_globals.geometry_count = 0;
	for (level = NUMBER_OF_DETAIL_LEVELS_PER_MODEL - 1; level >= 0; level--)
	{
		for (region_index = 0; region_index < model->regions.count; region_index++)
		{
			struct model_region const *region = TAG_BLOCK_GET_ELEMENT(&model->regions, region_index,
				struct model_region);
			short permutation_index;

			for (permutation_index = 0; permutation_index < region->permutations.count; permutation_index++)
			{
				struct model_region_permutation const *permutation = TAG_BLOCK_GET_ELEMENT(&region->permutations,
					permutation_index, struct model_region_permutation);
				short geometry_index = permutation->geometry_indices[level];
				struct model_geometry *geometry;
				short patched;

				if (!VALID_INDEX(geometry_index, model->geometries.count))
					continue;
				geometry = TAG_BLOCK_GET_ELEMENT(&model->geometries, geometry_index, struct model_geometry);
				for (patched = 0; patched < long_nose_chief_globals.geometry_count; patched++)
				{
					if (long_nose_chief_globals.geometries[patched].geometry == geometry)
						break;
				}
				if (patched < long_nose_chief_globals.geometry_count ||
					long_nose_chief_globals.geometry_count >= MAXIMUM_GEOMETRIES_PER_MODEL)
				{
					continue;
				}
				if (geometry_patch(&long_nose_chief_globals.geometries[long_nose_chief_globals.geometry_count],
						model, geometry, level, &head_matrix.position, head_node_index, visor_shader_index,
						armor_shader_index))
				{
					long_nose_chief_globals.geometry_count++;
				}
			}
		}
	}

	/* the visor golden orange */
	visor_reference = TAG_BLOCK_GET_ELEMENT(&model->shaders, visor_shader_index, struct model_shader_reference);
	long_nose_chief_globals.visor = (struct shader_model_reflection *)shader_get_and_verify_type(
		shader_definition_get(visor_reference->shader.index), _shader_type_model);
	long_nose_chief_globals.visor_perpendicular_tint = long_nose_chief_globals.visor->perpendicular_tint_color;
	long_nose_chief_globals.visor_parallel_tint = long_nose_chief_globals.visor->parallel_tint_color;
	long_nose_chief_globals.visor->perpendicular_tint_color = long_nose_visor_perpendicular_tint;
	long_nose_chief_globals.visor->parallel_tint_color = long_nose_visor_parallel_tint;
	long_nose_chief_globals.applied = TRUE;

	return TRUE;
}


/* ---------- public code */

/* (scenario_tags_load, once a map's tags are loaded) */
void long_nose_chief_tags_loaded(
	void)
{
	long_nose_chief_globals.map_loaded = TRUE;
	if (long_nose_chief_wanted())
		long_nose_chief_apply();
}

/* (scenario_tags_unload, before anything of the map's is let go) */
void long_nose_chief_tags_unloaded(
	void)
{
	long_nose_chief_restore();
	long_nose_chief_globals.map_loaded = FALSE;
}

/* (the script function long_nose_chief) On or off now, and for the maps
after, until the game quits. */
void long_nose_chief_enable(
	boolean enabled)
{
	long_nose_chief_globals.console_setting = enabled ? TRUE : FALSE;
	if (!long_nose_chief_globals.map_loaded)
		return;
	if (!enabled)
	{
		long_nose_chief_restore();
	}
	else if (!long_nose_chief_apply())
	{
		console_warning("long_nose_chief: this map has no %s to give a nose", LONG_NOSE_CHIEF_MODEL);
	}
}
