/* bots_cover.c: a bot that falls back to cover (bots_internal.h)

A bot whose shields are nearly gone in a fight (and, unless its target is in
view, whose body is hurt too) looks for cover: a node of the walking graph a
few world units off that its target cannot see (the game's line of sight
test from the target's eyes to a standing bot's eyes there), not toward the
target, the nearest such. It goes there, still facing and shooting at a
target it sees, waits there while its shields come back, and fights again:
when they are back, when the target comes within a few world units, or after
a while. Easy bots stand and fight; peaceful ones never fight. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "units/units.h"

#include "bots_internal.h"

#include <math.h>

/* the shields (a fraction) under which it falls back, and the body it
keeps; the shields it waits for */
#define BOT_COVER_SHIELDS 0.25f
#define BOT_COVER_BODY 0.75f
#define BOT_COVER_RECOVERED 0.95f
/* how near and far cover may be (world units), how many candidates are
looked at with the line of sight test, and the eyes' height above a node */
#define BOT_COVER_NEAREST 2.5f
#define BOT_COVER_FURTHEST 12.0f
#define BOT_COVER_CANDIDATES 16
#define BOT_COVER_EYES 0.6f
/* the longest it stays fallen back (ticks), how often it looks for cover,
how near its target may come before it fights again, and how near the
cover point counts as there */
#define BOT_COVER_TICKS (8 * TICKS_PER_SECOND)
#define BOT_COVER_LOOK_TICKS (2 * TICKS_PER_SECOND)
#define BOT_COVER_TOO_CLOSE 3.5f
#define BOT_COVER_THERE 0.8f

static boolean bot_cover_hidden(
	real_point3d const *eye,
	struct bots_nav_point const *point)
{
	struct collision_result collision;
	real_point3d to;
	real_vector3d vector;

	to.x = point->x;
	to.y = point->y;
	to.z = point->z + BOT_COVER_EYES;
	vector.i = to.x - eye->x;
	vector.j = to.y - eye->y;
	vector.k = to.z - eye->z;
	return collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_ignore_two_sided_surfaces_bit) | FLAG(_collision_test_structure_bit) |
		FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) |
		FLAG(_collision_test_objects_machines_bit), eye, &vector, NONE, &collision);
}

/* the nearest node hidden from the eyes, not toward them (FALSE: none) */
static boolean bot_cover_find(
	struct bots_nav const *nav,
	real_point3d const *position,
	real_point3d const *eye,
	struct bots_nav_point *cover)
{
	long candidates[BOT_COVER_CANDIDATES];
	real scores[BOT_COVER_CANDIDATES];
	long count = 0;
	real size = nav->parameters.tile_size;
	long reach = (long)ceil(BOT_COVER_FURTHEST / size);
	long tile_x = (long)floor((position->x - nav->origin_x) / size);
	long tile_y = (long)floor((position->y - nav->origin_y) / size);
	real enemy_now = (real)sqrt((eye->x - position->x) * (eye->x - position->x) + (eye->y - position->y) * (eye->y - position->y));
	long tx, ty, index;

	for (ty = tile_y - reach; ty <= tile_y + reach; ty++)
	{
		for (tx = tile_x - reach; tx <= tile_x + reach; tx++)
		{
			long node;

			if (tx < 0 || ty < 0 || tx >= nav->tiles_x || ty >= nav->tiles_y)
				continue;
			for (node = nav->tile_first[ty * nav->tiles_x + tx]; node < nav->tile_first[ty * nav->tiles_x + tx + 1]; node++)
			{
				struct bots_nav_node const *candidate = &nav->nodes[node];
				real dx = candidate->point.x - position->x, dy = candidate->point.y - position->y;
				real away = (real)sqrt(dx * dx + dy * dy);
				real enemy = (real)sqrt((candidate->point.x - eye->x) * (candidate->point.x - eye->x) +
					(candidate->point.y - eye->y) * (candidate->point.y - eye->y));
				real score;
				long slot;

				if (candidate->reach != (_bots_nav_reached_bit | _bots_nav_reaches_bit) || away < BOT_COVER_NEAREST ||
					away > BOT_COVER_FURTHEST || fabs(candidate->point.z - position->z) > 2.0f || enemy < enemy_now - 1.0f)
				{
					continue;
				}
				/* (near it, and away from the target) */
				score = away - 0.5f * (enemy - enemy_now);
				if (count < BOT_COVER_CANDIDATES)
				{
					slot = count++;
				}
				else
				{
					/* (in place of the worst kept, if better) */
					slot = 0;
					for (index = 1; index < count; index++)
					{
						if (scores[index] > scores[slot])
							slot = index;
					}
					if (score >= scores[slot])
						continue;
				}
				candidates[slot] = node;
				scores[slot] = score;
			}
		}
	}
	/* (the best first, until one is hidden) */
	while (count > 0)
	{
		long best = 0;

		for (index = 1; index < count; index++)
		{
			if (scores[index] < scores[best])
				best = index;
		}
		if (bot_cover_hidden(eye, &nav->nodes[candidates[best]].point))
		{
			*cover = nav->nodes[candidates[best]].point;
			return TRUE;
		}
		candidates[best] = candidates[count - 1];
		scores[best] = scores[count - 1];
		count--;
	}
	return FALSE;
}

void bot_cover_update(
	struct bot *bot,
	long unit_index)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_combat *combat = &brain->combat;
	struct unit_datum *unit = unit_get(unit_index);
	struct bots_nav *nav = bots_nav_current();
	struct player_datum *target_player = player_try_and_get(combat->target_player);
	long now = game_time_get();
	real_point3d eye;
	real distance;

	if (!nav || bot->difficulty <= _bot_difficulty_easy || !target_player || target_player->unit_index == NONE)
	{
		combat->cover_until_tick = 0;
		combat->in_cover = FALSE;
		return;
	}
	if (combat->target_visible)
		unit_get_camera_position(target_player->unit_index, &eye);
	else
	{
		eye = combat->last_seen_position;
		eye.z += BOT_COVER_EYES;
	}
	distance = (real)sqrt((eye.x - unit->object.position.x) * (eye.x - unit->object.position.x) +
		(eye.y - unit->object.position.y) * (eye.y - unit->object.position.y));
	if (now < combat->cover_until_tick)
	{
		/* (shields back, the target upon it, or long enough: it fights) */
		if ((combat->in_cover && unit->object.shield_vitality >= BOT_COVER_RECOVERED) ||
			(combat->target_visible && distance < BOT_COVER_TOO_CLOSE))
		{
			combat->cover_until_tick = 0;
			combat->in_cover = FALSE;
		}
		return;
	}
	combat->in_cover = FALSE;
	if (now < combat->cover_look_tick || unit->object.shield_vitality >= BOT_COVER_SHIELDS ||
		(!combat->target_visible && unit->object.body_vitality >= BOT_COVER_BODY) ||
		(combat->target_visible && distance < BOT_COVER_TOO_CLOSE))
	{
		return;
	}
	combat->cover_look_tick = now + BOT_COVER_LOOK_TICKS;
	if (!bot_cover_find(nav, &unit->object.position, &eye, &combat->cover_point))
		return;
	combat->cover_until_tick = now + BOT_COVER_TICKS;
	brain->statistics.retreats++;
}

boolean bot_cover_goal(
	struct bot *bot,
	real_point3d const *position,
	struct bots_nav_point *goal)
{
	struct bot_combat *combat = &bot->brain.combat;
	real dx = combat->cover_point.x - position->x;
	real dy = combat->cover_point.y - position->y;

	if (game_time_get() >= combat->cover_until_tick)
		return FALSE;
	if (!combat->in_cover && dx * dx + dy * dy < BOT_COVER_THERE * BOT_COVER_THERE)
	{
		combat->in_cover = TRUE;
		bot->brain.statistics.covers_reached++;
	}
	*goal = combat->cover_point;
	return TRUE;
}
