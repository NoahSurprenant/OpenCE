/* bots_nav.h: the bots' walking graph (bots_nav.c)

Built from a level's collision triangles (the structure BSP's and the static
scenery's, bots_nav_game.c gathers them) the way Recast builds a navigation
mesh: the triangles rasterised into columns of solid spans, the tops of
spans whose slope a biped walks and with room above for its height kept as
cells, the cells a biped's radius from an edge dropped, and each square
tile's connected pieces of cells made the graph's nodes. Nodes join where
their cells meet (a walk), and where a biped drops from one down to another
or jumps up onto one; teleporters and hand-checked links (a map's overrides)
join them too. Only what the level's spawns reach, or what reaches them, is
kept.

This file needs nothing of the engine (plain C types, the C library's
memory and exact maths only), so tools/harness can build it alone. */

#ifndef __BOTS_NAV_H
#define __BOTS_NAV_H

/* the generator's version: a graph cached by another is built again */
#define BOTS_NAV_GENERATOR_VERSION 2

/* how a bot goes along an edge */
enum
{
	_bots_nav_walk,
	/* walks off the edge, onto the node below */
	_bots_nav_drop,
	/* jumps up onto the node above */
	_bots_nav_jump,
	/* walks onto a teleporter, and is at its exit */
	_bots_nav_teleport,
	/* walks into a ladder (a climbable surface: bipeds.c walks up it as up a
	floor while facing it), and off at its top */
	_bots_nav_climb,
	/* drops down beside a ladder, facing away from it (facing it, the
	ladder would hold it: bipeds.c) */
	_bots_nav_descend,
	NUMBER_OF_BOTS_NAV_EDGE_TYPES
};

/* a node's reach from the level's main part (the spawns'): reachable from
it, and it reachable from there */
enum
{
	_bots_nav_reached_bit = 1,
	_bots_nav_reaches_bit = 2,
};

struct bots_nav_point
{
	float x, y, z;
};

struct bots_nav_parameters
{
	/* the cells' size and the heights' step (world units) */
	float cell_size;
	float cell_height;
	/* the biped: its radius and standing height, the most a floor
	slopes (as the cosine of its normal's angle to up), the step it walks
	up, the most it jumps up and drops down, and how far across (from an
	edge) a drop or jump goes */
	float agent_radius;
	float agent_height;
	float walkable_normal_z;
	float step_height;
	float jump_height;
	float drop_height;
	float link_reach;
	/* the side of a tile, whose pieces are nodes */
	float tile_size;
	/* how far around the spawns the level is looked at (0: all of it) */
	float seed_margin;
};

/* an edge a level's own table adds (a map's overrides, a teleporter) */
struct bots_nav_link
{
	struct bots_nav_point from, to;
	unsigned char type;
};

/* a ladder: the middle of its bottom and top edges, on its face, and the
way its face looks (level) */
struct bots_nav_ladder
{
	struct bots_nav_point bottom, top;
	float normal_x, normal_y;
};

struct bots_nav_input
{
	/* triangles, nine floats each, wound counterclockwise seen from in
	front (a two-sided surface both ways) */
	float const *triangles;
	long triangle_count;
	/* the spawns: what they reach, and what reaches them, is kept */
	struct bots_nav_point const *seeds;
	long seed_count;
	/* teleporters and overrides */
	struct bots_nav_link const *links;
	long link_count;
	struct bots_nav_ladder const *ladders;
	long ladder_count;
	/* whether a biped moves straight from one point to another unblocked
	(the game's collision test), for the drops and jumps found; NULL takes
	them all */
	int (*clear)(void *context, struct bots_nav_point const *from, struct bots_nav_point const *to);
	void *context;
};

struct bots_nav_node
{
	struct bots_nav_point point;
	long first_edge;
	long edge_count;
	unsigned char reach;
	unsigned char pad[3];
};

struct bots_nav_edge
{
	long target;
	float cost;
	unsigned char type;
	/* (bots_brain.c) a bot failed to go along it on this map */
	unsigned char failed;
	unsigned char pad[2];
	/* where the move starts and ends (a drop's edge, a jump's take-off) */
	struct bots_nav_point from, to;
};

/* what building found (for the log and the tests) */
struct bots_nav_statistics
{
	long columns_x, columns_y;
	long triangles;
	long cells;
	long eroded_cells;
	long nodes_before_pruning;
	long walk_edges, drop_edges, jump_edges, teleport_edges, link_edges, climb_edges, descend_edges;
	long rejected_links;
};

struct bots_nav
{
	struct bots_nav_parameters parameters;
	/* the tiles: their grid, and each one's nodes (nodes are in tile order) */
	float origin_x, origin_y;
	long tiles_x, tiles_y;
	long *tile_first;
	long node_count;
	struct bots_nav_node *nodes;
	long edge_count;
	struct bots_nav_edge *edges;
	/* the node the spawns' part was reached from */
	long main_node;
	struct bots_nav_statistics statistics;
	/* A*'s working memory (bots_nav_find_path), made when first needed */
	void *search;
};

/* the stock parameters for Halo's multiplayer biped (cyborg_mp: radius
0.2, height 0.7, 45 degree slopes, a jump about 0.69 high) */
void bots_nav_default_parameters(
	struct bots_nav_parameters *parameters);
/* the graph of a level; 0 (and nav empty) when it cannot be built (no
memory, no triangles) */
int bots_nav_build(
	struct bots_nav_input const *input,
	struct bots_nav_parameters const *parameters,
	struct bots_nav *nav);
void bots_nav_free(
	struct bots_nav *nav);
/* the node at a point: in its tile or the next ones, the nearest whose
height is within reach below or a little above; -1 for none */
long bots_nav_locate(
	struct bots_nav const *nav,
	float x,
	float y,
	float z);
/* the cheapest way from one node to another as the edges taken (written to
edges backwards: edges[count - 1] is the first), at most maximum of them;
the count, 0 if from is to, -1 if there is none (or it is longer) */
long bots_nav_find_path(
	struct bots_nav *nav,
	long from,
	long to,
	long *edges,
	long maximum);
/* the graph as bytes for the cache (bots_nav_free_bytes them), and back (0
if the bytes are not a graph of this version with these parameters and key).
This file's memory is the C library's, freed here: the game's own sources
have malloc and free of their own (debug_memory.c) */
unsigned char *bots_nav_save(
	struct bots_nav const *nav,
	unsigned long key,
	long *size);
void bots_nav_free_bytes(
	unsigned char *bytes);
int bots_nav_load(
	struct bots_nav *nav,
	unsigned char const *bytes,
	long size,
	unsigned long key,
	struct bots_nav_parameters const *parameters);

#endif
