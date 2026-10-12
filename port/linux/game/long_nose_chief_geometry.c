/*
LONG_NOSE_CHIEF_GEOMETRY.C

The long-nose Master Chief's pieces (long_nose_chief_geometry.h), made from
the measures below as the design's previews were (a Blender model built on
the cyborg's own mesh, signed off by the owner): every length is in world
units, where Chief stands about 0.7 tall, and every height is above the head
node's bind-pose origin.

The nose is a cone of nose_sides sides, a little flatter than wide, tapering
to a point over 0.17 (about 1.2 helmet lengths) straight out of the visor's
middle and tilted up 3 degrees. Its base is sunk into the visor, so no cap is
needed. Its texture coordinates lie inside the visor's own area of the
cyborg's texture, as it is drawn with the visor's shader: a gold beak.

The smile is drawn with the armor's shader. A dark backing strip, at black
texels of the texture, follows a smile's curve across the visor just in
front of it, its ends drawn to points; two rows of eight small boxes stand on
it, at near-white texels where the armor's colour change does not reach.
Each vertex of the backing and the teeth sits where a ray straight back
along -x meets the helmet (the caller's surface), a little in front of it,
so the smile follows the visor's curve.
*/

#include "long_nose_chief_geometry.h"

#include <math.h>
#include <string.h>

/* ---------- constants */

#define LONG_NOSE_PI 3.14159265358979f

/* the nose */
#define NOSE_HEIGHT 0.0524f
#define NOSE_LENGTH 0.17f
#define NOSE_HALF_WIDTH 0.019f
#define NOSE_HALF_HEIGHT 0.0125f
#define NOSE_TILT_DEGREES 3.f
/* how far its base is sunk behind the visor's front */
#define NOSE_SUNK 0.009f
/* how its width falls toward the point: (1 - s) to this power, s the
fraction of its length */
#define NOSE_TAPER 1.05f
/* inside the visor's area of the cyborg's texture (u 0.58 to 0.69, v 0.21
to 0.29: as the model stores them, v down from the top) */
#define NOSE_U 0.635f
#define NOSE_U_SPREAD 0.015f
#define NOSE_V 0.255f
#define NOSE_V_SPREAD (-0.02f)

/* the smile */
#define MOUTH_HEIGHT 0.0344f
#define MOUTH_HALF_WIDTH 0.029f
/* half the mouth's opening at its middle, and how far its ends rise */
#define MOUTH_HALF_OPENING 0.008f
#define MOUTH_SMILE 0.007f
/* the backing, a little wider and taller than the teeth, and in front of
the visor by this much */
#define BACKING_SPAN 1.06f
#define BACKING_OPENING 1.1f
#define BACKING_STANDOFF 0.001f
/* the teeth's span of the mouth's width (-1 to 1), the gap between two,
how far each row's roots stand off the mouth's middle line, how much of the
opening they fill, and how far in front of the visor their backs and fronts
are */
#define TEETH_SPAN 0.93f
#define TEETH_GAP 0.0009f
#define TEETH_ROOT 0.0006f
#define TEETH_LENGTH 0.95f
#define TEETH_BACK 0.0014f
#define TEETH_FRONT 0.0042f
/* black texels, and near-white ones the colour change does not reach */
#define BACKING_U 0.9415f
#define BACKING_V 0.013f
#define TEETH_U 0.906f
#define TEETH_V 0.012f

/* where the rays at the surface start, in front of the head node */
#define SURFACE_RAY_START 0.5f

/* the nose's rings, as fractions of its length (the point is at 1) */
static float const nose_rings[] = { 0.f, 0.18f, 0.42f, 0.68f };

#define NUMBER_OF_NOSE_RINGS ((int)(sizeof(nose_rings) / sizeof(nose_rings[0])))

/* ---------- private code */

static void vector_set(
	float *v,
	float x,
	float y,
	float z)
{
	v[0] = x;
	v[1] = y;
	v[2] = z;
}

static void vector_cross(
	float const *a,
	float const *b,
	float *result)
{
	float x = a[1] * b[2] - a[2] * b[1];
	float y = a[2] * b[0] - a[0] * b[2];
	float z = a[0] * b[1] - a[1] * b[0];

	vector_set(result, x, y, z);
}

static float vector_normalize(
	float *v)
{
	float length = (float)sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length > 0.f)
	{
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}

	return length;
}

/* the triangle's normal, scaled by twice its area */
static void triangle_normal(
	struct long_nose_mesh const *mesh,
	long triangle_index,
	float *normal)
{
	unsigned short const *triangle = mesh->triangles[triangle_index];
	float const *a = mesh->vertices[triangle[0]].position;
	float const *b = mesh->vertices[triangle[1]].position;
	float const *c = mesh->vertices[triangle[2]].position;
	float ab[3];
	float ac[3];

	vector_set(ab, b[0] - a[0], b[1] - a[1], b[2] - a[2]);
	vector_set(ac, c[0] - a[0], c[1] - a[1], c[2] - a[2]);
	vector_cross(ab, ac, normal);
}

static long vertex_add(
	struct long_nose_mesh *mesh,
	float const *position,
	float u,
	float v)
{
	struct long_nose_vertex *vertex = &mesh->vertices[mesh->vertex_count];

	memcpy(vertex->position, position, sizeof(vertex->position));
	vector_set(vertex->normal, 0.f, 0.f, 0.f);
	vertex->texcoord[0] = u;
	vertex->texcoord[1] = v;

	return mesh->vertex_count++;
}

static void triangle_add(
	struct long_nose_mesh *mesh,
	long a,
	long b,
	long c)
{
	unsigned short *triangle = mesh->triangles[mesh->triangle_count++];

	triangle[0] = (unsigned short)a;
	triangle[1] = (unsigned short)b;
	triangle[2] = (unsigned short)c;
}

static void quadrilateral_add(
	struct long_nose_mesh *mesh,
	long a,
	long b,
	long c,
	long d)
{
	triangle_add(mesh, a, b, c);
	triangle_add(mesh, a, c, d);
}

/* every vertex from first on a smooth normal: its triangles', by area */
static void smooth_normals(
	struct long_nose_mesh *mesh,
	long first_vertex,
	long first_triangle)
{
	long triangle_index;
	long vertex_index;

	for (triangle_index = first_triangle; triangle_index < mesh->triangle_count; triangle_index++)
	{
		float normal[3];
		int corner;

		triangle_normal(mesh, triangle_index, normal);
		for (corner = 0; corner < 3; corner++)
		{
			float *sum = mesh->vertices[mesh->triangles[triangle_index][corner]].normal;

			sum[0] += normal[0];
			sum[1] += normal[1];
			sum[2] += normal[2];
		}
	}
	for (vertex_index = first_vertex; vertex_index < mesh->vertex_count; vertex_index++)
	{
		if (vector_normalize(mesh->vertices[vertex_index].normal) <= 0.f)
			vector_set(mesh->vertices[vertex_index].normal, 1.f, 0.f, 0.f);
	}
}

/* the point on the helmet in front of (y, z), standoff in front of it;
where no ray meets it, at the visor's middle's depth */
static void surface_point(
	long_nose_surface_proc surface,
	void *context,
	float fallback_x,
	float y,
	float z,
	float standoff,
	float *point)
{
	float x;

	if (!surface(context, y, z, &x))
		x = fallback_x;
	vector_set(point, x + standoff, y, z);
}

static int nose_build(
	struct long_nose_mesh *mesh,
	int sides,
	float const head[3],
	long_nose_surface_proc surface,
	void *context)
{
	float tilt = NOSE_TILT_DEGREES * LONG_NOSE_PI / 180.f;
	float direction[3];
	float base[3];
	float point[3];
	long rings[NUMBER_OF_NOSE_RINGS];
	long tip;
	int ring;
	int side;

	vector_set(direction, (float)cos(tilt), 0.f, (float)sin(tilt));
	if (!surface(context, 0.f, head[2] + NOSE_HEIGHT, &base[0]))
		return 0;
	vector_set(base, base[0] - NOSE_SUNK, 0.f, head[2] + NOSE_HEIGHT);

	for (ring = 0; ring < NUMBER_OF_NOSE_RINGS; ring++)
	{
		float s = nose_rings[ring];
		float taper = (float)pow(1.f - s, NOSE_TAPER);

		rings[ring] = mesh->vertex_count;
		for (side = 0; side < sides; side++)
		{
			float angle = 2.f * LONG_NOSE_PI * (float)side / (float)sides;
			float across = (float)cos(angle) * NOSE_HALF_WIDTH * taper;
			float up = (float)sin(angle) * NOSE_HALF_HEIGHT * taper;
			float position[3];

			/* the ring stands square to the nose's tilted line */
			vector_set(position,
				base[0] + direction[0] * s * NOSE_LENGTH - up * direction[2],
				across,
				base[2] + direction[2] * s * NOSE_LENGTH + up * direction[0]);
			vertex_add(mesh, position,
				NOSE_U + NOSE_U_SPREAD * (float)cos(angle),
				NOSE_V + NOSE_V_SPREAD * s / nose_rings[NUMBER_OF_NOSE_RINGS - 1]);
		}
	}
	vector_set(point,
		base[0] + direction[0] * NOSE_LENGTH,
		0.f,
		base[2] + direction[2] * NOSE_LENGTH);
	tip = vertex_add(mesh, point, NOSE_U, NOSE_V + NOSE_V_SPREAD);

	for (ring = 0; ring + 1 < NUMBER_OF_NOSE_RINGS; ring++)
	{
		for (side = 0; side < sides; side++)
		{
			int next = (side + 1) % sides;

			quadrilateral_add(mesh, rings[ring] + side, rings[ring] + next, rings[ring + 1] + next,
				rings[ring + 1] + side);
		}
	}
	for (side = 0; side < sides; side++)
	{
		triangle_add(mesh, rings[NUMBER_OF_NOSE_RINGS - 1] + side,
			rings[NUMBER_OF_NOSE_RINGS - 1] + (side + 1) % sides, tip);
	}
	smooth_normals(mesh, 0, 0);
	/* (the point's normal is the nose's line) */
	memcpy(mesh->vertices[tip].normal, direction, sizeof(direction));

	return 1;
}

/* the mouth's middle line at t (-1 at its right end to 1 at its left), and
half its opening there */
static float mouth_line(
	float const head[3],
	float t)
{
	return head[2] + MOUTH_HEIGHT + MOUTH_SMILE * t * t;
}

static float mouth_opening(
	float t)
{
	return MOUTH_HALF_OPENING * (1.f - 0.8f * t * t * t * t);
}

/* one tooth: a box from its root line (y0 to y1 at z_root0, z_root1) to its
tip line (z_tip0, z_tip1), each face with its own four vertices so that it
is lit flat */
static void tooth_build(
	struct long_nose_mesh *mesh,
	long_nose_surface_proc surface,
	void *context,
	float fallback_x,
	float y0,
	float y1,
	float z_root0,
	float z_root1,
	float z_tip0,
	float z_tip1)
{
	/* corners: root0, root1, tip1, tip0; back then front */
	float corners[2][4][3];
	float center[3] = { 0.f, 0.f, 0.f };
	/* the five faces (not the back, against the visor), by corner: index
	+ 4 for the front */
	static int const faces[5][4] = {
		{ 4, 5, 6, 7 }, /* front */
		{ 0, 1, 5, 4 }, /* the root edge */
		{ 3, 7, 6, 2 }, /* the tip edge */
		{ 0, 4, 7, 3 }, /* the y0 side */
		{ 1, 2, 6, 5 }, /* the y1 side */
	};
	int layer;
	int face;

	for (layer = 0; layer < 2; layer++)
	{
		float standoff = layer ? TEETH_FRONT : TEETH_BACK;
		int corner;

		surface_point(surface, context, fallback_x, y0, z_root0, standoff, corners[layer][0]);
		surface_point(surface, context, fallback_x, y1, z_root1, standoff, corners[layer][1]);
		surface_point(surface, context, fallback_x, y1, z_tip1, standoff, corners[layer][2]);
		surface_point(surface, context, fallback_x, y0, z_tip0, standoff, corners[layer][3]);
		for (corner = 0; corner < 4; corner++)
		{
			center[0] += corners[layer][corner][0] / 8.f;
			center[1] += corners[layer][corner][1] / 8.f;
			center[2] += corners[layer][corner][2] / 8.f;
		}
	}

	for (face = 0; face < 5; face++)
	{
		long first = mesh->vertex_count;
		float normal[3];
		float middle[3] = { 0.f, 0.f, 0.f };
		int corner;

		for (corner = 0; corner < 4; corner++)
		{
			int index = faces[face][corner];
			float const *position = corners[index / 4][index % 4];

			vertex_add(mesh, position, TEETH_U, TEETH_V);
			middle[0] += position[0] / 4.f;
			middle[1] += position[1] / 4.f;
			middle[2] += position[2] / 4.f;
		}
		quadrilateral_add(mesh, first, first + 1, first + 2, first + 3);
		/* (out of the box, whichever way the row's corners turn) */
		triangle_normal(mesh, mesh->triangle_count - 2, normal);
		if (normal[0] * (middle[0] - center[0]) + normal[1] * (middle[1] - center[1]) +
				normal[2] * (middle[2] - center[2]) < 0.f)
		{
			unsigned short *a = mesh->triangles[mesh->triangle_count - 2];
			unsigned short *b = mesh->triangles[mesh->triangle_count - 1];
			unsigned short swap;

			swap = a[1], a[1] = a[2], a[2] = swap;
			swap = b[1], b[1] = b[2], b[2] = swap;
			normal[0] = -normal[0];
			normal[1] = -normal[1];
			normal[2] = -normal[2];
		}
		vector_normalize(normal);
		for (corner = 0; corner < 4; corner++)
			memcpy(mesh->vertices[first + corner].normal, normal, sizeof(normal));
	}
}

static int smile_build(
	struct long_nose_mesh *mesh,
	float const head[3],
	long_nose_surface_proc surface,
	void *context)
{
	long top[LONG_NOSE_MOUTH_SEGMENTS + 1];
	long bottom[LONG_NOSE_MOUTH_SEGMENTS + 1];
	float fallback_x;
	int segment;
	int row;
	int tooth;

	if (!surface(context, 0.f, mouth_line(head, 0.f), &fallback_x))
		return 0;

	/* the backing */
	for (segment = 0; segment <= LONG_NOSE_MOUTH_SEGMENTS; segment++)
	{
		float t = -1.f + 2.f * (float)segment / (float)LONG_NOSE_MOUTH_SEGMENTS;
		float y = t * MOUTH_HALF_WIDTH * BACKING_SPAN;
		float half = mouth_opening(t) * BACKING_OPENING;
		float position[3];

		surface_point(surface, context, fallback_x, y, mouth_line(head, t) + half, BACKING_STANDOFF, position);
		top[segment] = vertex_add(mesh, position, BACKING_U, BACKING_V);
		surface_point(surface, context, fallback_x, y, mouth_line(head, t) - half, BACKING_STANDOFF, position);
		bottom[segment] = vertex_add(mesh, position, BACKING_U, BACKING_V);
	}
	for (segment = 0; segment < LONG_NOSE_MOUTH_SEGMENTS; segment++)
		quadrilateral_add(mesh, bottom[segment], bottom[segment + 1], top[segment + 1], top[segment]);
	smooth_normals(mesh, 0, 0);

	/* the teeth, upper row then lower, each from its root at the mouth's
	middle line out to most of the opening */
	for (row = 1; row >= -1; row -= 2)
	{
		for (tooth = 0; tooth < LONG_NOSE_TEETH_PER_ROW; tooth++)
		{
			float t0 = -TEETH_SPAN + 2.f * TEETH_SPAN * (float)tooth / (float)LONG_NOSE_TEETH_PER_ROW;
			float t1 = -TEETH_SPAN + 2.f * TEETH_SPAN * (float)(tooth + 1) / (float)LONG_NOSE_TEETH_PER_ROW;

			tooth_build(mesh, surface, context, fallback_x,
				t0 * MOUTH_HALF_WIDTH + TEETH_GAP / 2.f,
				t1 * MOUTH_HALF_WIDTH - TEETH_GAP / 2.f,
				mouth_line(head, t0) + (float)row * TEETH_ROOT,
				mouth_line(head, t1) + (float)row * TEETH_ROOT,
				mouth_line(head, t0) + (float)row * mouth_opening(t0) * TEETH_LENGTH,
				mouth_line(head, t1) + (float)row * mouth_opening(t1) * TEETH_LENGTH);
		}
	}

	return 1;
}

/* ---------- public code */

int long_nose_mesh_build(
	struct long_nose_mesh *mesh,
	int piece,
	int nose_sides,
	float const head[3],
	long_nose_surface_proc surface,
	void *context)
{
	int built = 0;

	mesh->vertex_count = 0;
	mesh->triangle_count = 0;
	switch (piece)
	{
	case _long_nose_piece_nose:
		if (nose_sides >= 3 && nose_sides <= LONG_NOSE_MAXIMUM_NOSE_SIDES)
			built = nose_build(mesh, nose_sides, head, surface, context);
		break;
	case _long_nose_piece_smile:
		built = smile_build(mesh, head, surface, context);
		break;
	}
	if (!built)
	{
		mesh->vertex_count = 0;
		mesh->triangle_count = 0;
	}

	return built;
}

long long_nose_mesh_strip(
	struct long_nose_mesh const *mesh,
	int reverse,
	unsigned short *indices,
	long maximum_index_count)
{
	long count = 0;
	long triangle_index;

	for (triangle_index = 0; triangle_index < mesh->triangle_count; triangle_index++)
	{
		unsigned short const *triangle = mesh->triangles[triangle_index];
		unsigned short a = triangle[0];
		unsigned short b = reverse ? triangle[2] : triangle[1];
		unsigned short c = reverse ? triangle[1] : triangle[2];

		if (count + 6 > maximum_index_count)
			return 0;
		/* a strip's triangle at an even index keeps its corners' order and
		one at an odd index swaps its first two: each triangle starts at an
		even index, after the last index repeated (once or twice, for that)
		and its own first twice, which make only triangles of no area */
		if (count)
		{
			unsigned short last = indices[count - 1];

			indices[count++] = last;
			if (count % 2 == 0)
				indices[count++] = last;
			indices[count++] = a;
		}
		indices[count++] = a;
		indices[count++] = b;
		indices[count++] = c;
	}

	return count;
}
