/*
BOTS_NAV.C (test)

The bots' walking graph (port/linux/game/bots_nav.c, the whole file, which
needs nothing of the engine) built from small made-up levels of boxes: a
floor's nodes and the walk across it, a drop one way and not back, a jump up
onto a step and the drop back down, a wall in the way, a low ceiling, a slope
too steep, a teleporter, what the spawns cannot reach dropped, and the cache's
bytes read back (and refused when they are not this graph's).
test_bots_nav.py gives the code (under_test.inc) and its header.
*/

#include <math.h>
#include "harness.h"

#include "under_test.inc"

#define MAXIMUM_TRIANGLES 4096

static float triangles[MAXIMUM_TRIANGLES * 9];
static long triangle_count;

static void triangle(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz)
{
	float *t = triangles + triangle_count++ * 9;

	t[0] = ax; t[1] = ay; t[2] = az; t[3] = bx; t[4] = by; t[5] = bz; t[6] = cx; t[7] = cy; t[8] = cz;
}

/* a quad, its corners counterclockwise seen from its front */
static void quad(float const *a, float const *b, float const *c, float const *d)
{
	triangle(a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]);
	triangle(a[0], a[1], a[2], c[0], c[1], c[2], d[0], d[1], d[2]);
}

/* a solid box's faces, outward */
static void box(float x0, float y0, float z0, float x1, float y1, float z1)
{
	float p[8][3] = {
		{ x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
		{ x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 },
	};

	quad(p[4], p[5], p[6], p[7]);
	quad(p[0], p[3], p[2], p[1]);
	quad(p[0], p[1], p[5], p[4]);
	quad(p[1], p[2], p[6], p[5]);
	quad(p[2], p[3], p[7], p[6]);
	quad(p[3], p[0], p[4], p[7]);
}

/* walls: none of the test's moves crosses the plane x = wall_x below
wall_top, when there is a wall */
static int wall;
static float wall_x, wall_top;

static int clear(void *context, struct bots_nav_point const *from, struct bots_nav_point const *to)
{
	(void)context;
	if (!wall)
		return 1;
	if ((from->x - wall_x) * (to->x - wall_x) < 0.0f)
	{
		float t = (wall_x - from->x) / (to->x - from->x);
		float z = from->z + (to->z - from->z) * t;

		return z > wall_top;
	}
	return 1;
}

static struct bots_nav nav;
static struct bots_nav_point seeds[8];
static long seed_count;
static struct bots_nav_link links[4];
static long link_count;
static struct bots_nav_ladder ladders[2];
static long ladder_count;

/* the graph a vehicle drives, not the walking graph (the drive cases) */
static int vehicle;

static void build(void)
{
	struct bots_nav_input input;
	struct bots_nav_parameters parameters;

	if (vehicle)
		bots_nav_vehicle_parameters(&parameters);
	else
		bots_nav_default_parameters(&parameters);
	memset(&input, 0, sizeof(input));
	input.triangles = triangles;
	input.triangle_count = triangle_count;
	input.seeds = seeds;
	input.seed_count = seed_count;
	input.links = links;
	input.link_count = link_count;
	input.ladders = ladders;
	input.ladder_count = ladder_count;
	input.clear = clear;
	CHECK(bots_nav_build(&input, &parameters, &nav), "the graph was not built");
}

static long node_at(float x, float y, float z)
{
	return bots_nav_locate(&nav, x, y, z);
}

/* the path's edge types, first first, as letters (w d j t), or "none" */
static char const *path(float x0, float y0, float z0, float x1, float y1, float z1)
{
	static char text[256];
	long edges[200];
	long from = node_at(x0, y0, z0), to = node_at(x1, y1, z1);
	long count, index, length = 0;

	CHECK(from >= 0 && to >= 0, "no node at (%g %g %g) or (%g %g %g)", x0, y0, z0, x1, y1, z1);
	count = bots_nav_find_path(&nav, from, to, edges, 200);
	if (count < 0)
		return "none";
	/* (only the moves that are not walks, in order: a walk is many edges) */
	for (index = count - 1; index >= 0; index--)
	{
		char letter = "wdjtcl"[nav.edges[edges[index]].type];

		if (letter != 'w')
			text[length++] = letter;
	}
	text[length] = 0;
	return length ? text : "walk";
}

/* the level, the seeds and the graph gone, for another level */
static void reset(void)
{
	bots_nav_free(&nav);
	triangle_count = 0;
	seed_count = 0;
	link_count = 0;
	ladder_count = 0;
	wall = 0;
}

static void seed(float x, float y, float z)
{
	seeds[seed_count].x = x;
	seeds[seed_count].y = y;
	seeds[seed_count].z = z;
	seed_count++;
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "floor"))
	{
		box(0, 0, -1, 10, 10, 0);
		seed(1, 1, 0);
		build();
		/* (ten by ten tiles, the edge ones a little smaller: every tile one piece) */
		CHECK(nav.node_count == 100, "%ld nodes", nav.node_count);
		CHECK(!strcmp(path(0.5f, 0.5f, 0, 9.5f, 9.5f, 0), "walk"), "%s", path(0.5f, 0.5f, 0, 9.5f, 9.5f, 0));
		CHECK(node_at(5, 5, 0.3f) >= 0 && node_at(5, 5, 3.0f) < 0 && node_at(5, 5, -1.0f) < 0, "located off the floor");
		CHECK(nav.statistics.eroded_cells < nav.statistics.cells, "the edges were not eroded");
	}
	else if (!strcmp(name, "drop"))
	{
		box(0, 0, -1, 5, 5, 1);
		box(5.3f, 0, -1, 10, 5, 0);
		seed(1, 1, 1);
		seed(9, 4, 0);
		build();
		CHECK(!strcmp(path(1, 1, 1, 9, 4, 0), "d"), "down: %s", path(1, 1, 1, 9, 4, 0));
		CHECK(!strcmp(path(9, 4, 0, 1, 1, 1), "none"), "back up: %s", path(9, 4, 0, 1, 1, 1));
	}
	else if (!strcmp(name, "jump"))
	{
		box(0, 0, -1, 5, 5, 0);
		box(5, 0, -1, 10, 5, 0.4f);
		seed(1, 1, 0);
		build();
		CHECK(!strcmp(path(1, 1, 0, 9, 4, 0.4f), "j"), "up: %s", path(1, 1, 0, 9, 4, 0.4f));
		CHECK(!strcmp(path(9, 4, 0.4f, 1, 1, 0), "d"), "down: %s", path(9, 4, 0.4f, 1, 1, 0));
		/* (and too high a step to jump: nothing up) */
		triangle_count = 0;
		seed_count = 0;
		bots_nav_free(&nav);
		box(0, 0, -1, 5, 5, 0);
		box(5, 0, -1, 10, 5, 0.9f);
		seed(1, 1, 0);
		seed(9, 4, 0.9f);
		build();
		CHECK(!strcmp(path(1, 1, 0, 9, 4, 0.9f), "none"), "up 0.9: %s", path(1, 1, 0, 9, 4, 0.9f));
	}
	else if (!strcmp(name, "wall"))
	{
		/* (a floor and, past a wall too narrow to stand on, a lower one: a
		drop over the wall, were it not there) */
		box(0, 0, -1, 4.95f, 5, 1);
		box(4.95f, 0, -1, 5.05f, 5, 2.5f);
		box(5.05f, 0, -1, 10, 5, 0);
		wall = 1;
		wall_x = 5.0f;
		wall_top = 2.5f;
		seed(1, 1, 1);
		seed(9, 4, 0);
		build();
		/* (the far side neither reached from the first spawn's side nor
		reaching it: not kept) */
		CHECK(node_at(1, 1, 1) >= 0 && node_at(9, 4, 0) < 0, "through the wall");
		{
			long node;

			for (node = 0; node < nav.node_count; node++)
				CHECK(nav.nodes[node].point.z < 2.0f, "a node on the wall's top");
		}
	}
	else if (!strcmp(name, "ceiling"))
	{
		box(0, 0, -1, 10, 5, 0);
		/* (half a world unit of room under it: less than the biped's height) */
		box(4, 0, 0.5f, 6, 5, 1);
		seed(1, 1, 0);
		build();
		CHECK(node_at(5, 2.5f, 0) < 0, "a node under the low ceiling");
		CHECK(node_at(5, 2.5f, 1) >= 0, "no node on its top");
	}
	else if (!strcmp(name, "slope"))
	{
		float a[3] = { 0, 0, 0 }, b[3] = { 4, 0, 0 }, c[3] = { 4, 4, 0 }, d[3] = { 0, 4, 0 };
		float e[3] = { 4, 0, 0 }, f[3] = { 6, 0, 4 }, g[3] = { 6, 4, 4 }, h[3] = { 4, 4, 0 };
		float i[3] = { 6, 0, 4 }, j[3] = { 10, 0, 4 }, k[3] = { 10, 4, 4 }, l[3] = { 6, 4, 4 };

		/* (a floor, a slope of 63 degrees up to a ledge, the ledge) */
		quad(a, b, c, d);
		quad(e, f, g, h);
		quad(i, j, k, l);
		seed(1, 1, 0);
		seed(9, 2, 4);
		build();
		CHECK(node_at(5, 2, 2) < 0, "a node on the steep slope");
		/* (the ledge neither reached up the slope nor, a 4 unit drop, reaching
		the floor: not kept) */
		CHECK(node_at(1, 1, 0) >= 0 && node_at(9, 2, 4) < 0, "up or down the steep slope");
	}
	else if (!strcmp(name, "ramp"))
	{
		float a[3] = { 0, 0, 0 }, b[3] = { 4, 0, 0 }, c[3] = { 4, 4, 0 }, d[3] = { 0, 4, 0 };
		float e[3] = { 4, 0, 0 }, f[3] = { 8, 0, 2 }, g[3] = { 8, 4, 2 }, h[3] = { 4, 4, 0 };

		/* (a floor and a slope of 27 degrees, walked up and down) */
		quad(a, b, c, d);
		quad(e, f, g, h);
		seed(1, 1, 0);
		build();
		CHECK(!strcmp(path(1, 1, 0, 7.5f, 2, 1.75f), "walk"), "up the ramp: %s", path(1, 1, 0, 7.5f, 2, 1.75f));
		CHECK(!strcmp(path(7.5f, 2, 1.75f, 1, 1, 0), "walk"), "down: %s", path(7.5f, 2, 1.75f, 1, 1, 0));
	}
	else if (!strcmp(name, "teleporter"))
	{
		box(0, 0, -1, 5, 5, 0);
		box(20, 0, -1, 25, 5, 0);
		seed(1, 1, 0);
		seed(24, 4, 0);
		links[0].from.x = 2.5f; links[0].from.y = 2.5f; links[0].from.z = 0;
		links[0].to.x = 22.5f; links[0].to.y = 2.5f; links[0].to.z = 0;
		links[0].type = _bots_nav_teleport;
		link_count = 1;
		build();
		CHECK(!strcmp(path(1, 1, 0, 24, 4, 0), "t"), "across: %s", path(1, 1, 0, 24, 4, 0));
		CHECK(!strcmp(path(24, 4, 0, 1, 1, 0), "none"), "back: %s", path(24, 4, 0, 1, 1, 0));
		CHECK(nav.statistics.teleport_edges == 1, "%ld teleporter edges", nav.statistics.teleport_edges);
	}
	else if (!strcmp(name, "ladder"))
	{
		/* (a floor, and a deck 2.5 up past a wall with a ladder on it) */
		box(0, 0, -1, 5, 5, 0);
		box(5, 0, -1, 10, 5, 2.5f);
		ladders[0].bottom.x = 5; ladders[0].bottom.y = 2.5f; ladders[0].bottom.z = 0;
		ladders[0].top = ladders[0].bottom;
		ladders[0].top.z = 2.5f;
		ladders[0].normal_x = -1; ladders[0].normal_y = 0;
		ladder_count = 1;
		seed(1, 1, 0);
		build();
		CHECK(!strcmp(path(1, 1, 0, 9, 4, 2.5f), "c"), "up: %s", path(1, 1, 0, 9, 4, 2.5f));
		CHECK(strcmp(path(9, 4, 2.5f, 1, 1, 0), "none"), "no way down");
		CHECK(nav.statistics.descend_edges == 1, "%ld ways down beside the ladder", nav.statistics.descend_edges);
		CHECK(nav.statistics.climb_edges == 1, "%ld ladder edges", nav.statistics.climb_edges);
	}
	else if (!strcmp(name, "prune"))
	{
		/* (an island nobody reaches nor leaves, and one only reached) */
		box(0, 0, -1, 5, 5, 0);
		box(10, 0, -1, 15, 5, 0);
		box(20, 0, -1, 25, 5, 0);
		box(5.3f, 0, -2, 9.7f, 5, -1.2f);
		seed(1, 1, 0);
		build();
		CHECK(nav.statistics.nodes_before_pruning > nav.node_count, "nothing pruned (%ld nodes)", nav.node_count);
		CHECK(node_at(22, 2, 0) < 0, "the island far away kept");
		CHECK(node_at(7, 2, -1.2f) >= 0 && !(nav.nodes[node_at(7, 2, -1.2f)].reach & _bots_nav_reaches_bit) &&
			(nav.nodes[node_at(7, 2, -1.2f)].reach & _bots_nav_reached_bit), "the pit (reached, not reaching back)");
		CHECK(nav.main_node >= 0 && nav.nodes[nav.main_node].reach == (_bots_nav_reached_bit | _bots_nav_reaches_bit),
			"the main node");
	}
	else if (!strcmp(name, "drive"))
	{
		/* (open ground: two by two tiles, driven across) */
		vehicle = 1;
		box(0, 0, -1, 20, 10, 0);
		seed(1, 1, 0);
		build();
		CHECK(nav.node_count == 50, "%ld nodes on a floor 20 by 10", nav.node_count);
		CHECK(!strcmp(path(1, 1, 0, 19, 9, 0), "walk"), "across: %s", path(1, 1, 0, 19, 9, 0));
		{
			long node;

			for (node = 0; node < nav.node_count; node++)
			{
				struct bots_nav_point const *point = &nav.nodes[node].point;

				CHECK(point->x > 0.45f && point->x < 19.55f && point->y > 0.45f && point->y < 9.55f,
					"a node within half a world unit of the edge (%g %g)", point->x, point->y);
			}
		}
		/* (a bridge 0.8 wide between two floors: walked over, not driven) */
		reset();
		vehicle = 0;
		box(0, 0, -1, 8, 10, 0);
		box(8, 4.6f, -1, 12, 5.4f, 0);
		box(12, 0, -1, 20, 10, 0);
		seed(1, 1, 0);
		build();
		CHECK(!strcmp(path(1, 1, 0, 16, 5, 0), "walk"), "no walk over the bridge: %s", path(1, 1, 0, 16, 5, 0));
		bots_nav_free(&nav);
		vehicle = 1;
		build();
		CHECK(node_at(1, 1, 0) >= 0 && node_at(16, 5, 0) < 0, "a vehicle over the narrow bridge");
		/* (a slope of 40 degrees up to a deck: walked up, not driven, and the
		deck's drop of 1.7 not driven off) */
		reset();
		{
			float a[3] = { 0, 0, 0 }, b[3] = { 4, 0, 0 }, c[3] = { 4, 4, 0 }, d[3] = { 0, 4, 0 };
			float e[3] = { 4, 0, 0 }, f[3] = { 6, 0, 1.678f }, g[3] = { 6, 4, 1.678f }, h[3] = { 4, 4, 0 };
			float i[3] = { 6, 0, 1.678f }, j[3] = { 10, 0, 1.678f }, k[3] = { 10, 4, 1.678f }, l[3] = { 6, 4, 1.678f };

			quad(a, b, c, d);
			quad(e, f, g, h);
			quad(i, j, k, l);
		}
		seed(1, 1, 0);
		vehicle = 0;
		build();
		CHECK(!strcmp(path(1, 1, 0, 8.5f, 2, 1.678f), "walk"), "not walked up 40 degrees: %s",
			path(1, 1, 0, 8.5f, 2, 1.678f));
		bots_nav_free(&nav);
		vehicle = 1;
		build();
		{
			long node;

			for (node = 0; node < nav.node_count; node++)
				CHECK(nav.nodes[node].point.z < 0.2f, "a node on the slope or the deck (%g %g %g)",
					nav.nodes[node].point.x, nav.nodes[node].point.y, nav.nodes[node].point.z);
		}
		CHECK(node_at(1, 1, 0) >= 0 && node_at(8.5f, 2, 1.678f) < 0, "a vehicle up the slope or off the deck");
		/* (a step of 0.5: jumped up on foot; driven down off, never up) */
		reset();
		box(0, 0, -1, 6, 6, 0);
		box(6, 0, -1, 12, 6, 0.5f);
		seed(1, 1, 0);
		seed(11, 5, 0.5f);
		vehicle = 0;
		build();
		CHECK(!strcmp(path(1, 1, 0, 11, 5, 0.5f), "j"), "not jumped up: %s", path(1, 1, 0, 11, 5, 0.5f));
		bots_nav_free(&nav);
		vehicle = 1;
		build();
		CHECK(!strcmp(path(1, 1, 0, 11, 5, 0.5f), "none"), "driven up the step: %s", path(1, 1, 0, 11, 5, 0.5f));
		CHECK(!strcmp(path(11, 5, 0.5f, 1, 1, 0), "d"), "not driven off the step: %s", path(11, 5, 0.5f, 1, 1, 0));
		CHECK(nav.statistics.jump_edges == 0, "%ld jumps in a drive graph", nav.statistics.jump_edges);
		/* (and its cache: refused as a walking graph's) */
		{
			struct bots_nav loaded;
			struct bots_nav_parameters parameters;
			unsigned char *bytes;
			long size;

			bytes = bots_nav_save(&nav, 99, &size);
			CHECK(bytes && size > 0, "not saved");
			bots_nav_vehicle_parameters(&parameters);
			CHECK(bots_nav_load(&loaded, bytes, size, 99, &parameters), "not loaded");
			CHECK(loaded.node_count == nav.node_count, "loaded differently");
			bots_nav_free(&loaded);
			bots_nav_default_parameters(&parameters);
			CHECK(!bots_nav_load(&loaded, bytes, size, 99, &parameters), "a drive graph loaded as a walking graph");
			bots_nav_free_bytes(bytes);
		}
	}
	else if (!strcmp(name, "cache"))
	{
		struct bots_nav loaded;
		struct bots_nav_parameters parameters;
		unsigned char *bytes;
		long size;

		box(0, 0, -1, 5, 5, 1);
		box(5.3f, 0, -1, 10, 5, 0);
		seed(1, 1, 1);
		build();
		bots_nav_default_parameters(&parameters);
		bytes = bots_nav_save(&nav, 1234, &size);
		CHECK(bytes && size > 0, "not saved");
		CHECK(bots_nav_load(&loaded, bytes, size, 1234, &parameters), "not loaded");
		CHECK(loaded.node_count == nav.node_count && loaded.edge_count == nav.edge_count &&
			!memcmp(loaded.nodes, nav.nodes, sizeof(*nav.nodes) * nav.node_count) &&
			!memcmp(loaded.edges, nav.edges, sizeof(*nav.edges) * nav.edge_count) && loaded.main_node == nav.main_node,
			"loaded differently");
		bots_nav_free(&loaded);
		CHECK(!bots_nav_load(&loaded, bytes, size, 4321, &parameters), "another map's loaded");
		CHECK(!bots_nav_load(&loaded, bytes, size - 1, 1234, &parameters), "a short file loaded");
		parameters.jump_height += 0.1f;
		CHECK(!bots_nav_load(&loaded, bytes, size, 1234, &parameters), "other parameters' loaded");
		bots_nav_default_parameters(&parameters);
		/* (an edge's target past the nodes: refused, not read past) */
		{
			long last_edge = (long)(size - sizeof(struct bots_nav_edge));

			((struct bots_nav_edge *)(bytes + last_edge))->target = nav.node_count + 5;
			CHECK(!bots_nav_load(&loaded, bytes, size, 1234, &parameters), "a damaged graph loaded");
		}
		bots_nav_free_bytes(bytes);
	}
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	bots_nav_free(&nav);
	return 0;
}
