/*
LONG_NOSE_CHIEF_CHECK.C

Checks the long-nose Master Chief's pieces (port/linux/game/
long_nose_chief_geometry.c) against a made-up helmet, an ellipsoid where the
cyborg's is (its head node at the cyborg's bind-pose place): the counts of
each piece, that every value is a number, every normal of unit length and
every triangle wound outward along its normals, the nose's length, tilt and
width, the smile on the helmet's front and within the mouth, the texture
coordinates in their areas, that a helmet not found makes nothing, and that
the strips draw exactly the meshes' triangles, in their winding, either way
round. Prints PASS, or each failure and exits 1. Built and run by
tools/test_linux_port.py; no game data is needed.
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../port/linux/game/long_nose_chief_geometry.h"

/* the made-up helmet's front: an ellipsoid's half in front of its middle */
#define HELMET_X (-0.007f)
#define HELMET_Z 0.64f
#define HELMET_DEPTH 0.055f
#define HELMET_HALF_WIDTH 0.045f
#define HELMET_HALF_HEIGHT 0.06f

static float const head[3] = { -0.007176f, 0.f, 0.609093f };
static int failures = 0;

#define CHECK(condition, ...) \
	do { if (!(condition)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int helmet(void *context, float y, float z, float *x)
{
	float dy = y / HELMET_HALF_WIDTH;
	float dz = (z - HELMET_Z) / HELMET_HALF_HEIGHT;
	float inside = 1.f - dy * dy - dz * dz;

	(void)context;
	if (inside < 0.f)
		return 0;
	*x = HELMET_X + HELMET_DEPTH * (float)sqrt(inside);
	return 1;
}

static int nowhere(void *context, float y, float z, float *x)
{
	(void)context;
	(void)y;
	(void)z;
	(void)x;
	return 0;
}

static void triangle_normal(struct long_nose_mesh const *mesh, long index, float *n)
{
	float const *a = mesh->vertices[mesh->triangles[index][0]].position;
	float const *b = mesh->vertices[mesh->triangles[index][1]].position;
	float const *c = mesh->vertices[mesh->triangles[index][2]].position;
	float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	float ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };

	n[0] = ab[1] * ac[2] - ab[2] * ac[1];
	n[1] = ab[2] * ac[0] - ab[0] * ac[2];
	n[2] = ab[0] * ac[1] - ab[1] * ac[0];
}

/* every value a number, normals of unit length, triangles of area naming
vertices that exist and wound along their corners' normals */
static void check_mesh(char const *name, struct long_nose_mesh const *mesh)
{
	long i;
	long against = 0;

	for (i = 0; i < mesh->vertex_count; i++)
	{
		struct long_nose_vertex const *v = &mesh->vertices[i];
		float length = (float)sqrt(v->normal[0] * v->normal[0] + v->normal[1] * v->normal[1] + v->normal[2] * v->normal[2]);
		int k;

		for (k = 0; k < 3; k++)
			CHECK(isfinite(v->position[k]) && isfinite(v->normal[k]), "%s vertex %ld not a number", name, i);
		CHECK(fabs(length - 1.f) < 1e-4f, "%s vertex %ld normal length %f", name, i, length);
	}
	for (i = 0; i < mesh->triangle_count; i++)
	{
		float n[3];
		float area;
		int corner;

		for (corner = 0; corner < 3; corner++)
			CHECK(mesh->triangles[i][corner] < mesh->vertex_count, "%s triangle %ld names vertex %u", name, i,
				mesh->triangles[i][corner]);
		triangle_normal(mesh, i, n);
		area = (float)sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) / 2.f;
		CHECK(area > 1e-9f, "%s triangle %ld has no area", name, i);
		for (corner = 0; corner < 3; corner++)
		{
			float const *vn = mesh->vertices[mesh->triangles[i][corner]].normal;

			if (n[0] * vn[0] + n[1] * vn[1] + n[2] * vn[2] <= 0.f)
				against++;
		}
	}
	CHECK(against == 0, "%s: %ld triangle corners wound against their normals", name, against);
}

/* the strip's triangles of area, in order and winding, are the mesh's */
static void check_strip(char const *name, struct long_nose_mesh const *mesh)
{
	static unsigned short indices[LONG_NOSE_MAXIMUM_STRIP_INDICES];
	int reverse;

	for (reverse = 0; reverse < 2; reverse++)
	{
		long count = long_nose_mesh_strip(mesh, reverse, indices, LONG_NOSE_MAXIMUM_STRIP_INDICES);
		long next = 0;
		long i;

		CHECK(count >= 3 && count <= 3 + 6 * (mesh->triangle_count - 1), "%s strip of %ld indices", name, count);
		for (i = 0; i + 2 < count; i++)
		{
			unsigned short a = indices[i], b = indices[i + 1], c = indices[i + 2];
			unsigned short const *t;

			if (a == b || b == c || a == c)
				continue;
			/* (a strip's odd triangles swap their first two corners) */
			if (i % 2)
			{
				unsigned short swap = a;
				a = b;
				b = swap;
			}
			if (next >= mesh->triangle_count)
			{
				CHECK(0, "%s strip has more triangles than the mesh", name);
				break;
			}
			t = mesh->triangles[next++];
			CHECK(a == t[0] && b == (reverse ? t[2] : t[1]) && c == (reverse ? t[1] : t[2]),
				"%s strip triangle %ld (%u %u %u) is not the mesh's (%u %u %u)%s", name, next - 1, a, b, c, t[0], t[1],
				t[2], reverse ? " reversed" : "");
		}
		CHECK(next == mesh->triangle_count, "%s strip draws %ld of %ld triangles", name, next, mesh->triangle_count);
		CHECK(long_nose_mesh_strip(mesh, reverse, indices, 5) == 0 || mesh->triangle_count == 1,
			"%s strip past its room", name);
	}
}

static void check_nose(int sides)
{
	static struct long_nose_mesh mesh;
	char name[32];
	float base_x;
	float tip[3] = { -1e9f, 0.f, 0.f };
	float axis[3];
	long i;

	sprintf(name, "nose of %d", sides);
	CHECK(long_nose_mesh_build(&mesh, _long_nose_piece_nose, sides, head, helmet, NULL), "%s not built", name);
	CHECK(mesh.vertex_count == 4 * sides + 1, "%s has %ld vertices", name, mesh.vertex_count);
	CHECK(mesh.triangle_count == 7 * sides, "%s has %ld triangles", name, mesh.triangle_count);
	check_mesh(name, &mesh);
	check_strip(name, &mesh);

	helmet(NULL, 0.f, head[2] + 0.0524f, &base_x);
	for (i = 0; i < mesh.vertex_count; i++)
	{
		float const *p = mesh.vertices[i].position;
		float const *uv = mesh.vertices[i].texcoord;

		if (p[0] > tip[0])
		{
			tip[0] = p[0];
			tip[1] = p[1];
			tip[2] = p[2];
		}
		CHECK(fabs(p[1]) <= 0.019f + 1e-5f, "%s vertex %ld wider than the nose (%f)", name, i, p[1]);
		CHECK(p[0] >= base_x - 0.01f, "%s vertex %ld behind its base (%f)", name, i, p[0]);
		CHECK(uv[0] > 0.58f && uv[0] < 0.69f && uv[1] > 0.21f && uv[1] < 0.29f,
			"%s vertex %ld texture (%f %f) outside the visor's", name, i, uv[0], uv[1]);
	}
	/* 0.17 long, 3 degrees up, from 0.009 behind the visor's middle */
	CHECK(fabs(tip[0] - (base_x - 0.009f + 0.17f * (float)cos(3.0 * 3.14159265 / 180.0))) < 1e-4f, "%s tip x %f", name,
		tip[0]);
	CHECK(fabs(tip[1]) < 1e-6f, "%s tip off the middle (%f)", name, tip[1]);
	CHECK(fabs(tip[2] - (head[2] + 0.0524f + 0.17f * (float)sin(3.0 * 3.14159265 / 180.0))) < 1e-4f, "%s tip z %f",
		name, tip[2]);
	/* outward: each triangle faces away from the nose's line */
	axis[0] = tip[0] - (base_x - 0.009f);
	axis[1] = 0.f;
	axis[2] = tip[2] - (head[2] + 0.0524f);
	for (i = 0; i < mesh.triangle_count; i++)
	{
		float n[3];
		float center[3] = { 0.f, 0.f, 0.f };
		float s;
		float out[3];
		int corner;

		for (corner = 0; corner < 3; corner++)
		{
			float const *p = mesh.vertices[mesh.triangles[i][corner]].position;

			center[0] += p[0] / 3.f;
			center[1] += p[1] / 3.f;
			center[2] += p[2] / 3.f;
		}
		s = ((center[0] - (base_x - 0.009f)) * axis[0] + (center[2] - (head[2] + 0.0524f)) * axis[2]) /
			(axis[0] * axis[0] + axis[2] * axis[2]);
		out[0] = center[0] - (base_x - 0.009f + s * axis[0]);
		out[1] = center[1];
		out[2] = center[2] - (head[2] + 0.0524f + s * axis[2]);
		triangle_normal(&mesh, i, n);
		CHECK(n[0] * out[0] + n[1] * out[1] + n[2] * out[2] > 0.f, "%s triangle %ld faces in", name, i);
	}
}

static void check_smile(void)
{
	static struct long_nose_mesh mesh;
	long i;
	long dark = 0;
	long white = 0;

	CHECK(long_nose_mesh_build(&mesh, _long_nose_piece_smile, 0, head, helmet, NULL), "smile not built");
	/* the backing's 13 pairs and 12 quadrilaterals; 16 teeth of 5 faces */
	CHECK(mesh.vertex_count == 26 + 16 * 20, "smile has %ld vertices", mesh.vertex_count);
	CHECK(mesh.triangle_count == 24 + 16 * 10, "smile has %ld triangles", mesh.triangle_count);
	check_mesh("smile", &mesh);
	check_strip("smile", &mesh);
	for (i = 0; i < mesh.vertex_count; i++)
	{
		float const *p = mesh.vertices[i].position;
		float const *uv = mesh.vertices[i].texcoord;
		float front;

		CHECK(fabs(p[1]) <= 0.029f * 1.06f + 1e-5f, "smile vertex %ld past the mouth's width (%f)", i, p[1]);
		CHECK(p[2] > head[2] + 0.0344f - 0.0095f && p[2] < head[2] + 0.0344f + 0.007f + 0.0095f,
			"smile vertex %ld outside the mouth's height (%f)", i, p[2]);
		CHECK(helmet(NULL, p[1], p[2], &front) && p[0] > front && p[0] < front + 0.005f,
			"smile vertex %ld not just in front of the helmet (%f, front %f)", i, p[0], front);
		if (fabs(uv[0] - 0.9415f) < 1e-6f && fabs(uv[1] - 0.013f) < 1e-6f)
			dark++;
		else if (fabs(uv[0] - 0.906f) < 1e-6f && fabs(uv[1] - 0.012f) < 1e-6f)
			white++;
	}
	CHECK(dark == 26 && white == 16 * 20, "smile textures: %ld dark, %ld white", dark, white);
	/* the teeth's fronts face forward */
	for (i = 24; i < mesh.triangle_count; i += 10)
	{
		float n[3];

		triangle_normal(&mesh, i, n);
		CHECK(n[0] > 0.f, "tooth %ld's front faces back", (i - 24) / 10);
	}
}

int main(void)
{
	static struct long_nose_mesh mesh;

	check_nose(LONG_NOSE_NOSE_SIDES);
	check_nose(LONG_NOSE_FAR_NOSE_SIDES);
	check_smile();

	CHECK(!long_nose_mesh_build(&mesh, _long_nose_piece_nose, LONG_NOSE_NOSE_SIDES, head, nowhere, NULL) &&
		!mesh.vertex_count && !mesh.triangle_count, "a nose without a helmet");
	CHECK(!long_nose_mesh_build(&mesh, _long_nose_piece_smile, 0, head, nowhere, NULL) && !mesh.vertex_count,
		"a smile without a helmet");
	CHECK(!long_nose_mesh_build(&mesh, _long_nose_piece_nose, 2, head, helmet, NULL), "a nose of 2 sides");
	CHECK(!long_nose_mesh_build(&mesh, _long_nose_piece_nose, LONG_NOSE_MAXIMUM_NOSE_SIDES + 1, head, helmet, NULL),
		"a nose of too many sides");
	CHECK(long_nose_mesh_build(&mesh, _long_nose_piece_nose, LONG_NOSE_MAXIMUM_NOSE_SIDES, head, helmet, NULL) &&
		mesh.vertex_count <= LONG_NOSE_MAXIMUM_VERTICES, "the most sides a nose may have");
	CHECK(!long_nose_mesh_build(&mesh, NUMBER_OF_LONG_NOSE_PIECES, 10, head, helmet, NULL), "a piece that is none");

	if (failures)
	{
		printf("%d failures\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
