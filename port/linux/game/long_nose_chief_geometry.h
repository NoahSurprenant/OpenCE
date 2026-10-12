/* long_nose_chief_geometry.h: the long-nose Master Chief's added pieces
(game.long_nose_chief: long_nose_chief.c), made by code from a handful of
measures, so that no game data is needed to make them or to test them. A
piece is made in the bind-pose space of the model it goes on (x forward, y
left, z up, world units: Chief stands about 0.7 tall), around its head
node, against the helmet's own front, which the caller finds. No header of
the game's is needed: the tests build it alone (tools/long_nose_chief_check.c). */

#ifndef LONG_NOSE_CHIEF_GEOMETRY_H
#define LONG_NOSE_CHIEF_GEOMETRY_H

enum
{
	/* the nose's sides, near and far (the model's lowest level of detail) */
	LONG_NOSE_NOSE_SIDES = 10,
	LONG_NOSE_FAR_NOSE_SIDES = 6,
	LONG_NOSE_MAXIMUM_NOSE_SIDES = 16,
	/* the mouth's backing, in segments across, and the teeth in each row */
	LONG_NOSE_MOUTH_SEGMENTS = 12,
	LONG_NOSE_TEETH_PER_ROW = 8,

	/* the most a piece has: the smile's 26 backing and 16 * 20 tooth
	vertices, its 24 + 160 triangles */
	LONG_NOSE_MAXIMUM_VERTICES = 360,
	LONG_NOSE_MAXIMUM_TRIANGLES = 200,
	/* a strip of them: 3 indices for the first, at most 6 for each after */
	LONG_NOSE_MAXIMUM_STRIP_INDICES = 3 + 6 * (LONG_NOSE_MAXIMUM_TRIANGLES - 1)
};

enum
{
	/* the nose: a tapered cone out of the visor, drawn with the visor's shader */
	_long_nose_piece_nose = 0,
	/* the smile: a dark backing and two rows of teeth on the visor, drawn
	with the armor's shader */
	_long_nose_piece_smile,
	NUMBER_OF_LONG_NOSE_PIECES
};

struct long_nose_vertex
{
	float position[3];
	/* unit length */
	float normal[3];
	/* in the model's texture, before its base map scale */
	float texcoord[2];
};

struct long_nose_mesh
{
	long vertex_count;
	long triangle_count;
	struct long_nose_vertex vertices[LONG_NOSE_MAXIMUM_VERTICES];
	/* counter-clockwise seen from outside: the normal of a, b, c is
	(b - a) x (c - a), which points out */
	unsigned short triangles[LONG_NOSE_MAXIMUM_TRIANGLES][3];
};

/* The helmet's front: where a ray along -x through (y, z) of the model's
space, from in front of the face, first meets the model, in *x. Returns 0
when it meets nothing there. */
typedef int (*long_nose_surface_proc)(void *context, float y, float z, float *x);

/* Makes `piece` (a _long_nose_piece_*) in `mesh`, for a model whose head
node stands at `head` in the bind pose; the nose has nose_sides sides
(3 to LONG_NOSE_MAXIMUM_NOSE_SIDES). Returns 0, and an empty mesh, when the
surface is not found in front of the face. */
int long_nose_mesh_build(
	struct long_nose_mesh *mesh,
	int piece,
	int nose_sides,
	float const head[3],
	long_nose_surface_proc surface,
	void *context);

/* A triangle strip of the mesh's triangles, each kept in its winding
(reversed when `reverse`: for a renderer whose front faces are clockwise),
joined by triangles of no area. Returns its index count (the strip's
triangles are 2 fewer), or 0 when more than maximum_index_count. */
long long_nose_mesh_strip(
	struct long_nose_mesh const *mesh,
	int reverse,
	unsigned short *indices,
	long maximum_index_count);

#endif
