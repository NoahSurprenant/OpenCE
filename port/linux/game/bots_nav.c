/* bots_nav.c: the bots' walking graph (bots_nav.h)

1. The level is cut into columns cell_size square, a strip of rows at a
   time; each triangle clipped to a column marks a span of it solid, and
   overlapping spans merge (Recast's rasterisation). A span is walkable when
   the triangle making its top faces up within the biped's slope.
2. The top of a walkable span with the biped's height of room above it is a
   cell. Each column's cells are kept, row by row, sorted.
3. A cell's neighbour in each of the four directions is the cell of that
   column within a step of its height. Cells less than the biped's radius
   from one with a neighbour missing (an edge, a wall) are dropped.
4. Each tile's connected pieces of cells are the nodes, at the cell nearest
   the piece's middle; nodes whose cells are neighbours are joined (walks).
   A cell at the edge, within link_reach across of a lower cell (down to
   drop_height) or a higher one (up to jump_height), joins its node to that
   cell's (a drop, a jump): the shortest such of each pair of nodes, if the
   game's collision test lets a biped move along it.
5. Teleporters and a level's own links join the nodes at their ends.
6. From the node of the spawns' main part (the part most spawns are in),
   what it reaches and what reaches it is kept; the rest goes. */

#include "bots_nav.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BOTS_NAV_STRIP_ROWS 16
#define BOTS_NAV_MAXIMUM_COLUMNS (16L * 1024L * 1024L)
#define BOTS_NAV_NONE (-1L)
#define BOTS_NAV_NO_SPAN (-1L)
/* how far across a drop or jump goes from the edge without a run at it,
and the most a running jump across a gap (up to link_reach) goes up */
#define BOTS_NAV_STEP_ACROSS 1.0f
#define BOTS_NAV_GAP_RISE 0.5f

/* ---------- growing arrays */

static int bots_nav_reserve(
	void **array,
	long *capacity,
	long count,
	long element_size)
{
	void *grown;
	long new_capacity;

	if (count <= *capacity)
		return 1;
	new_capacity = *capacity ? *capacity : 1024;
	while (new_capacity < count)
		new_capacity *= 2;
	grown = realloc(*array, (size_t)new_capacity * (size_t)element_size);
	if (!grown)
		return 0;
	*array = grown;
	*capacity = new_capacity;
	return 1;
}

/* ---------- the builder's state */

struct bots_nav_spans
{
	float *bottom;
	float *top;
	long *next;
	unsigned char *walkable;
	long count;
	long capacity;
	/* the strip's first span of each column (BOTS_NAV_NO_SPAN: none) */
	long *head;
};

struct bots_nav_cells
{
	unsigned short *x;
	unsigned short *y;
	float *z;
	long count;
	long capacity;
	/* each row's first cell (columns_y + 1 of them) */
	long *row_first;
};

struct bots_nav_builder
{
	struct bots_nav_parameters parameters;
	struct bots_nav_input const *input;
	float minimum_x, minimum_y;
	long columns_x, columns_y;
	struct bots_nav_spans spans;
	struct bots_nav_cells cells;
	/* each cell's distance (cells) from an edge, and its node */
	unsigned char *distance;
	long *node;
	long *queue;
	long erosion;
	/* nodes and edges as they are found */
	struct bots_nav_node *nodes;
	long node_count;
	long node_capacity;
	struct bots_nav_edge *edges;
	long edge_count;
	long edge_capacity;
	long tile_cells;
	long tiles_x, tiles_y;
	long *tile_first;
	struct bots_nav_statistics statistics;
};

void bots_nav_default_parameters(
	struct bots_nav_parameters *parameters)
{
	parameters->cell_size = 0.1f;
	parameters->cell_height = 0.025f;
	/* (the biped's is 0.2: a little more keeps it off walls' corners) */
	parameters->agent_radius = 0.25f;
	parameters->agent_height = 0.7f;
	/* (cos 45 degrees, the biped's maximum_slope_angle) */
	parameters->walkable_normal_z = 0.7071f;
	/* (a step a biped walks up; a bot jumps a higher one) */
	parameters->step_height = 0.3f;
	/* (a jump is about 0.69 high: in the game bots made 0.45 and 0.68 and
	missed 0.72 and 0.83, crouching too) */
	parameters->jump_height = 0.65f;
	/* (the globals' harmful falling distance is 3: no damage below it) */
	parameters->drop_height = 2.9f;
	/* (a running jump crosses about 2.5) */
	parameters->link_reach = 2.0f;
	parameters->tile_size = 1.0f;
	parameters->seed_margin = 30.0f;
}

void bots_nav_vehicle_parameters(
	struct bots_nav_parameters *parameters)
{
	bots_nav_default_parameters(parameters);
	/* (a Warthog is about 0.75 wide and 1.5 long, a Ghost narrower: half a
	world unit keeps either off walls and out of doors) */
	parameters->agent_radius = 0.5f;
	/* (a Warthog with its gunner standing) */
	parameters->agent_height = 1.0f;
	/* (cos 35 degrees: wheels slide back down steeper grass) */
	parameters->walkable_normal_z = 0.8192f;
	/* (lips and kerbs driven over; nothing jumped: a jump no higher than a
	step is none) */
	parameters->step_height = 0.3f;
	parameters->jump_height = 0.3f;
	/* (a small drop driven off, one way; no cliffs) */
	parameters->drop_height = 1.0f;
	/* (across the two radii between the cells either side of a drop) */
	parameters->link_reach = 1.5f;
	/* (open ground: bigger tiles, fewer nodes) */
	parameters->tile_size = 2.0f;
	/* (the whole level: the vehicles stand at the bases, and the ways between
	them reach far past them on Sidewinder) */
	parameters->seed_margin = 0.0f;
}

/* ---------- 1. rasterisation */

/* the polygon's points on the side of the axis-aligned line where
a * x + b * y + offset >= 0 (a and b are 1, -1 or 0); their count */
static int bots_nav_clip(
	float const *in,
	int count,
	float *out,
	float a,
	float b,
	float offset)
{
	int result = 0;
	int index;
	int previous;

	for (index = 0, previous = count - 1; index < count; previous = index, index++)
	{
		float here = a * in[index * 3] + b * in[index * 3 + 1] + offset;
		float before = a * in[previous * 3] + b * in[previous * 3 + 1] + offset;

		if ((before >= 0.0f) != (here >= 0.0f))
		{
			float t = before / (before - here);

			out[result * 3] = in[previous * 3] + (in[index * 3] - in[previous * 3]) * t;
			out[result * 3 + 1] = in[previous * 3 + 1] + (in[index * 3 + 1] - in[previous * 3 + 1]) * t;
			out[result * 3 + 2] = in[previous * 3 + 2] + (in[index * 3 + 2] - in[previous * 3 + 2]) * t;
			result++;
		}
		if (here >= 0.0f)
		{
			out[result * 3] = in[index * 3];
			out[result * 3 + 1] = in[index * 3 + 1];
			out[result * 3 + 2] = in[index * 3 + 2];
			result++;
		}
	}
	return result;
}

/* a solid span of a column of the strip, merged with those it overlaps; a
merged top is walkable if the higher of the tops within a step of each
other is (Recast's rule) */
static int bots_nav_add_span(
	struct bots_nav_builder *builder,
	long column,
	float bottom,
	float top,
	unsigned char walkable)
{
	struct bots_nav_spans *spans = &builder->spans;
	long previous = BOTS_NAV_NO_SPAN;
	long current = spans->head[column];
	float step = builder->parameters.step_height;
	long span;

	while (current != BOTS_NAV_NO_SPAN)
	{
		long next;

		if (spans->bottom[current] > top)
			break;
		if (spans->top[current] < bottom)
		{
			previous = current;
			current = spans->next[current];
			continue;
		}
		if (spans->bottom[current] < bottom)
			bottom = spans->bottom[current];
		if (spans->top[current] > top)
		{
			walkable = fabs(top - spans->top[current]) <= step ?
				(unsigned char)(walkable | spans->walkable[current]) : spans->walkable[current];
			top = spans->top[current];
		}
		else if (fabs(top - spans->top[current]) <= step)
		{
			walkable = (unsigned char)(walkable | spans->walkable[current]);
		}
		next = spans->next[current];
		if (previous == BOTS_NAV_NO_SPAN)
			spans->head[column] = next;
		else
			spans->next[previous] = next;
		/* (the merged span is no longer used: the strip's spans all go together) */
		current = next;
	}
	if (spans->count >= spans->capacity)
	{
		long capacity = spans->capacity ? spans->capacity * 2 : 65536;
		float *bottoms = (float *)realloc(spans->bottom, (size_t)capacity * sizeof(float));
		float *tops;
		long *nexts;
		unsigned char *walkables;

		if (!bottoms)
			return 0;
		spans->bottom = bottoms;
		if (!(tops = (float *)realloc(spans->top, (size_t)capacity * sizeof(float))))
			return 0;
		spans->top = tops;
		if (!(nexts = (long *)realloc(spans->next, (size_t)capacity * sizeof(long))))
			return 0;
		spans->next = nexts;
		if (!(walkables = (unsigned char *)realloc(spans->walkable, (size_t)capacity)))
			return 0;
		spans->walkable = walkables;
		spans->capacity = capacity;
	}
	span = spans->count++;
	spans->bottom[span] = bottom;
	spans->top[span] = top;
	spans->walkable[span] = walkable;
	if (previous == BOTS_NAV_NO_SPAN)
	{
		spans->next[span] = spans->head[column];
		spans->head[column] = span;
	}
	else
	{
		spans->next[span] = spans->next[previous];
		spans->next[previous] = span;
	}
	return 1;
}

/* the triangle's spans in the strip's rows */
static int bots_nav_rasterize_triangle(
	struct bots_nav_builder *builder,
	long triangle,
	long strip_first_row,
	long strip_rows)
{
	float const *points = builder->input->triangles + triangle * 9;
	float size = builder->parameters.cell_size;
	float height = builder->parameters.cell_height;
	float polygon[7 * 3], row_in[7 * 3], row_out[7 * 3], column_in[7 * 3], column_out[7 * 3];
	float ab[3], ac[3], normal[3], length;
	float lowest_x, highest_x, lowest_y, highest_y;
	long first_row, last_row, first_column, last_column, row, column;
	unsigned char walkable;
	int index;

	for (index = 0; index < 3; index++)
	{
		ab[index] = points[3 + index] - points[index];
		ac[index] = points[6 + index] - points[index];
	}
	normal[0] = ab[1] * ac[2] - ab[2] * ac[1];
	normal[1] = ab[2] * ac[0] - ab[0] * ac[2];
	normal[2] = ab[0] * ac[1] - ab[1] * ac[0];
	length = (float)sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
	if (length <= 0.0f)
		return 1;
	walkable = normal[2] / length >= builder->parameters.walkable_normal_z;

	lowest_x = highest_x = points[0];
	lowest_y = highest_y = points[1];
	for (index = 1; index < 3; index++)
	{
		if (points[index * 3] < lowest_x) lowest_x = points[index * 3];
		if (points[index * 3] > highest_x) highest_x = points[index * 3];
		if (points[index * 3 + 1] < lowest_y) lowest_y = points[index * 3 + 1];
		if (points[index * 3 + 1] > highest_y) highest_y = points[index * 3 + 1];
	}
	first_row = (long)floor((lowest_y - builder->minimum_y) / size);
	last_row = (long)floor((highest_y - builder->minimum_y) / size);
	first_column = (long)floor((lowest_x - builder->minimum_x) / size);
	last_column = (long)floor((highest_x - builder->minimum_x) / size);
	if (first_row < strip_first_row)
		first_row = strip_first_row;
	if (last_row > strip_first_row + strip_rows - 1)
		last_row = strip_first_row + strip_rows - 1;
	if (first_column < 0)
		first_column = 0;
	if (last_column > builder->columns_x - 1)
		last_column = builder->columns_x - 1;
	memcpy(polygon, points, 9 * sizeof(float));

	for (row = first_row; row <= last_row; row++)
	{
		float row_low = builder->minimum_y + (float)row * size;
		int count = bots_nav_clip(polygon, 3, row_in, 0.0f, 1.0f, -row_low);

		if (count < 3)
			continue;
		count = bots_nav_clip(row_in, count, row_out, 0.0f, -1.0f, row_low + size);
		if (count < 3)
			continue;
		for (column = first_column; column <= last_column; column++)
		{
			float column_low = builder->minimum_x + (float)column * size;
			int clipped = bots_nav_clip(row_out, count, column_in, 1.0f, 0.0f, -column_low);
			float bottom, top;

			if (clipped < 3)
				continue;
			clipped = bots_nav_clip(column_in, clipped, column_out, -1.0f, 0.0f, column_low + size);
			if (clipped < 3)
				continue;
			bottom = top = column_out[2];
			for (index = 1; index < clipped; index++)
			{
				if (column_out[index * 3 + 2] < bottom) bottom = column_out[index * 3 + 2];
				if (column_out[index * 3 + 2] > top) top = column_out[index * 3 + 2];
			}
			bottom = (float)floor(bottom / height) * height;
			top = (float)ceil(top / height) * height;
			if (!bots_nav_add_span(builder, (row - strip_first_row) * builder->columns_x + column, bottom, top, walkable))
				return 0;
		}
	}
	return 1;
}

/* ---------- 2. cells */

static int bots_nav_add_cell(
	struct bots_nav_builder *builder,
	long x,
	long y,
	float z)
{
	struct bots_nav_cells *cells = &builder->cells;

	if (cells->count >= cells->capacity)
	{
		long capacity = cells->capacity ? cells->capacity * 2 : 65536;
		unsigned short *xs = (unsigned short *)realloc(cells->x, (size_t)capacity * sizeof(unsigned short));
		unsigned short *ys;
		float *zs;

		if (!xs)
			return 0;
		cells->x = xs;
		if (!(ys = (unsigned short *)realloc(cells->y, (size_t)capacity * sizeof(unsigned short))))
			return 0;
		cells->y = ys;
		if (!(zs = (float *)realloc(cells->z, (size_t)capacity * sizeof(float))))
			return 0;
		cells->z = zs;
		cells->capacity = capacity;
	}
	cells->x[cells->count] = (unsigned short)x;
	cells->y[cells->count] = (unsigned short)y;
	cells->z[cells->count] = z;
	cells->count++;
	return 1;
}

static int bots_nav_compare_longs(
	void const *a,
	void const *b)
{
	long x = *(long const *)a, y = *(long const *)b;

	return x < y ? -1 : x > y;
}

/* the triangles a strip at a time: their spans, then the strip's cells */
static int bots_nav_rasterize(
	struct bots_nav_builder *builder)
{
	struct bots_nav_input const *input = builder->input;
	long *order;
	long *keys;
	long *active;
	long active_count = 0;
	long next_triangle = 0;
	long strip;
	long index;

	order = (long *)malloc((size_t)(input->triangle_count ? input->triangle_count : 1) * 2 * sizeof(long));
	active = (long *)malloc((size_t)(input->triangle_count ? input->triangle_count : 1) * sizeof(long));
	builder->spans.head = (long *)malloc((size_t)BOTS_NAV_STRIP_ROWS * (size_t)builder->columns_x * sizeof(long));
	builder->cells.row_first = (long *)malloc((size_t)(builder->columns_y + 1) * sizeof(long));
	if (!order || !active || !builder->spans.head || !builder->cells.row_first)
	{
		free(order);
		free(active);
		return 0;
	}
	/* (the triangles by their first row: pairs of row and index, sorted) */
	keys = order;
	for (index = 0; index < input->triangle_count; index++)
	{
		float const *points = input->triangles + index * 9;
		float lowest_y = points[1];

		if (points[4] < lowest_y) lowest_y = points[4];
		if (points[7] < lowest_y) lowest_y = points[7];
		keys[index * 2] = (long)floor((lowest_y - builder->minimum_y) / builder->parameters.cell_size);
		keys[index * 2 + 1] = index;
	}
	qsort(keys, (size_t)input->triangle_count, 2 * sizeof(long), bots_nav_compare_longs);

	for (strip = 0; strip < builder->columns_y; strip += BOTS_NAV_STRIP_ROWS)
	{
		long rows = builder->columns_y - strip < BOTS_NAV_STRIP_ROWS ? builder->columns_y - strip : BOTS_NAV_STRIP_ROWS;
		long kept = 0;
		long row;

		builder->spans.count = 0;
		for (index = 0; index < rows * builder->columns_x; index++)
			builder->spans.head[index] = BOTS_NAV_NO_SPAN;
		while (next_triangle < input->triangle_count && keys[next_triangle * 2] < strip + rows)
			active[active_count++] = keys[next_triangle++ * 2 + 1];
		for (index = 0; index < active_count; index++)
		{
			float const *points = input->triangles + active[index] * 9;
			float highest_y = points[1];

			if (points[4] > highest_y) highest_y = points[4];
			if (points[7] > highest_y) highest_y = points[7];
			/* (a triangle wholly before this strip is done with) */
			if ((long)floor((highest_y - builder->minimum_y) / builder->parameters.cell_size) < strip)
				continue;
			active[kept++] = active[index];
			if (!bots_nav_rasterize_triangle(builder, active[index], strip, rows))
			{
				free(order);
				free(active);
				return 0;
			}
		}
		active_count = kept;
		for (row = 0; row < rows; row++)
		{
			long x;

			builder->cells.row_first[strip + row] = builder->cells.count;
			for (x = 0; x < builder->columns_x; x++)
			{
				long span;

				for (span = builder->spans.head[row * builder->columns_x + x]; span != BOTS_NAV_NO_SPAN;
					span = builder->spans.next[span])
				{
					long above = builder->spans.next[span];
					float room = above == BOTS_NAV_NO_SPAN ? 1.0e9f :
						builder->spans.bottom[above] - builder->spans.top[span];

					if (builder->spans.walkable[span] && room >= builder->parameters.agent_height &&
						!bots_nav_add_cell(builder, x, strip + row, builder->spans.top[span]))
					{
						free(order);
						free(active);
						return 0;
					}
				}
			}
		}
	}
	builder->cells.row_first[builder->columns_y] = builder->cells.count;
	free(order);
	free(active);
	free(builder->spans.bottom);
	free(builder->spans.top);
	free(builder->spans.next);
	free(builder->spans.walkable);
	free(builder->spans.head);
	memset(&builder->spans, 0, sizeof(builder->spans));
	builder->statistics.cells = builder->cells.count;
	return 1;
}

/* the first cell of the column, and the count of its cells */
static long bots_nav_column(
	struct bots_nav_builder const *builder,
	long x,
	long y,
	long *count)
{
	struct bots_nav_cells const *cells = &builder->cells;
	long low, high, first;

	*count = 0;
	if (x < 0 || y < 0 || x >= builder->columns_x || y >= builder->columns_y)
		return BOTS_NAV_NONE;
	low = cells->row_first[y];
	high = cells->row_first[y + 1];
	while (low < high)
	{
		long middle = (low + high) / 2;

		if (cells->x[middle] < x)
			low = middle + 1;
		else
			high = middle;
	}
	first = low;
	while (low < cells->row_first[y + 1] && cells->x[low] == x)
		low++;
	*count = low - first;
	return first;
}

static long const bots_nav_dx[4] = { 1, -1, 0, 0 };
static long const bots_nav_dy[4] = { 0, 0, 1, -1 };

/* ---------- 3. neighbours and erosion */

/* the cell's neighbour in a direction: that column's cell within a step of
its height, the nearest; BOTS_NAV_NONE for none */
static long bots_nav_neighbor(
	struct bots_nav_builder const *builder,
	long cell,
	int direction)
{
	struct bots_nav_cells const *cells = &builder->cells;
	long count;
	long first = bots_nav_column(builder, (long)cells->x[cell] + bots_nav_dx[direction],
		(long)cells->y[cell] + bots_nav_dy[direction], &count);
	long best = BOTS_NAV_NONE;
	float best_step = builder->parameters.step_height;
	long index;

	for (index = first; index < first + count; index++)
	{
		float step = (float)fabs(cells->z[index] - cells->z[cell]);

		if (step <= best_step)
		{
			best_step = step;
			best = index;
		}
	}
	return best;
}

/* each cell's distance in cells from one missing a neighbour, up to the
radius (a breadth-first search from those) */
static int bots_nav_erode(
	struct bots_nav_builder *builder)
{
	long count = builder->cells.count;
	long head = 0, tail = 0;
	long cell;

	builder->distance = (unsigned char *)malloc((size_t)(count ? count : 1));
	builder->queue = (long *)malloc((size_t)(count ? count : 1) * sizeof(long));
	if (!builder->distance || !builder->queue)
		return 0;
	builder->erosion = (long)ceil(builder->parameters.agent_radius / builder->parameters.cell_size - 0.001f);
	for (cell = 0; cell < count; cell++)
	{
		int direction;

		builder->distance[cell] = 255;
		for (direction = 0; direction < 4; direction++)
		{
			if (bots_nav_neighbor(builder, cell, direction) == BOTS_NAV_NONE)
			{
				builder->distance[cell] = 0;
				builder->queue[tail++] = cell;
				break;
			}
		}
	}
	while (head < tail)
	{
		long here = builder->queue[head++];
		int direction;

		if (builder->distance[here] >= builder->erosion)
			continue;
		for (direction = 0; direction < 4; direction++)
		{
			long there = bots_nav_neighbor(builder, here, direction);

			if (there != BOTS_NAV_NONE && builder->distance[there] > builder->distance[here] + 1)
			{
				builder->distance[there] = (unsigned char)(builder->distance[here] + 1);
				builder->queue[tail++] = there;
			}
		}
	}
	builder->statistics.eroded_cells = 0;
	for (cell = 0; cell < count; cell++)
	{
		if (builder->distance[cell] >= builder->erosion)
			builder->statistics.eroded_cells++;
	}
	return 1;
}

#define BOTS_NAV_CELL_OK(builder, cell) ((builder)->distance[cell] >= (builder)->erosion)

/* ---------- 4. nodes and edges */

static struct bots_nav_point bots_nav_cell_point(
	struct bots_nav_builder const *builder,
	long cell)
{
	struct bots_nav_point point;

	point.x = builder->minimum_x + ((float)builder->cells.x[cell] + 0.5f) * builder->parameters.cell_size;
	point.y = builder->minimum_y + ((float)builder->cells.y[cell] + 0.5f) * builder->parameters.cell_size;
	point.z = builder->cells.z[cell];
	return point;
}

static float bots_nav_distance(
	struct bots_nav_point const *a,
	struct bots_nav_point const *b)
{
	float dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;

	return (float)sqrt(dx * dx + dy * dy + dz * dz);
}

/* each tile's connected pieces of cells, nodes in tile order */
static int bots_nav_make_nodes(
	struct bots_nav_builder *builder)
{
	long count = builder->cells.count;
	long tile_x, tile_y;
	long cell;

	builder->node = (long *)malloc((size_t)(count ? count : 1) * sizeof(long));
	builder->tile_cells = (long)floor(builder->parameters.tile_size / builder->parameters.cell_size + 0.5f);
	if (builder->tile_cells < 1)
		builder->tile_cells = 1;
	builder->tiles_x = (builder->columns_x + builder->tile_cells - 1) / builder->tile_cells;
	builder->tiles_y = (builder->columns_y + builder->tile_cells - 1) / builder->tile_cells;
	builder->tile_first = (long *)malloc((size_t)(builder->tiles_x * builder->tiles_y + 1) * sizeof(long));
	if (!builder->node || !builder->tile_first)
		return 0;
	for (cell = 0; cell < count; cell++)
		builder->node[cell] = BOTS_NAV_NONE;
	for (tile_y = 0; tile_y < builder->tiles_y; tile_y++)
	{
		for (tile_x = 0; tile_x < builder->tiles_x; tile_x++)
		{
			long row;

			builder->tile_first[tile_y * builder->tiles_x + tile_x] = builder->node_count;
			for (row = tile_y * builder->tile_cells;
				row < (tile_y + 1) * builder->tile_cells && row < builder->columns_y; row++)
			{
				long first, ignored;

				first = bots_nav_column(builder, tile_x * builder->tile_cells, row, &ignored);
				if (first == BOTS_NAV_NONE)
					continue;
				/* (the row's cells from the tile's first column: the search
				lands on the first at or past it) */
				{
					long low = builder->cells.row_first[row], high = builder->cells.row_first[row + 1];

					while (low < high)
					{
						long middle = (low + high) / 2;

						if (builder->cells.x[middle] < tile_x * builder->tile_cells)
							low = middle + 1;
						else
							high = middle;
					}
					first = low;
				}
				for (cell = first; cell < builder->cells.row_first[row + 1] &&
					builder->cells.x[cell] < (tile_x + 1) * builder->tile_cells; cell++)
				{
					long head = 0, tail = 0;
					float sum_x = 0.0f, sum_y = 0.0f;
					long best = cell;
					float best_distance = 1.0e30f;
					long index;
					struct bots_nav_node *node;

					if (!BOTS_NAV_CELL_OK(builder, cell) || builder->node[cell] != BOTS_NAV_NONE)
						continue;
					if (!bots_nav_reserve((void **)&builder->nodes, &builder->node_capacity, builder->node_count + 1,
						sizeof(struct bots_nav_node)))
					{
						return 0;
					}
					builder->node[cell] = builder->node_count;
					builder->queue[tail++] = cell;
					while (head < tail)
					{
						long here = builder->queue[head++];
						int direction;

						sum_x += (float)builder->cells.x[here];
						sum_y += (float)builder->cells.y[here];
						for (direction = 0; direction < 4; direction++)
						{
							long there = bots_nav_neighbor(builder, here, direction);

							if (there != BOTS_NAV_NONE && BOTS_NAV_CELL_OK(builder, there) &&
								builder->node[there] == BOTS_NAV_NONE &&
								builder->cells.x[there] / builder->tile_cells == tile_x &&
								builder->cells.y[there] / builder->tile_cells == tile_y)
							{
								builder->node[there] = builder->node_count;
								builder->queue[tail++] = there;
							}
						}
					}
					sum_x /= (float)tail;
					sum_y /= (float)tail;
					for (index = 0; index < tail; index++)
					{
						float dx = (float)builder->cells.x[builder->queue[index]] - sum_x;
						float dy = (float)builder->cells.y[builder->queue[index]] - sum_y;

						if (dx * dx + dy * dy < best_distance)
						{
							best_distance = dx * dx + dy * dy;
							best = builder->queue[index];
						}
					}
					node = &builder->nodes[builder->node_count++];
					memset(node, 0, sizeof(*node));
					node->point = bots_nav_cell_point(builder, best);
				}
			}
		}
	}
	builder->tile_first[builder->tiles_x * builder->tiles_y] = builder->node_count;
	builder->statistics.nodes_before_pruning = builder->node_count;
	return 1;
}

/* an edge as found: its nodes, its kind, its ends, and how far it goes
across (the shortest of a pair is kept) */
struct bots_nav_candidate
{
	long from_node;
	long to_node;
	long type;
	float across;
	struct bots_nav_point from, to;
};

static int bots_nav_compare_candidates(
	void const *a,
	void const *b)
{
	struct bots_nav_candidate const *x = (struct bots_nav_candidate const *)a;
	struct bots_nav_candidate const *y = (struct bots_nav_candidate const *)b;

	if (x->from_node != y->from_node)
		return x->from_node < y->from_node ? -1 : 1;
	if (x->to_node != y->to_node)
		return x->to_node < y->to_node ? -1 : 1;
	if (x->type != y->type)
		return x->type < y->type ? -1 : 1;
	return x->across < y->across ? -1 : x->across > y->across;
}

struct bots_nav_candidates
{
	struct bots_nav_candidate *items;
	long count;
	long capacity;
};

static int bots_nav_add_candidate(
	struct bots_nav_candidates *candidates,
	long from_node,
	long to_node,
	long type,
	float across,
	struct bots_nav_point const *from,
	struct bots_nav_point const *to)
{
	struct bots_nav_candidate *candidate;

	if (!bots_nav_reserve((void **)&candidates->items, &candidates->capacity, candidates->count + 1,
		sizeof(struct bots_nav_candidate)))
	{
		return 0;
	}
	candidate = &candidates->items[candidates->count++];
	candidate->from_node = from_node;
	candidate->to_node = to_node;
	candidate->type = type;
	candidate->across = across;
	candidate->from = *from;
	candidate->to = *to;
	return 1;
}

/* whether a drop or a jump passes by a ladder (which would catch a biped
facing it): those go by the ladder's own edges */
static int bots_nav_near_ladder(
	struct bots_nav_builder const *builder,
	struct bots_nav_candidate const *candidate)
{
	float low = candidate->from.z < candidate->to.z ? candidate->from.z : candidate->to.z;
	float high = candidate->from.z > candidate->to.z ? candidate->from.z : candidate->to.z;
	float sx = candidate->to.x - candidate->from.x, sy = candidate->to.y - candidate->from.y;
	float length_squared = sx * sx + sy * sy;
	long index;

	for (index = 0; index < builder->input->ladder_count; index++)
	{
		struct bots_nav_ladder const *ladder = &builder->input->ladders[index];
		float t = length_squared > 0.0001f ?
			((ladder->bottom.x - candidate->from.x) * sx + (ladder->bottom.y - candidate->from.y) * sy) / length_squared : 0.0f;
		float px, py;

		if (ladder->top.z < low - 0.1f || ladder->bottom.z > high + 0.1f)
			continue;
		t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
		px = candidate->from.x + sx * t - ladder->bottom.x;
		py = candidate->from.y + sy * t - ladder->bottom.y;
		if (px * px + py * py < 0.8f * 0.8f)
			return 1;
	}
	return 0;
}

/* whether a drop or a jump is clear: across at the higher height, and
down or up at the far side */
static int bots_nav_link_clear(
	struct bots_nav_builder const *builder,
	struct bots_nav_candidate const *candidate)
{
	struct bots_nav_input const *input = builder->input;
	float top = candidate->from.z > candidate->to.z ? candidate->from.z : candidate->to.z;
	float const heights[3] = { 0.1f, 0.35f, 0.65f };
	int index;

	if (!input->clear)
		return 1;
	/* (across at the higher floor's height, at the biped's feet, middle and
	head: a railing stops it as a wall does; then down, or up, the far or
	near side) */
	for (index = 0; index < 3; index++)
	{
		struct bots_nav_point start, corner, end;

		start = candidate->from;
		start.z += heights[index];
		end = candidate->to;
		end.z += heights[index];
		if (candidate->type == _bots_nav_drop)
		{
			corner.x = candidate->to.x;
			corner.y = candidate->to.y;
			corner.z = top + heights[index];
		}
		else
		{
			corner.x = candidate->from.x;
			corner.y = candidate->from.y;
			corner.z = top + heights[index] + 0.1f;
		}
		if (!input->clear(input->context, &start, &corner) || !input->clear(input->context, &corner, &end))
			return 0;
	}
	return 1;
}

/* the edges: walks between neighbouring cells' nodes, drops and jumps
from the cells at the edge, the input's links; then each node's, in order */
static int bots_nav_make_edges(
	struct bots_nav_builder *builder,
	struct bots_nav *nav)
{
	struct bots_nav_candidates candidates;
	long count = builder->cells.count;
	long reach = (long)ceil(builder->parameters.link_reach / builder->parameters.cell_size);
	float size = builder->parameters.cell_size;
	long cell;
	long index;

	memset(&candidates, 0, sizeof(candidates));
	for (cell = 0; cell < count; cell++)
	{
		struct bots_nav_point here;
		int direction;

		if (!BOTS_NAV_CELL_OK(builder, cell))
			continue;
		here = bots_nav_cell_point(builder, cell);
		for (direction = 0; direction < 4; direction++)
		{
			long there = bots_nav_neighbor(builder, cell, direction);

			/* (both ways: a cell's neighbour is its column's nearest in
			height, which from the other side may be another cell of this
			column, a floor and a pad on it) */
			if (there != BOTS_NAV_NONE && BOTS_NAV_CELL_OK(builder, there) && builder->node[there] != builder->node[cell])
			{
				if (!bots_nav_add_candidate(&candidates, builder->node[cell], builder->node[there], _bots_nav_walk, 0.0f,
					&builder->nodes[builder->node[cell]].point, &builder->nodes[builder->node[there]].point) ||
					!bots_nav_add_candidate(&candidates, builder->node[there], builder->node[cell], _bots_nav_walk, 0.0f,
					&builder->nodes[builder->node[there]].point, &builder->nodes[builder->node[cell]].point))
				{
					free(candidates.items);
					return 0;
				}
			}
		}
		/* (drops and jumps start at the edge of what is walkable) */
		if (builder->distance[cell] != builder->erosion)
			continue;
		{
			long x0 = (long)builder->cells.x[cell], y0 = (long)builder->cells.y[cell];
			long x, y;

			for (y = y0 - reach; y <= y0 + reach; y++)
			{
				for (x = x0 - reach; x <= x0 + reach; x++)
				{
					long column_count;
					long first = (x == x0 && y == y0) ? BOTS_NAV_NONE : bots_nav_column(builder, x, y, &column_count);
					float across = (float)sqrt((float)((x - x0) * (x - x0) + (y - y0) * (y - y0))) * size;

					if (first == BOTS_NAV_NONE || across > builder->parameters.link_reach)
						continue;
					for (index = first; index < first + column_count; index++)
					{
						float rise = builder->cells.z[index] - builder->cells.z[cell];
						long type = rise < -builder->parameters.step_height && rise >= -builder->parameters.drop_height ?
							_bots_nav_drop : rise > builder->parameters.step_height && rise <= builder->parameters.jump_height ?
							_bots_nav_jump : -1;
						struct bots_nav_point there;

						if (type < 0 || !BOTS_NAV_CELL_OK(builder, index) || builder->node[index] == builder->node[cell])
							continue;
						/* (across a gap, a running jump: up a little at most) */
						if (across > BOTS_NAV_STEP_ACROSS && type == _bots_nav_jump && rise > BOTS_NAV_GAP_RISE)
							continue;
						there = bots_nav_cell_point(builder, index);
						if (!bots_nav_add_candidate(&candidates, builder->node[cell], builder->node[index], type, across,
							&here, &there))
						{
							free(candidates.items);
							return 0;
						}
					}
				}
			}
		}
	}

	/* (the input's links: teleporters, a level's own) */
	for (index = 0; index < builder->input->link_count; index++)
	{
		struct bots_nav_link const *link = &builder->input->links[index];
		long from_node = bots_nav_locate(nav, link->from.x, link->from.y, link->from.z);
		long to_node = bots_nav_locate(nav, link->to.x, link->to.y, link->to.z);

		if (from_node < 0 || to_node < 0 || from_node == to_node)
		{
			builder->statistics.rejected_links++;
			continue;
		}
		if (!bots_nav_add_candidate(&candidates, from_node, to_node, (long)link->type + NUMBER_OF_BOTS_NAV_EDGE_TYPES,
			-1.0f, &link->from, &link->to))
		{
			free(candidates.items);
			return 0;
		}
	}

	/* (ladders: from the floor before the bottom, to the floor at the top,
	behind the ladder or before it; the way down is a drop) */
	for (index = 0; index < builder->input->ladder_count; index++)
	{
		struct bots_nav_ladder const *ladder = &builder->input->ladders[index];
		struct bots_nav_point foot, face;
		long from_node, to_node = BOTS_NAV_NONE;
		int side;

		foot.x = ladder->bottom.x + ladder->normal_x * 0.35f;
		foot.y = ladder->bottom.y + ladder->normal_y * 0.35f;
		foot.z = ladder->bottom.z + 0.05f;
		from_node = bots_nav_locate(nav, foot.x, foot.y, foot.z);
		for (side = 0; side < 2 && to_node == BOTS_NAV_NONE; side++)
		{
			float way = side ? 0.45f : -0.45f;
			long node = bots_nav_locate(nav, ladder->top.x + ladder->normal_x * way, ladder->top.y + ladder->normal_y * way,
				ladder->top.z - 0.4f);

			/* (a ladder may stand up past the floor it leads to) */
			if (node >= 0 && nav->nodes[node].point.z > ladder->top.z - 1.0f && nav->nodes[node].point.z < ladder->top.z + 0.3f)
				to_node = node;
		}
		if (from_node < 0 || to_node < 0 || from_node == to_node)
		{
			builder->statistics.rejected_links++;
			continue;
		}
		/* (its start the ladder's face: a bot walks into it, which climbs) */
		face = ladder->bottom;
		face.z += 0.05f;
		if (!bots_nav_add_candidate(&candidates, from_node, to_node, _bots_nav_climb + NUMBER_OF_BOTS_NAV_EDGE_TYPES, -1.0f,
			&face, &nav->nodes[to_node].point))
		{
			free(candidates.items);
			return 0;
		}
		/* (and down: over the top of its face, to the foot, its back to it) */
		face.z = nav->nodes[to_node].point.z;
		if (!bots_nav_add_candidate(&candidates, to_node, from_node, _bots_nav_descend + NUMBER_OF_BOTS_NAV_EDGE_TYPES, -1.0f,
			&face, &foot))
		{
			free(candidates.items);
			return 0;
		}
	}

	/* each pair's: a walk if there is one, else its shortest drop or jump
	that is clear; and every given link. Sorted by the node they leave,
	the edges come out in each node's range */
	qsort(candidates.items, (size_t)candidates.count, sizeof(struct bots_nav_candidate), bots_nav_compare_candidates);
	builder->edge_count = 0;
	index = 0;
	while (index < candidates.count)
	{
		long group_end = index;
		int walked = 0;
		int linked = 0;

		while (group_end < candidates.count && candidates.items[group_end].from_node == candidates.items[index].from_node &&
			candidates.items[group_end].to_node == candidates.items[index].to_node)
		{
			group_end++;
		}
		for (; index < group_end; index++)
		{
			struct bots_nav_candidate const *candidate = &candidates.items[index];
			int given = candidate->type >= NUMBER_OF_BOTS_NAV_EDGE_TYPES;
			long type = given ? candidate->type - NUMBER_OF_BOTS_NAV_EDGE_TYPES : candidate->type;
			struct bots_nav_node const *from_node = &builder->nodes[candidate->from_node];
			struct bots_nav_node const *to_node = &builder->nodes[candidate->to_node];
			struct bots_nav_edge *edge;

			if (!given)
			{
				if (type == _bots_nav_walk)
				{
					if (walked)
						continue;
					walked = 1;
				}
				else
				{
					if (walked || linked)
						continue;
					if (bots_nav_near_ladder(builder, candidate) || !bots_nav_link_clear(builder, candidate))
					{
						builder->statistics.rejected_links++;
						continue;
					}
					linked = 1;
				}
			}
			if (!bots_nav_reserve((void **)&builder->edges, &builder->edge_capacity, builder->edge_count + 1,
				sizeof(struct bots_nav_edge)))
			{
				free(candidates.items);
				return 0;
			}
			edge = &builder->edges[builder->edge_count++];
			memset(edge, 0, sizeof(*edge));
			edge->target = candidate->to_node;
			edge->type = (unsigned char)type;
			edge->from = candidate->from;
			edge->to = candidate->to;
			if (type == _bots_nav_walk)
			{
				edge->cost = bots_nav_distance(&from_node->point, &to_node->point);
				builder->statistics.walk_edges++;
			}
			else
			{
				/* (the way to its start and from its end, the move itself, and
				a little more for the time it takes) */
				edge->cost = bots_nav_distance(&from_node->point, &candidate->from) +
					bots_nav_distance(&candidate->from, &candidate->to) +
					bots_nav_distance(&candidate->to, &to_node->point) +
					(type == _bots_nav_jump ? 1.5f : type == _bots_nav_drop ? 0.5f : 1.0f);
				if (given)
					builder->statistics.link_edges += type != _bots_nav_teleport && type != _bots_nav_climb &&
						type != _bots_nav_descend;
				if (type == _bots_nav_climb)
					builder->statistics.climb_edges++;
				if (type == _bots_nav_descend)
					builder->statistics.descend_edges++;
				if (type == _bots_nav_drop)
					builder->statistics.drop_edges++;
				else if (type == _bots_nav_jump)
					builder->statistics.jump_edges++;
				else if (type == _bots_nav_teleport)
					builder->statistics.teleport_edges++;
			}
			if (builder->nodes[candidate->from_node].edge_count == 0)
				builder->nodes[candidate->from_node].first_edge = builder->edge_count - 1;
			builder->nodes[candidate->from_node].edge_count++;
		}
	}
	free(candidates.items);
	return 1;
}

/* ---------- 6. what the spawns reach */

/* the nodes reachable from the start along the edges, marked with the bit */
static void bots_nav_flood(
	struct bots_nav_node *nodes,
	struct bots_nav_edge const *edges,
	long start,
	unsigned char bit,
	long *queue)
{
	long head = 0, tail = 0;

	nodes[start].reach |= bit;
	queue[tail++] = start;
	while (head < tail)
	{
		long here = queue[head++];
		long index;

		for (index = nodes[here].first_edge; index < nodes[here].first_edge + nodes[here].edge_count; index++)
		{
			long there = edges[index].target;

			if (!(nodes[there].reach & bit))
			{
				nodes[there].reach |= bit;
				queue[tail++] = there;
			}
		}
	}
}

/* each node's sources (the nodes with an edge to it), for flooding backwards */
struct bots_nav_sources
{
	long *first;
	long *nodes;
};

static int bots_nav_sources_make(
	struct bots_nav_sources *sources,
	struct bots_nav_node const *nodes,
	long node_count,
	struct bots_nav_edge const *edges,
	long edge_count)
{
	long *fill = (long *)malloc((size_t)(node_count ? node_count : 1) * sizeof(long));
	long node, index;

	sources->first = (long *)calloc((size_t)node_count + 1, sizeof(long));
	sources->nodes = (long *)malloc((size_t)(edge_count ? edge_count : 1) * sizeof(long));
	if (!fill || !sources->first || !sources->nodes)
	{
		free(fill);
		return 0;
	}
	for (index = 0; index < edge_count; index++)
		sources->first[edges[index].target + 1]++;
	for (node = 0; node < node_count; node++)
		sources->first[node + 1] += sources->first[node];
	for (node = 0; node < node_count; node++)
		fill[node] = sources->first[node];
	for (node = 0; node < node_count; node++)
	{
		for (index = nodes[node].first_edge; index < nodes[node].first_edge + nodes[node].edge_count; index++)
			sources->nodes[fill[edges[index].target]++] = node;
	}
	free(fill);
	return 1;
}

/* the nodes from which the start is reachable, marked with the bit */
static void bots_nav_flood_backwards(
	struct bots_nav_node *nodes,
	struct bots_nav_sources const *sources,
	long start,
	unsigned char bit,
	long *queue)
{
	long head = 0, tail = 0;

	nodes[start].reach |= bit;
	queue[tail++] = start;
	while (head < tail)
	{
		long here = queue[head++];
		long index;

		for (index = sources->first[here]; index < sources->first[here + 1]; index++)
		{
			long there = sources->nodes[index];

			if (!(nodes[there].reach & bit))
			{
				nodes[there].reach |= bit;
				queue[tail++] = there;
			}
		}
	}
}

/* each node's reach from the start: reached from it, reaching it */
static void bots_nav_reach_from(
	struct bots_nav_builder *builder,
	struct bots_nav_sources const *sources,
	long start,
	long *queue)
{
	long node;

	for (node = 0; node < builder->node_count; node++)
		builder->nodes[node].reach = 0;
	bots_nav_flood(builder->nodes, builder->edges, start, _bots_nav_reached_bit, queue);
	bots_nav_flood_backwards(builder->nodes, sources, start, _bots_nav_reaches_bit, queue);
}

/* the main node: of the spawns' nodes, the one most spawns can go to and
come back from (on a map whose spawns stand on a roof as well as below it,
the roof reaches all of them by dropping, but only its own come back); what
it reaches, and what reaches it, kept, the rest dropped and the nodes and
edges numbered again */
static int bots_nav_prune(
	struct bots_nav_builder *builder,
	struct bots_nav *nav)
{
	struct bots_nav_sources sources;
	long *queue = (long *)malloc((size_t)(builder->node_count ? builder->node_count : 1) * sizeof(long));
	long *renumber = (long *)malloc((size_t)(builder->node_count ? builder->node_count : 1) * sizeof(long));
	long *seed_nodes = (long *)malloc((size_t)(builder->input->seed_count + 1) * sizeof(long));
	unsigned char *tried = (unsigned char *)calloc((size_t)(builder->input->seed_count + 1), 1);
	long best_seed_node = BOTS_NAV_NONE;
	long best_reached = -1;
	long seed;
	long node, index, kept_nodes = 0, kept_edges = 0;
	int made;

	memset(&sources, 0, sizeof(sources));
	made = queue && renumber && seed_nodes && tried &&
		bots_nav_sources_make(&sources, builder->nodes, builder->node_count, builder->edges, builder->edge_count);
	if (!made)
	{
		free(queue);
		free(renumber);
		free(seed_nodes);
		free(tried);
		free(sources.first);
		free(sources.nodes);
		return 0;
	}
	for (seed = 0; seed < builder->input->seed_count; seed++)
	{
		struct bots_nav_point const *point = &builder->input->seeds[seed];

		seed_nodes[seed] = bots_nav_locate(nav, point->x, point->y, point->z);
	}
	for (seed = 0; seed < builder->input->seed_count; seed++)
	{
		long reached = 0;
		long other;

		/* (a seed both ways from one tried before has its part: the same count) */
		if (seed_nodes[seed] < 0 || tried[seed])
			continue;
		bots_nav_reach_from(builder, &sources, seed_nodes[seed], queue);
		for (other = 0; other < builder->input->seed_count; other++)
		{
			if (seed_nodes[other] >= 0 &&
				builder->nodes[seed_nodes[other]].reach == (_bots_nav_reached_bit | _bots_nav_reaches_bit))
			{
				reached++;
				tried[other] = 1;
			}
		}
		if (reached > best_reached)
		{
			best_reached = reached;
			best_seed_node = seed_nodes[seed];
		}
	}
	free(seed_nodes);
	free(tried);
	if (best_seed_node == BOTS_NAV_NONE)
	{
		/* (no spawns given, or none on the graph: all of it kept) */
		for (node = 0; node < builder->node_count; node++)
			builder->nodes[node].reach = _bots_nav_reached_bit | _bots_nav_reaches_bit;
		nav->main_node = builder->node_count ? 0 : BOTS_NAV_NONE;
		free(queue);
		free(renumber);
		free(sources.first);
		free(sources.nodes);
		return 1;
	}
	bots_nav_reach_from(builder, &sources, best_seed_node, queue);
	free(sources.first);
	free(sources.nodes);
	/* (kept nodes stay in tile order) */
	for (node = 0; node < builder->node_count; node++)
		renumber[node] = builder->nodes[node].reach ? kept_nodes++ : BOTS_NAV_NONE;
	{
		long tile;

		for (tile = 0; tile <= builder->tiles_x * builder->tiles_y; tile++)
		{
			long first = builder->tile_first[tile];

			while (first < builder->node_count && renumber[first] == BOTS_NAV_NONE)
				first++;
			builder->tile_first[tile] = first < builder->node_count ? renumber[first] : kept_nodes;
		}
	}
	for (node = 0; node < builder->node_count; node++)
	{
		struct bots_nav_node moved = builder->nodes[node];
		long first = kept_edges;

		if (renumber[node] == BOTS_NAV_NONE)
			continue;
		for (index = moved.first_edge; index < moved.first_edge + moved.edge_count; index++)
		{
			if (renumber[builder->edges[index].target] == BOTS_NAV_NONE)
				continue;
			builder->edges[kept_edges] = builder->edges[index];
			builder->edges[kept_edges].target = renumber[builder->edges[index].target];
			kept_edges++;
		}
		moved.first_edge = first;
		moved.edge_count = kept_edges - first;
		builder->nodes[renumber[node]] = moved;
	}
	nav->main_node = renumber[best_seed_node];
	builder->node_count = kept_nodes;
	builder->edge_count = kept_edges;
	free(queue);
	free(renumber);
	return 1;
}

/* ---------- the graph */

long bots_nav_locate(
	struct bots_nav const *nav,
	float x,
	float y,
	float z)
{
	float size = nav->parameters.tile_size;
	long tile_x = (long)floor((x - nav->origin_x) / size);
	long tile_y = (long)floor((y - nav->origin_y) / size);
	long best = BOTS_NAV_NONE;
	float best_distance = 1.0e30f;
	long ty, tx;

	if (!nav->nodes || !nav->tile_first)
		return BOTS_NAV_NONE;
	/* (the tile and the eight around it: a point near a tile's edge may be
	nearest another's node) */
	for (ty = tile_y - 1; ty <= tile_y + 1; ty++)
	{
		for (tx = tile_x - 1; tx <= tile_x + 1; tx++)
		{
			long index;

			if (tx < 0 || ty < 0 || tx >= nav->tiles_x || ty >= nav->tiles_y)
				continue;
			for (index = nav->tile_first[ty * nav->tiles_x + tx]; index < nav->tile_first[ty * nav->tiles_x + tx + 1]; index++)
			{
				struct bots_nav_point const *point = &nav->nodes[index].point;
				float dx = point->x - x, dy = point->y - y, dz = z - point->z;
				float distance;

				/* (a floor up to a biped's height below the point, or a step above) */
				if (dz < -0.5f || dz > 1.5f)
					continue;
				distance = dx * dx + dy * dy + dz * dz * 0.25f;
				if (distance < best_distance)
				{
					best_distance = distance;
					best = index;
				}
			}
		}
	}
	return best;
}

int bots_nav_build(
	struct bots_nav_input const *input,
	struct bots_nav_parameters const *parameters,
	struct bots_nav *nav)
{
	struct bots_nav_builder builder;
	float maximum_x = -1.0e30f, maximum_y = -1.0e30f;
	long index;
	int built = 0;

	memset(nav, 0, sizeof(*nav));
	memset(&builder, 0, sizeof(builder));
	nav->main_node = BOTS_NAV_NONE;
	builder.parameters = *parameters;
	builder.input = input;
	builder.minimum_x = builder.minimum_y = 1.0e30f;
	for (index = 0; index < input->triangle_count * 3; index++)
	{
		float x = input->triangles[index * 3], y = input->triangles[index * 3 + 1];

		if (x < builder.minimum_x) builder.minimum_x = x;
		if (x > maximum_x) maximum_x = x;
		if (y < builder.minimum_y) builder.minimum_y = y;
		if (y > maximum_y) maximum_y = y;
	}
	/* (around the spawns only, when given: a level's far reaches, a ship's
	hull, are no place a bot goes) */
	if (input->seed_count > 0 && parameters->seed_margin > 0.0f)
	{
		float low_x = 1.0e30f, low_y = 1.0e30f, high_x = -1.0e30f, high_y = -1.0e30f;

		for (index = 0; index < input->seed_count; index++)
		{
			if (input->seeds[index].x < low_x) low_x = input->seeds[index].x;
			if (input->seeds[index].x > high_x) high_x = input->seeds[index].x;
			if (input->seeds[index].y < low_y) low_y = input->seeds[index].y;
			if (input->seeds[index].y > high_y) high_y = input->seeds[index].y;
		}
		if (low_x - parameters->seed_margin > builder.minimum_x) builder.minimum_x = low_x - parameters->seed_margin;
		if (high_x + parameters->seed_margin < maximum_x) maximum_x = high_x + parameters->seed_margin;
		if (low_y - parameters->seed_margin > builder.minimum_y) builder.minimum_y = low_y - parameters->seed_margin;
		if (high_y + parameters->seed_margin < maximum_y) maximum_y = high_y + parameters->seed_margin;
	}
	if (input->triangle_count <= 0 || maximum_x <= builder.minimum_x || maximum_y <= builder.minimum_y)
		return 0;
	builder.columns_x = (long)ceil((maximum_x - builder.minimum_x) / parameters->cell_size) + 1;
	builder.columns_y = (long)ceil((maximum_y - builder.minimum_y) / parameters->cell_size) + 1;
	builder.statistics.columns_x = builder.columns_x;
	builder.statistics.columns_y = builder.columns_y;
	builder.statistics.triangles = input->triangle_count;
	if (builder.columns_x > 65535 || builder.columns_y > 65535 ||
		builder.columns_x * builder.columns_y > BOTS_NAV_MAXIMUM_COLUMNS)
	{
		return 0;
	}

	nav->parameters = *parameters;
	nav->origin_x = builder.minimum_x;
	nav->origin_y = builder.minimum_y;
	if (bots_nav_rasterize(&builder) && bots_nav_erode(&builder) && bots_nav_make_nodes(&builder))
	{
		/* (the nodes, for bots_nav_locate as the edges are made) */
		nav->tiles_x = builder.tiles_x;
		nav->tiles_y = builder.tiles_y;
		nav->tile_first = builder.tile_first;
		nav->nodes = builder.nodes;
		nav->node_count = builder.node_count;
		if (bots_nav_make_edges(&builder, nav) && bots_nav_prune(&builder, nav))
		{
			built = 1;
		}
	}
	free(builder.cells.x);
	free(builder.cells.y);
	free(builder.cells.z);
	free(builder.cells.row_first);
	free(builder.distance);
	free(builder.node);
	free(builder.queue);
	free(builder.spans.bottom);
	free(builder.spans.top);
	free(builder.spans.next);
	free(builder.spans.walkable);
	free(builder.spans.head);
	if (!built)
	{
		free(builder.nodes);
		free(builder.edges);
		free(builder.tile_first);
		memset(nav, 0, sizeof(*nav));
		nav->main_node = BOTS_NAV_NONE;
		return 0;
	}
	nav->tile_first = builder.tile_first;
	nav->nodes = builder.nodes;
	nav->node_count = builder.node_count;
	nav->edges = builder.edges;
	nav->edge_count = builder.edge_count;
	nav->statistics = builder.statistics;
	return 1;
}

void bots_nav_free(
	struct bots_nav *nav)
{
	free(nav->tile_first);
	free(nav->nodes);
	free(nav->edges);
	free(nav->search);
	memset(nav, 0, sizeof(*nav));
	nav->main_node = BOTS_NAV_NONE;
}

/* ---------- A* */

struct bots_nav_search
{
	long node_count;
	unsigned long stamp;
	unsigned long *seen;
	unsigned long *closed;
	float *cost;
	/* the edge each node was reached by, and the node it leaves */
	long *via;
	long *parent;
	/* the open list: a binary heap of nodes by their estimate */
	long *heap;
	float *estimate;
	long heap_count;
};

static struct bots_nav_search *bots_nav_search_get(
	struct bots_nav *nav)
{
	struct bots_nav_search *search = (struct bots_nav_search *)nav->search;
	long count = nav->node_count ? nav->node_count : 1;
	unsigned char *memory;

	if (search && search->node_count == nav->node_count)
		return search;
	free(search);
	/* (one block: the header, then each array; the heap holds a node once
	for each time its cost falls, at most its edges' count, so as many as
	there are edges and nodes) */
	memory = (unsigned char *)calloc(1, sizeof(struct bots_nav_search) +
		(size_t)count * (2 * sizeof(unsigned long) + sizeof(float) + 2 * sizeof(long)) +
		(size_t)(count + nav->edge_count + 1) * (sizeof(long) + sizeof(float)));
	nav->search = memory;
	if (!memory)
		return NULL;
	search = (struct bots_nav_search *)memory;
	memory += sizeof(struct bots_nav_search);
	search->node_count = nav->node_count;
	search->seen = (unsigned long *)memory;
	memory += (size_t)count * sizeof(unsigned long);
	search->closed = (unsigned long *)memory;
	memory += (size_t)count * sizeof(unsigned long);
	search->cost = (float *)memory;
	memory += (size_t)count * sizeof(float);
	search->via = (long *)memory;
	memory += (size_t)count * sizeof(long);
	search->parent = (long *)memory;
	memory += (size_t)count * sizeof(long);
	search->heap = (long *)memory;
	memory += (size_t)(count + nav->edge_count + 1) * sizeof(long);
	search->estimate = (float *)memory;
	return search;
}

static void bots_nav_heap_push(
	struct bots_nav_search *search,
	long node,
	float estimate)
{
	long here = search->heap_count++;

	while (here > 0)
	{
		long parent = (here - 1) / 2;

		if (search->estimate[parent] <= estimate)
			break;
		search->heap[here] = search->heap[parent];
		search->estimate[here] = search->estimate[parent];
		here = parent;
	}
	search->heap[here] = node;
	search->estimate[here] = estimate;
}

static long bots_nav_heap_pop(
	struct bots_nav_search *search)
{
	long top = search->heap[0];
	long last = --search->heap_count;
	long node = search->heap[last];
	float estimate = search->estimate[last];
	long here = 0;

	for (;;)
	{
		long child = here * 2 + 1;

		if (child >= last)
			break;
		if (child + 1 < last && search->estimate[child + 1] < search->estimate[child])
			child++;
		if (search->estimate[child] >= estimate)
			break;
		search->heap[here] = search->heap[child];
		search->estimate[here] = search->estimate[child];
		here = child;
	}
	if (last > 0)
	{
		search->heap[here] = node;
		search->estimate[here] = estimate;
	}
	return top;
}

long bots_nav_find_path(
	struct bots_nav *nav,
	long from,
	long to,
	long *edges,
	long maximum)
{
	struct bots_nav_search *search;
	struct bots_nav_point const *goal;
	long count = 0;
	long node;

	if (from < 0 || to < 0 || from >= nav->node_count || to >= nav->node_count)
		return -1;
	if (from == to)
		return 0;
	if (!(search = bots_nav_search_get(nav)))
		return -1;
	goal = &nav->nodes[to].point;
	search->stamp++;
	search->heap_count = 0;
	search->seen[from] = search->stamp;
	search->cost[from] = 0.0f;
	search->via[from] = BOTS_NAV_NONE;
	bots_nav_heap_push(search, from, bots_nav_distance(&nav->nodes[from].point, goal));
	while (search->heap_count > 0)
	{
		long here = bots_nav_heap_pop(search);
		long index;

		if (search->closed[here] == search->stamp)
			continue;
		search->closed[here] = search->stamp;
		if (here == to)
			break;
		for (index = nav->nodes[here].first_edge; index < nav->nodes[here].first_edge + nav->nodes[here].edge_count; index++)
		{
			long there = nav->edges[index].target;
			float cost = search->cost[here] + nav->edges[index].cost;

			/* (the edges of the kinds the caller keeps off: none taken) */
			if (nav->avoided_types & (1u << nav->edges[index].type))
				continue;

			if (search->closed[there] == search->stamp)
				continue;
			if (search->seen[there] != search->stamp || cost < search->cost[there])
			{
				search->seen[there] = search->stamp;
				search->cost[there] = cost;
				search->via[there] = index;
				search->parent[there] = here;
				bots_nav_heap_push(search, there, cost + bots_nav_distance(&nav->nodes[there].point, goal));
			}
		}
	}
	if (search->closed[to] != search->stamp)
		return -1;
	/* (the edges back from the goal) */
	for (node = to; node != from; node = search->parent[node])
	{
		if (count >= maximum)
			return -1;
		edges[count++] = search->via[node];
	}
	return count;
}

/* ---------- the cache's bytes */

#define BOTS_NAV_MAGIC 0x56414E42UL /* "BNAV" */

struct bots_nav_header
{
	unsigned long magic;
	unsigned long version;
	unsigned long key;
	struct bots_nav_parameters parameters;
	float origin_x, origin_y;
	long tiles_x, tiles_y;
	long node_count, edge_count;
	long main_node;
	struct bots_nav_statistics statistics;
};

unsigned char *bots_nav_save(
	struct bots_nav const *nav,
	unsigned long key,
	long *size)
{
	struct bots_nav_header header;
	long tiles = nav->tiles_x * nav->tiles_y + 1;
	unsigned char *bytes;
	unsigned char *at;

	*size = (long)sizeof(header) + tiles * (long)sizeof(long) + nav->node_count * (long)sizeof(struct bots_nav_node) +
		nav->edge_count * (long)sizeof(struct bots_nav_edge);
	if (!(bytes = (unsigned char *)malloc((size_t)*size)))
		return NULL;
	memset(&header, 0, sizeof(header));
	header.magic = BOTS_NAV_MAGIC;
	header.version = BOTS_NAV_GENERATOR_VERSION;
	header.key = key;
	header.parameters = nav->parameters;
	header.origin_x = nav->origin_x;
	header.origin_y = nav->origin_y;
	header.tiles_x = nav->tiles_x;
	header.tiles_y = nav->tiles_y;
	header.node_count = nav->node_count;
	header.edge_count = nav->edge_count;
	header.main_node = nav->main_node;
	header.statistics = nav->statistics;
	at = bytes;
	memcpy(at, &header, sizeof(header));
	at += sizeof(header);
	memcpy(at, nav->tile_first, (size_t)tiles * sizeof(long));
	at += (size_t)tiles * sizeof(long);
	memcpy(at, nav->nodes, (size_t)nav->node_count * sizeof(struct bots_nav_node));
	at += (size_t)nav->node_count * sizeof(struct bots_nav_node);
	memcpy(at, nav->edges, (size_t)nav->edge_count * sizeof(struct bots_nav_edge));
	return bytes;
}

void bots_nav_free_bytes(
	unsigned char *bytes)
{
	free(bytes);
}

int bots_nav_load(
	struct bots_nav *nav,
	unsigned char const *bytes,
	long size,
	unsigned long key,
	struct bots_nav_parameters const *parameters)
{
	struct bots_nav_header header;
	long tiles;
	long index;

	memset(nav, 0, sizeof(*nav));
	nav->main_node = BOTS_NAV_NONE;
	if (size < (long)sizeof(header))
		return 0;
	memcpy(&header, bytes, sizeof(header));
	if (header.magic != BOTS_NAV_MAGIC || header.version != BOTS_NAV_GENERATOR_VERSION || header.key != key ||
		memcmp(&header.parameters, parameters, sizeof(*parameters)) || header.tiles_x <= 0 || header.tiles_y <= 0 ||
		header.tiles_x > 65535 || header.tiles_y > 65535 || header.node_count < 0 || header.edge_count < 0)
	{
		return 0;
	}
	tiles = header.tiles_x * header.tiles_y + 1;
	if (size != (long)sizeof(header) + tiles * (long)sizeof(long) +
		header.node_count * (long)sizeof(struct bots_nav_node) + header.edge_count * (long)sizeof(struct bots_nav_edge))
	{
		return 0;
	}
	nav->tile_first = (long *)malloc((size_t)tiles * sizeof(long));
	nav->nodes = (struct bots_nav_node *)malloc((size_t)(header.node_count ? header.node_count : 1) *
		sizeof(struct bots_nav_node));
	nav->edges = (struct bots_nav_edge *)malloc((size_t)(header.edge_count ? header.edge_count : 1) *
		sizeof(struct bots_nav_edge));
	if (!nav->tile_first || !nav->nodes || !nav->edges)
	{
		bots_nav_free(nav);
		return 0;
	}
	bytes += sizeof(header);
	memcpy(nav->tile_first, bytes, (size_t)tiles * sizeof(long));
	bytes += (size_t)tiles * sizeof(long);
	memcpy(nav->nodes, bytes, (size_t)header.node_count * sizeof(struct bots_nav_node));
	bytes += (size_t)header.node_count * sizeof(struct bots_nav_node);
	memcpy(nav->edges, bytes, (size_t)header.edge_count * sizeof(struct bots_nav_edge));
	/* (a file someone changed: every index checked, so that nothing reads
	past an array) */
	for (index = 0; index < tiles; index++)
	{
		if (nav->tile_first[index] < 0 || nav->tile_first[index] > header.node_count ||
			(index > 0 && nav->tile_first[index] < nav->tile_first[index - 1]))
		{
			bots_nav_free(nav);
			return 0;
		}
	}
	for (index = 0; index < header.node_count; index++)
	{
		struct bots_nav_node const *node = &nav->nodes[index];

		if (node->first_edge < 0 || node->edge_count < 0 || node->first_edge + node->edge_count > header.edge_count)
		{
			bots_nav_free(nav);
			return 0;
		}
	}
	for (index = 0; index < header.edge_count; index++)
	{
		if (nav->edges[index].target < 0 || nav->edges[index].target >= header.node_count ||
			nav->edges[index].type >= NUMBER_OF_BOTS_NAV_EDGE_TYPES)
		{
			bots_nav_free(nav);
			return 0;
		}
	}
	if (header.main_node < -1 || header.main_node >= header.node_count)
	{
		bots_nav_free(nav);
		return 0;
	}
	nav->parameters = header.parameters;
	nav->origin_x = header.origin_x;
	nav->origin_y = header.origin_y;
	nav->tiles_x = header.tiles_x;
	nav->tiles_y = header.tiles_y;
	nav->node_count = header.node_count;
	nav->edge_count = header.edge_count;
	nav->main_node = header.main_node;
	nav->statistics = header.statistics;
	return 1;
}
