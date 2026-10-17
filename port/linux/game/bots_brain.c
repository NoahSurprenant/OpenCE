/* bots_brain.c: what a bot does each tick (bots_internal.h)

A bot roams the map: it picks a node of the walking graph (bots_nav.c) that
the spawns' part reaches and that reaches it back, plans the way there (A*),
and follows it: walks from node to node (cutting across where the nodes ahead
lie along a line), walks off a drop's edge, jumps up at a jump's take-off,
steps onto a teleporter's entrance. It notices when it gets nowhere: it
jumps, then plans again, then keeps off the edge it was on and picks
another goal. Without a graph it wanders, as before: walks ahead, turns now
and then, turns away and jumps when stuck.

Its input is a player's (players.h's player_action): where it faces, how it
moves, its buttons, always within what player_control.c lets a person give
(the aim's pitch within 85 degrees, the yaw within a turn), since every
machine takes it as it takes a person's. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/players.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "bots_internal.h"

#include <math.h>
#include <stdio.h>

/* the platform layer's (port_config.c, the port's log) */
int config_boolean(const char *name);
const char *config_string(const char *name);
void platform_log(const char *format, ...);
/* (debug.bot_trace: each bot's way each second, in the log) */
static boolean bot_trace_checked, bot_trace;

/* the ticks between a wanderer's turns, and between looks at how far a bot
has come; how far it must have come; how fast it turns (radians a tick) */
#define BOT_WANDER_TURN_TICKS_MINIMUM 45
#define BOT_WANDER_TURN_TICKS_RANGE 75
#define BOT_CHECK_TICKS 45
#define BOT_PROGRESS 0.3f
#define BOT_TURN_RATE (_pi / 15.0f)
/* how near a bot comes to a node, or to a move's start or end, before it
goes on; how far off its way it may be before it plans again */
#define BOT_NODE_REACHED 0.45f
#define BOT_MOVE_BEGUN 0.35f
#define BOT_MOVE_DONE 0.5f
#define BOT_LOST_DISTANCE 6.0f
/* the nodes ahead a bot cuts across to, and how near its line each between
must be */
#define BOT_LOOK_AHEAD 4
#define BOT_CORRIDOR 0.45f
/* how long a bot may come no nearer a point it heads for before as near as
it gets is near enough (ticks), and how much nearer counts */
#define BOT_APPROACH_STALL_TICKS 12
#define BOT_APPROACH_GAIN 0.05f
/* the ticks after a bot took a weapon before it swaps another for one */
#define BOT_PICKUP_SWAP_TICKS (30 * TICKS_PER_SECOND)
/* the ticks a bot stands at an item trying to take it before it gives up */
#define BOT_PICKUP_AT_TICKS (2 * TICKS_PER_SECOND)
/* a jump longer than this (world units) is taken with a run-up this long */
#define BOT_RUN_UP_SPAN 1.3f
#define BOT_RUN_UP 1.4f
/* the ticks a bot pushes on along a walk hardly moving before it hops */
#define BOT_STALL_TICKS 8
/* a step aside when getting nowhere (ticks) */
#define BOT_SIDESTEP_TICKS 12
/* the ticks an edge a bot got stuck on is kept off */
#define BOT_AVOID_TICKS (10 * TICKS_PER_SECOND)
/* how far a weapon on the ground may be for a bot to go and take it */
#define BOT_PICKUP_RANGE 20.0f

/* ---------- random numbers, the bot's own (debug.bot_seed: the same each run) */

real bot_random_real(
	struct bot_brain *brain)
{
	brain->random = brain->random * 1664525UL + 1013904223UL;
	return (real)((brain->random >> 8) & 0xFFFFFF) / (real)0x1000000;
}

static long bot_random_range(
	struct bot_brain *brain,
	long minimum,
	long range)
{
	return minimum + (long)(bot_random_real(brain) * (real)range);
}

/* ---------- angles */

/* an angle in [0, 2 pi), as the game takes a yaw */
static real bot_yaw_normalize(
	real yaw)
{
	while (yaw >= 2.0f * _pi)
		yaw -= 2.0f * _pi;
	while (yaw < 0.0f)
		yaw += 2.0f * _pi;
	return yaw >= 2.0f * _pi ? 0.0f : yaw;
}

/* the angle from one yaw to another, in (-pi, pi] */
static real bot_yaw_difference(
	real from,
	real to)
{
	real difference = bot_yaw_normalize(to - from);

	return difference > _pi ? difference - 2.0f * _pi : difference;
}

static real bot_pitch_clamp(
	real pitch)
{
	return PIN(pitch, -DEGREES_TO_RADIANS(BOT_MAXIMUM_PITCH_DEGREES), DEGREES_TO_RADIANS(BOT_MAXIMUM_PITCH_DEGREES));
}

/* ---------- the action */

void bot_action_idle(
	struct player_action *action,
	real yaw,
	real pitch)
{
	csmemset(action, 0, sizeof(*action));
	action->desired_facing.yaw = bot_yaw_normalize(yaw);
	action->desired_facing.pitch = bot_pitch_clamp(pitch);
	action->desired_weapon_index = NONE;
	action->desired_grenade_index = NONE;
	action->desired_zoom_level = NONE;
}

/* turns the bot toward a yaw (at most its turn rate a tick) */
static void bot_turn_to(
	struct bot_brain *brain,
	real yaw)
{
	real turn = bot_yaw_difference(brain->yaw, yaw);

	brain->yaw = bot_yaw_normalize(brain->yaw + PIN(turn, -BOT_TURN_RATE, BOT_TURN_RATE));
}

/* the throttle that moves the bot toward a point whatever way it faces
(throttle.i ahead, throttle.j to its left) */
static void bot_move_toward(
	struct player_action *action,
	struct bot_brain const *brain,
	real_point3d const *position,
	struct bots_nav_point const *target)
{
	real dx = target->x - position->x, dy = target->y - position->y;
	real relative;

	if (dx * dx + dy * dy < 0.0001f)
		return;
	relative = (real)atan2(dy, dx) - brain->yaw;
	action->throttle.i = (real)cos(relative);
	action->throttle.j = (real)sin(relative);
}

void bot_brain_reset(
	struct bot_brain *brain,
	unsigned long seed)
{
	csmemset(brain, 0, sizeof(*brain));
	brain->random = seed ? seed : 1;
	brain->unit_index = NONE;
	brain->path_next = NONE;
	brain->goal_node = NONE;
	brain->avoided_edge = NONE;
	brain->pickup_object = NONE;
	brain->pickup_given_up = NONE;
	brain->combat_goal_node = NONE;
	bot_combat_reset(&brain->combat);
}

/* ---------- wandering, without a graph */

static void bot_wander(
	struct bot_brain *brain,
	real_point3d const *position,
	struct player_action *action)
{
	if (--brain->turn_ticks <= 0)
	{
		brain->target_yaw = bot_yaw_normalize(brain->yaw + (bot_random_real(brain) - 0.5f) * 2.0f * _pi / 3.0f);
		brain->turn_ticks = bot_random_range(brain, BOT_WANDER_TURN_TICKS_MINIMUM, BOT_WANDER_TURN_TICKS_RANGE);
	}
	if (brain->stuck_count > 0)
	{
		brain->target_yaw = bot_yaw_normalize(brain->yaw + _pi * (0.5f + bot_random_real(brain)));
		brain->stuck_count = 0;
	}
	bot_turn_to(brain, brain->target_yaw);
	bot_action_idle(action, brain->yaw, brain->pitch);
	action->throttle.i = 1.0f;
	(void)position;
}

/* ---------- roaming the graph */

/* an edge a bot could not go along (a railing the graph did not see, a jump
too high in fact, a climbable surface no biped climbs): dearer for every bot
from now on, for this map; failed again, as good as gone */
static void bot_edge_failed(
	struct bots_nav *nav,
	long edge)
{
	if (edge >= 0 && edge < nav->edge_count && nav->edges[edge].cost < 500.0f)
		nav->edges[edge].cost += nav->edges[edge].failed ? 1000.0f : 40.0f;
	if (edge >= 0 && edge < nav->edge_count)
		nav->edges[edge].failed = 1;
}

static real bot_distance2d(
	real_point3d const *position,
	struct bots_nav_point const *point)
{
	real dx = point->x - position->x, dy = point->y - position->y;

	return (real)sqrt(dx * dx + dy * dy);
}

/* whether the bot is near enough the point it heads for at this step of its
way: within strict, or within loose and coming no nearer for a while (a
wall, a railing or a player between: as near as it gets) */
static boolean bot_near_enough(
	struct bot_brain *brain,
	real distance,
	real strict,
	real loose)
{
	long step = brain->path_next * 2 + (brain->edge_begun ? 0 : 1);

	if (distance < strict)
		return TRUE;
	if (step != brain->approach_step)
	{
		brain->approach_step = step;
		brain->approach_best = distance;
		brain->approach_ticks = 0;
		return FALSE;
	}
	if (distance < brain->approach_best - BOT_APPROACH_GAIN)
	{
		brain->approach_best = distance;
		brain->approach_ticks = 0;
		return FALSE;
	}
	return ++brain->approach_ticks > BOT_APPROACH_STALL_TICKS && distance < loose;
}

/* whether a biped walks straight from the bot's place to the point:
nothing of the level or its scenery in the way at its knees, either side
of its middle */
static boolean bot_line_walkable(
	real_point3d const *position,
	struct bots_nav_point const *point)
{
	real dx = point->x - position->x, dy = point->y - position->y;
	real length = (real)sqrt(dx * dx + dy * dy);
	short side;

	if (length < 0.01f)
		return TRUE;
	for (side = -1; side <= 1; side += 2)
	{
		struct collision_result collision;
		real_point3d from;
		real_vector3d vector;

		/* (a biped's radius to either side) */
		from.x = position->x - dy / length * 0.18f * side;
		from.y = position->y + dx / length * 0.18f * side;
		from.z = position->z + 0.35f;
		vector.i = dx;
		vector.j = dy;
		vector.k = point->z - position->z;
		if (collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit) |
			FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) |
			FLAG(_collision_test_objects_machines_bit), &from, &vector, NONE, &collision))
		{
			return FALSE;
		}
	}
	return TRUE;
}

/* a node the spawns' part reaches both ways, at random (NONE if none found) */
static long bot_random_goal(
	struct bot_brain *brain,
	struct bots_nav const *nav)
{
	short attempt;

	for (attempt = 0; attempt < 16; attempt++)
	{
		long node = (long)(bot_random_real(brain) * (real)nav->node_count);

		if (node >= 0 && node < nav->node_count &&
			nav->nodes[node].reach == (_bots_nav_reached_bit | _bots_nav_reaches_bit))
		{
			return node;
		}
	}
	return NONE;
}

/* the way from where the bot is to its goal (a new goal if it has none);
FALSE if there is none now */
static boolean bot_plan(
	struct bot_brain *brain,
	struct bots_nav *nav,
	real_point3d const *position,
	boolean new_goal)
{
	long here = bots_nav_locate(nav, position->x, position->y, position->z);
	long count;

	brain->path_next = NONE;
	brain->edge_begun = FALSE;
	if (here == NONE)
		return FALSE;
	/* (a way given up on at once, again and again: a second wandering
	instead, so that a bot never plans every tick) */
	if (game_time_get() / TICKS_PER_SECOND != brain->plan_second)
	{
		brain->plan_second = game_time_get() / TICKS_PER_SECOND;
		brain->plans_this_second = 0;
	}
	if (++brain->plans_this_second > 4)
		return FALSE;
	if (new_goal || brain->goal_node == NONE || brain->goal_node == here)
	{
		/* (debug.bot_goal: there, every other goal) */
		char const *debug_goal = config_string("debug.bot_goal");
		float gx, gy, gz;

		brain->goal_node = bot_random_goal(brain, nav);
		if (debug_goal && debug_goal[0] && sscanf(debug_goal, "%f %f %f", &gx, &gy, &gz) == 3 &&
			(brain->statistics.paths_planned % 2) == 0)
		{
			long node = bots_nav_locate(nav, gx, gy, gz);

			if (node != NONE)
				brain->goal_node = node;
			if (node == here)
				platform_log("bots: %p reached the debug goal (%.2f %.2f %.2f)", (void *)brain, gx, gy, gz);
		}
	}
	if (brain->goal_node == NONE)
		return FALSE;
	count = bots_nav_find_path(nav, here, brain->goal_node, brain->path, BOT_PATH_MAXIMUM);
	if (bot_trace && game_time_get() % TICKS_PER_SECOND == 0)
		platform_log("bots: trace %p tick %ld plan from node %ld (%.2f %.2f %.2f) at (%.2f %.2f %.2f) to %ld: %ld edges", (void *)brain,
			game_time_get(), here, nav->nodes[here].point.x, nav->nodes[here].point.y, nav->nodes[here].point.z, position->x,
			position->y, position->z, brain->goal_node, count);
	brain->statistics.paths_planned++;
	/* (an edge it got stuck on: another goal, whose way does not take it,
	if one is found soon) */
	if (count > 0 && brain->avoided_ticks > 0)
	{
		long index;

		for (index = 0; index < count; index++)
		{
			if (brain->path[index] == brain->avoided_edge)
			{
				count = -1;
				break;
			}
		}
	}
	if (count <= 0)
	{
		brain->statistics.paths_failed += count < 0;
		brain->goal_node = NONE;
		/* (a weapon it cannot get to: given up) */
		if (brain->pickup_object != NONE)
		{
			brain->pickup_given_up = brain->pickup_object;
			brain->pickup_object = NONE;
		}
		return FALSE;
	}
	brain->path_next = count - 1;
	return TRUE;
}

/* the point a bot heads for along a walk: the furthest of the next few
nodes whose way is all walks and passes near each node between */
static struct bots_nav_point const *bot_walk_target(
	struct bot_brain const *brain,
	struct bots_nav const *nav,
	real_point3d const *position)
{
	struct bots_nav_point const *target = &nav->nodes[nav->edges[brain->path[brain->path_next]].target].point;
	long ahead;

	for (ahead = 1; ahead < BOT_LOOK_AHEAD && brain->path_next - ahead >= 0; ahead++)
	{
		struct bots_nav_edge const *edge = &nav->edges[brain->path[brain->path_next - ahead]];
		struct bots_nav_point const *candidate = &nav->nodes[edge->target].point;
		real sx = candidate->x - position->x, sy = candidate->y - position->y;
		real length_squared = sx * sx + sy * sy;
		long between;
		boolean along = edge->type == _bots_nav_walk && length_squared > 0.0001f;

		for (between = 0; along && between < ahead; between++)
		{
			struct bots_nav_point const *node = &nav->nodes[nav->edges[brain->path[brain->path_next - between]].target].point;
			real t = ((node->x - position->x) * sx + (node->y - position->y) * sy) / length_squared;
			real px = position->x + sx * PIN(t, 0.0f, 1.0f) - node->x;
			real py = position->y + sy * PIN(t, 0.0f, 1.0f) - node->y;

			if (px * px + py * py > BOT_CORRIDOR * BOT_CORRIDOR || fabs(node->z - candidate->z) > 0.4f)
				along = FALSE;
		}
		/* (and nothing in the way: no corner cut into a wall) */
		if (!along || !bot_line_walkable(position, candidate))
			break;
		target = candidate;
	}
	return target;
}

static void bot_roam(
	struct bot_brain *brain,
	struct bots_nav *nav,
	real_point3d const *position,
	struct player_action *action)
{
	struct bots_nav_edge const *edge;
	struct bots_nav_point const *target;
	boolean jump = FALSE;

	if (brain->path_next == NONE && !bot_plan(brain, nav, position, FALSE))
	{
		/* (nowhere to go from here: a step aside, as a wanderer) */
		bot_wander(brain, position, action);
		return;
	}
	edge = &nav->edges[brain->path[brain->path_next]];
	if (edge->type == _bots_nav_walk)
	{
		target = &nav->nodes[edge->target].point;
		if (fabs(position->z - target->z) < 1.0f && bot_near_enough(brain, bot_distance2d(position, target), BOT_NODE_REACHED, 0.9f))
		{
			brain->path_next--;
			if (brain->path_next < 0)
			{
				brain->statistics.goals_reached++;
				brain->path_next = NONE;
				brain->goal_node = NONE;
				bot_action_idle(action, brain->yaw, brain->pitch);
				return;
			}
			edge = &nav->edges[brain->path[brain->path_next]];
		}
	}
	if (edge->type == _bots_nav_walk)
	{
		target = bot_walk_target(brain, nav, position);
		/* (a step up within reach ahead: hopped, as people do) */
		if (nav->nodes[edge->target].point.z - position->z > 0.12f &&
			bot_distance2d(position, &nav->nodes[edge->target].point) < 0.9f && brain->crouch_ticks == 0)
		{
			jump = TRUE;
		}
	}
	else if (edge->type == _bots_nav_descend)
	{
		/* down beside a ladder: its back to it, over the top of its face to
		its foot */
		real away = (real)atan2(edge->to.y - edge->from.y, edge->to.x - edge->from.x);

		if (!brain->edge_begun && bot_distance2d(position, &edge->from) < 0.8f)
			brain->edge_begun = TRUE;
		if (brain->edge_begun)
		{
			if (bot_distance2d(position, &edge->to) < BOT_MOVE_DONE && fabs(position->z - edge->to.z) < 0.5f)
			{
				brain->edge_begun = FALSE;
				brain->statistics.moves[edge->type]++;
				brain->path_next--;
				if (brain->path_next < 0)
				{
					brain->statistics.goals_reached++;
					brain->path_next = NONE;
					brain->goal_node = NONE;
				}
				bot_action_idle(action, brain->yaw, brain->pitch);
				return;
			}
			bot_turn_to(brain, away);
			bot_action_idle(action, brain->yaw, brain->pitch);
			bot_move_toward(action, brain, position, &edge->to);
			return;
		}
		target = &edge->from;
	}
	else if (edge->type == _bots_nav_climb)
	{
		/* a ladder: into its face until high enough, then off to the top */
		if (!brain->edge_begun && bot_near_enough(brain, bot_distance2d(position, &edge->from), 0.6f, 0.95f))
		{
			brain->edge_begun = TRUE;
			brain->climb_yaw = (real)atan2(edge->from.y - position->y, edge->from.x - position->x);
		}
		/* (near its top: off it, onto the top, with a hop over the lip) */
		target = brain->edge_begun && position->z > edge->to.z - 0.4f ? &edge->to : &edge->from;
		if (brain->edge_begun && target == &edge->to && position->z < edge->to.z - 0.1f && brain->crouch_ticks == 0)
			jump = TRUE;
		/* (on it: into its face, whatever the face's middle is from here) */
		if (brain->edge_begun && target == &edge->from)
		{
			bot_turn_to(brain, brain->climb_yaw);
			bot_action_idle(action, brain->yaw, DEGREES_TO_RADIANS(30.0f));
			action->throttle.i = 1.0f;
			return;
		}
		if (brain->edge_begun && bot_distance2d(position, &edge->to) < BOT_MOVE_DONE && fabs(position->z - edge->to.z) < 0.5f)
		{
			brain->edge_begun = FALSE;
			brain->statistics.moves[edge->type]++;
			brain->path_next--;
			if (brain->path_next < 0)
			{
				brain->statistics.goals_reached++;
				brain->path_next = NONE;
				brain->goal_node = NONE;
			}
			bot_action_idle(action, brain->yaw, brain->pitch);
			return;
		}
	}
	else
	{
		/* a drop, a jump or a teleporter: to its start, then across; a long
		jump with a run-up first, back along its line, so that the bot takes
		off running */
		static struct bots_nav_point run_up_point;
		boolean running_up = FALSE;

		if (edge->type == _bots_nav_jump && !brain->edge_begun)
		{
			real span_x = edge->to.x - edge->from.x, span_y = edge->to.y - edge->from.y;
			real span = (real)sqrt(span_x * span_x + span_y * span_y);

			if (brain->run_up_step != brain->path_next)
			{
				brain->run_up_step = brain->path_next;
				brain->run_up_done = span < BOT_RUN_UP_SPAN;
			}
			if (!brain->run_up_done)
			{
				run_up_point.x = edge->from.x - span_x / span * BOT_RUN_UP;
				run_up_point.y = edge->from.y - span_y / span * BOT_RUN_UP;
				run_up_point.z = edge->from.z;
				/* (no floor for it, or there: on to the take-off) */
				if (bots_nav_locate(nav, run_up_point.x, run_up_point.y, run_up_point.z) == NONE ||
					bot_near_enough(brain, bot_distance2d(position, &run_up_point), 0.4f, 0.7f))
				{
					brain->run_up_done = TRUE;
				}
				else
				{
					running_up = TRUE;
				}
			}
		}
		if (!running_up && !brain->edge_begun && bot_near_enough(brain, bot_distance2d(position, &edge->from), BOT_MOVE_BEGUN,
			edge->type == _bots_nav_teleport ? BOT_MOVE_BEGUN : edge->type == _bots_nav_jump ? 0.6f : 0.7f))
		{
			brain->edge_begun = TRUE;
			brain->jump_ticks = 0;
			jump = edge->type == _bots_nav_jump;
			if (jump && bot_trace)
				platform_log("bots: trace %p jump from (%.2f %.2f %.2f) up %.2f to (%.2f %.2f %.2f)", (void *)brain, position->x,
					position->y, position->z, edge->to.z - edge->from.z, edge->to.x, edge->to.y, edge->to.z);
			if (jump && edge->to.z - edge->from.z > 0.5f)
				brain->crouch_ticks = 12;
		}
		target = brain->edge_begun ? &edge->to : running_up ? &run_up_point : &edge->from;
		if (edge->type == _bots_nav_teleport)
		{
			static struct bots_nav_point step_off_point;

			if (brain->step_off)
			{
				real dx = position->x - edge->from.x, dy = position->y - edge->from.y;
				real length = (real)sqrt(dx * dx + dy * dy);

				if (length > 1.4f)
				{
					brain->step_off = FALSE;
				}
				else
				{
					step_off_point = edge->from;
					step_off_point.x += length > 0.01f ? dx / length * 1.8f : 1.8f;
					step_off_point.y += length > 0.01f ? dy / length * 1.8f : 0.0f;
					step_off_point.z = position->z;
					brain->edge_begun = FALSE;
					target = &step_off_point;
				}
			}
			/* (on the pad, not taken: off it, and on again) */
			else if (brain->edge_begun && bot_distance2d(position, &edge->from) < 1.0f &&
				++brain->teleport_wait > TICKS_PER_SECOND / 2)
			{
				brain->teleport_wait = 0;
				brain->step_off = TRUE;
			}
			/* (while on the way to its pad, or on it, its exit far off is no loss of the way) */
			if (!brain->step_off && bot_distance2d(position, &edge->from) < 2.0f)
				target = &edge->from;
		}
		/* (a jump, a drop or a teleporter that did not work out in time - a
		railing in the way, a ledge the graph took for a drop: kept off, and
		another way) */
		if (brain->edge_begun && ++brain->jump_ticks >
			(edge->type == _bots_nav_jump ? 45 : edge->type == _bots_nav_drop ? 60 : 150))
		{
			bot_edge_failed(nav, brain->path[brain->path_next]);
			brain->avoided_edge = brain->path[brain->path_next];
			brain->avoided_ticks = BOT_AVOID_TICKS;
			brain->edge_begun = FALSE;
			brain->path_next = NONE;
			bot_action_idle(action, brain->yaw, brain->pitch);
			return;
		}
		if (brain->edge_begun &&
			((bot_distance2d(position, &edge->to) < BOT_MOVE_DONE &&
				fabs(position->z - edge->to.z) < (edge->type == _bots_nav_jump ? 0.25f : 0.8f)) ||
			(edge->type == _bots_nav_teleport && bot_distance2d(position, &edge->from) > 2.0f)))
		{
			brain->edge_begun = FALSE;
			brain->statistics.moves[edge->type]++;
			brain->path_next--;
			if (brain->path_next < 0)
			{
				brain->statistics.goals_reached++;
				brain->path_next = NONE;
				brain->goal_node = NONE;
			}
			bot_action_idle(action, brain->yaw, brain->pitch);
			return;
		}
	}
	/* (off its way: a fall, a push, a teleporter it did not mean to take, a
	jump that fell short, with its way on above it; a ladder's top is a
	floor's height away) */
	if (bot_distance2d(position, target) > BOT_LOST_DISTANCE ||
		(fabs(position->z - target->z) > 2.5f && edge->type == _bots_nav_walk) ||
		(edge->type == _bots_nav_walk && nav->nodes[edge->target].point.z - position->z > 0.6f &&
			nav->nodes[edge->target].point.z - position->z > bot_distance2d(position, &nav->nodes[edge->target].point)))
	{
		if (bot_trace && game_time_get() % TICKS_PER_SECOND == 0)
			platform_log("bots: trace %p tick %ld lost at (%.2f %.2f %.2f) edge type %d target (%.2f %.2f %.2f)", (void *)brain,
				game_time_get(), position->x, position->y, position->z, (int)edge->type, target->x, target->y, target->z);
		brain->path_next = NONE;
		bot_action_idle(action, brain->yaw, brain->pitch);
		return;
	}
	if (bot_trace && game_time_get() % TICKS_PER_SECOND == 0)
	{
		platform_log("bots: trace %p tick %ld at (%.2f %.2f %.2f) edge %ld type %d step %ld of goal %ld target (%.2f %.2f %.2f) begun %d stuck %ld",
			(void *)brain, game_time_get(), position->x, position->y, position->z, brain->path[brain->path_next], (int)edge->type,
			brain->path_next, brain->goal_node, target->x, target->y, target->z, (int)brain->edge_begun, brain->stuck_count);
	}
	if (bot_distance2d(position, target) > 0.05f)
		bot_turn_to(brain, (real)atan2(target->y - position->y, target->x - position->x));
	bot_action_idle(action, brain->yaw, brain->pitch);
	bot_move_toward(action, brain, position, target);
	/* (getting nowhere on a walk: aside for a moment, round what is in the way) */
	if (brain->sidestep_ticks > 0)
	{
		real turn = brain->sidestep_side * DEGREES_TO_RADIANS(70.0f);
		real i = action->throttle.i, j = action->throttle.j;

		brain->sidestep_ticks--;
		action->throttle.i = i * (real)cos(turn) - j * (real)sin(turn);
		action->throttle.j = i * (real)sin(turn) + j * (real)cos(turn);
	}
	/* (pushing on along a walk, hardly moving, a few ticks: a hop, over a
	lip or a ridge the graph walks over; then not again for a little) */
	if (edge->type == _bots_nav_walk && brain->last_step < 0.02f &&
		action->throttle.i * action->throttle.i + action->throttle.j * action->throttle.j > 0.25f)
	{
		if (++brain->stall_ticks > BOT_STALL_TICKS)
		{
			jump = TRUE;
			brain->stall_ticks = -BOT_STALL_TICKS * 2;
		}
	}
	else if (brain->stall_ticks > 0)
	{
		brain->stall_ticks = 0;
	}
	else if (brain->stall_ticks < 0)
	{
		brain->stall_ticks++;
	}
	if (jump || brain->jump)
	{
		/* (a press: held one tick, as a button is) */
		SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
		brain->jump = FALSE;
	}
	else if (brain->crouch_ticks > 0)
	{
		brain->crouch_ticks--;
		SET_FLAG(action->control_flags, _unit_control_crouch_modifier_bit, TRUE);
	}
}

/* ---------- weapons lying about */

/* an item on the ground (not held, in the level) the bot wants
(bot_item_value: a weapon of a kind it has none of, grenades it lacks),
within reach (further for one it wants more), best for its distance first,
gone to: its node the bot's goal. Looked for every few seconds; one not
reached in time is given up */
static void bot_pickup_goal(
	struct bot *bot,
	struct bots_nav *nav,
	long unit_index,
	real_point3d const *position)
{
	struct bot_brain *brain = &bot->brain;
	struct object_iterator iterator;
	struct object_datum *item;
	long now = game_time_get();
	long best = NONE;
	long best_node = NONE;
	real best_score = 1.0e30f;

	if (brain->pickup_object != NONE)
	{
		item = object_try_and_get(brain->pickup_object);
		if (!item || item->object.parent_object_index != NONE || now > brain->pickup_until_tick)
		{
			/* (taken, by it or another, or given up: not tried again, and no
			other looked for a while) */
			if (item && item->object.parent_object_index == unit_index)
			{
				brain->statistics.weapons_taken++;
				/* (a rocket launcher, a sniper rifle, a fuel rod gun, a shotgun) */
				if (item->object.type == _object_type_weapon && bot_weapon_power(brain->pickup_object) >= 2.2f)
					brain->statistics.power_weapons_taken++;
				/* (no other swapped for it for a while: no back and forth) */
				brain->pickup_swap_tick = now + BOT_PICKUP_SWAP_TICKS;
			}
			else if (!item)
			{
				/* (equipment is gone once taken: by it, if it was there) */
				real dx = brain->pickup_position.x - position->x, dy = brain->pickup_position.y - position->y;

				if (dx * dx + dy * dy < 1.5f * 1.5f)
				{
					brain->statistics.items_taken++;
					brain->statistics.powerups_taken += brain->pickup_is_powerup;
				}
			}
			else if (item->object.parent_object_index == NONE)
			{
				brain->pickup_given_up = brain->pickup_object;
				brain->pickup_look_tick = now + 20 * TICKS_PER_SECOND;
			}
			brain->pickup_object = NONE;
			brain->goal_node = NONE;
			brain->path_next = NONE;
		}
		/* (its goal kept on the item: reaching the node is not taking it) */
		else if (brain->goal_node == NONE)
		{
			brain->goal_node = bots_nav_locate(nav, item->object.position.x, item->object.position.y,
				item->object.position.z);
		}
		return;
	}
	if (now < brain->pickup_look_tick)
		return;
	brain->pickup_look_tick = now + 3 * TICKS_PER_SECOND;
	object_iterator_new(&iterator, _object_mask_weapon | _object_mask_equipment, 0);
	while ((item = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		real dx = item->object.position.x - position->x;
		real dy = item->object.position.y - position->y;
		real dz = item->object.position.z - position->z;
		real distance = (real)sqrt(dx * dx + dy * dy + dz * dz);
		real value;
		long node;

		if (item->object.parent_object_index != NONE || !TEST_FLAG(item->object.flags, _object_connected_to_map_bit) ||
			iterator.index == brain->pickup_given_up)
		{
			continue;
		}
		/* (its hands full, not long after a swap: no weapon) */
		if (item->object.type == _object_type_weapon && now < brain->pickup_swap_tick &&
			unit_get_weapon_count(unit_index) >= 2)
		{
			continue;
		}
		value = bot_item_value(bot, unit_index, iterator.index);
		if (value <= 0.0f || distance > BOT_PICKUP_RANGE * value || distance / value >= best_score)
			continue;
		node = bots_nav_locate(nav, item->object.position.x, item->object.position.y, item->object.position.z);
		if (node == NONE || !(nav->nodes[node].reach & _bots_nav_reached_bit))
			continue;
		best = iterator.index;
		best_node = node;
		best_score = distance / value;
	}
	if (best == NONE)
		return;
	brain->pickup_object = best;
	brain->pickup_at_ticks = 0;
	brain->pickup_is_powerup = bot_item_is_powerup(best);
	brain->pickup_position = object_get(best)->object.position;
	brain->pickup_until_tick = now + 10 * TICKS_PER_SECOND;
	brain->goal_node = best_node;
	brain->path_next = NONE;
}

/* at the weapon: the action button pressed (a press every few ticks: it
takes the weapon, swapping its own for it when its hands are full) */
static boolean bot_pickup_act(
	struct bot_brain *brain,
	long unit_index,
	real_point3d const *position,
	struct player_action *action)
{
	short swap;
	boolean ready = TRUE;
	struct object_datum *weapon = brain->pickup_object != NONE ? object_try_and_get(brain->pickup_object) : NULL;
	struct bots_nav_point at;
	real dx, dy;

	if (!weapon)
		return FALSE;
	dx = weapon->object.position.x - position->x;
	dy = weapon->object.position.y - position->y;
	if (bot_trace && game_time_get() % TICKS_PER_SECOND == 0)
	{
		platform_log("bots: trace %p tick %ld pickup %lx at (%.2f %.2f %.2f), it at (%.2f %.2f %.2f) parent %lx %s",
			(void *)brain, game_time_get(), brain->pickup_object, position->x, position->y, position->z,
			weapon->object.position.x, weapon->object.position.y, weapon->object.position.z,
			weapon->object.parent_object_index, tag_get_name(weapon->definition_index));
	}
	if (dx * dx + dy * dy >= 1.5f * 1.5f || fabs(weapon->object.position.z - position->z) >= 0.6f)
		return FALSE;
	/* (near: straight to it, the graph's node being only near it, and the
	action button held, which takes it, swapping a weapon for it when its
	hands are full; standing there is getting somewhere) */
	at.x = weapon->object.position.x;
	at.y = weapon->object.position.y;
	at.z = weapon->object.position.z;
	bot_turn_to(brain, (real)atan2(dy, dx));
	bot_action_idle(action, brain->yaw, brain->pitch);
	if (dx * dx + dy * dy >= 0.3f * 0.3f)
		bot_move_toward(action, brain, position, &at);
	/* (its hands full: the worse of its weapons in hand first, the one the
	swap drops) */
	swap = bot_pickup_swap(unit_index, brain->pickup_object);
	if (swap != NONE && unit_get(unit_index)->unit.current_weapon_index != swap)
	{
		action->desired_weapon_index = swap;
		ready = FALSE;
	}
	/* (a press, again and again: a button held takes nothing more; the
	weapon swap button too, which takes a weapon in place of the one in hand
	when both hands are full, players.c's player_handle_weapon_swap) */
	if (dx * dx + dy * dy < 0.7f * 0.7f)
	{
		if (ready && game_time_get() % 8 < 2)
		{
			SET_FLAG(action->control_flags, _unit_control_action_bit, TRUE);
			SET_FLAG(action->control_flags, _unit_control_swap_weapons_bit, TRUE);
		}
		/* (there a while, and not taken: given up) */
		if (++brain->pickup_at_ticks > BOT_PICKUP_AT_TICKS)
		{
			brain->pickup_given_up = brain->pickup_object;
			brain->pickup_object = NONE;
			brain->pickup_at_ticks = 0;
			brain->goal_node = NONE;
			brain->path_next = NONE;
		}
	}
	/* (standing at it is getting somewhere) */
	brain->check_position = *position;
	brain->check_ticks = BOT_CHECK_TICKS;
	brain->progress_ticks = 0;
	return TRUE;
}

/* ---------- the game type's objective (bots_objectives.c) */

static real bot_flat(
	real_point3d const *position,
	struct bots_nav_point const *point)
{
	real dx = point->x - position->x, dy = point->y - position->y;

	return (real)sqrt(dx * dx + dy * dy);
}

/* a node within the radius of the point, at random, both ways reached (NONE:
none found) */
static long bot_node_within(
	struct bot_brain *brain,
	struct bots_nav const *nav,
	struct bots_nav_point const *point,
	real radius)
{
	short attempt;

	for (attempt = 0; attempt < 12; attempt++)
	{
		real angle = bot_random_real(brain) * 2.0f * _pi;
		real away = (real)sqrt(bot_random_real(brain)) * radius;
		long node = bots_nav_locate(nav, point->x + (real)cos(angle) * away, point->y + (real)sin(angle) * away, point->z + 0.5f);

		if (node != NONE && nav->nodes[node].reach == (_bots_nav_reached_bit | _bots_nav_reaches_bit) &&
			fabs(nav->nodes[node].point.z - point->z) < 1.5f)
		{
			return node;
		}
	}
	return NONE;
}

/* of a few nodes at random, the one furthest from the bot's enemies */
static long bot_flee_node(
	struct bot_brain *brain,
	struct bots_nav const *nav,
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	long best = NONE;
	real best_score = -1.0f;
	short attempt;

	for (attempt = 0; attempt < 8; attempt++)
	{
		long node = bot_random_goal(brain, nav);
		struct data_iterator iterator;
		struct player_datum *other;
		real nearest = 1.0e30f;

		if (node == NONE)
			continue;
		data_iterator_new(&iterator, player_data);
		while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			struct object_datum *enemy;
			real distance;

			if (other->unit_index == NONE || other->unit_index == unit_index)
				continue;
			enemy = object_get(other->unit_index);
			if (!game_team_is_enemy(unit->object.owner_team_index, enemy->object.owner_team_index))
				continue;
			distance = bot_flat(&enemy->object.position, &nav->nodes[node].point);
			if (distance < nearest)
				nearest = distance;
		}
		if (nearest > best_score)
		{
			best_score = nearest;
			best = node;
		}
	}
	return best;
}

/* the bot's goal on the graph the objective's point, planned again when the
point has moved */
static void bot_objective_head_for(
	struct bot_brain *brain,
	struct bots_nav *nav,
	struct bots_nav_point const *point)
{
	long now = game_time_get();
	real dx = point->x - brain->objective_planned.x, dy = point->y - brain->objective_planned.y;

	if (brain->goal_node == NONE ||
		(dx * dx + dy * dy > 1.5f * 1.5f && now - brain->objective_plan_tick >= TICKS_PER_SECOND / 2))
	{
		long node = bots_nav_locate(nav, point->x, point->y, point->z + 0.5f);

		brain->objective_planned = *point;
		brain->objective_plan_tick = now;
		if (node != NONE && node != brain->goal_node)
		{
			brain->goal_node = node;
			brain->path_next = NONE;
		}
	}
}

/* a fighting bot's goal, its objective's where that comes first: a flag
carried home, a flag near to take, a hill or a flag's stand it does not
leave, a carrier it hunts */
static struct bots_nav_point const *bot_objective_combat_goal(
	struct bot *bot,
	long unit_index,
	real_point3d const *position,
	struct bots_nav_point const *goal)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_objective const *objective = &brain->objective;
	struct bots_nav *nav = bots_nav_current();

	switch (objective->kind)
	{
	case _bot_objective_carry:
		if (objective->flee && nav)
		{
			if (game_time_get() >= brain->objective_wander_tick || brain->goal_node == NONE)
			{
				long node = bot_flee_node(brain, nav, unit_index);

				brain->objective_wander_tick = game_time_get() + 3 * TICKS_PER_SECOND;
				if (node != NONE)
					brain->objective_planned = nav->nodes[node].point;
			}
			return &brain->objective_planned;
		}
		return &objective->point;
	case _bot_objective_take:
		return bot_flat(position, &objective->point) < 10.0f ? &objective->point : goal;
	case _bot_objective_hold:
		if (bot_flat(position, &objective->point) > objective->radius + 2.0f)
			return &objective->point;
		/* (no chase out of it) */
		if (goal && bot_flat(position, goal) > 2.0f &&
			(goal->x - objective->point.x) * (goal->x - objective->point.x) +
			(goal->y - objective->point.y) * (goal->y - objective->point.y) >
			(objective->radius + 4.0f) * (objective->radius + 4.0f))
		{
			return NULL;
		}
		return goal;
	case _bot_objective_hunt:
		return goal ? &objective->point : NULL;
	default:
		return goal;
	}
}

/* the objective's moves when the bot fights nobody (FALSE: no objective) */
static boolean bot_objective_move(
	struct bot *bot,
	struct bots_nav *nav,
	long unit_index,
	real_point3d const *position,
	struct player_action *action)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_objective const *objective = &brain->objective;
	long now = game_time_get();
	real distance;

	if (objective->kind == _bot_objective_none || !nav)
		return FALSE;
	distance = bot_flat(position, &objective->point);
	if (objective->kind == _bot_objective_hold && distance <= objective->radius)
	{
		/* (within it: a spot within it now and then, and there it stands,
		looking about) */
		if (now >= brain->objective_wander_tick)
		{
			long node = bot_node_within(brain, nav, &objective->point, objective->radius);

			brain->objective_wander_tick = now + (long)((2.0f + bot_random_real(brain) * 3.0f) * TICKS_PER_SECOND);
			if (node != NONE)
			{
				brain->goal_node = node;
				brain->path_next = NONE;
			}
		}
		if (brain->goal_node == NONE)
		{
			bot_turn_to(brain, brain->yaw + 0.05f);
			bot_action_idle(action, brain->yaw, brain->pitch);
			brain->holding = TRUE;
			return TRUE;
		}
		bot_roam(brain, nav, position, action);
		return TRUE;
	}
	if (objective->kind == _bot_objective_carry && objective->flee)
	{
		/* (the ball: away from its enemies) */
		if (now >= brain->objective_wander_tick || brain->goal_node == NONE)
		{
			long node = bot_flee_node(brain, nav, unit_index);

			brain->objective_wander_tick = now + 4 * TICKS_PER_SECOND;
			if (node != NONE)
			{
				brain->goal_node = node;
				brain->path_next = NONE;
			}
		}
		bot_roam(brain, nav, position, action);
		return TRUE;
	}
	/* (there: straight onto it, a ball taken with a press) */
	if (distance < 1.5f && fabs(position->z - objective->point.z) < 0.8f && objective->kind != _bot_objective_hunt)
	{
		bot_turn_to(brain, (real)atan2(objective->point.y - position->y, objective->point.x - position->x));
		bot_action_idle(action, brain->yaw, brain->pitch);
		if (distance > 0.2f)
			bot_move_toward(action, brain, position, &objective->point);
		if (objective->kind == _bot_objective_take && now % 8 < 2)
		{
			SET_FLAG(action->control_flags, _unit_control_action_bit, TRUE);
			SET_FLAG(action->control_flags, _unit_control_swap_weapons_bit, TRUE);
		}
		brain->check_position = *position;
		brain->check_ticks = BOT_CHECK_TICKS;
		brain->progress_ticks = 0;
		return TRUE;
	}
	bot_objective_head_for(brain, nav, &objective->point);
	bot_roam(brain, nav, position, action);
	return TRUE;
}

/* ---------- noises */

/* a noise it heard (bots_hearing.c), not long ago: it turns to look that
way, then goes to see, along the graph, until it is there (FALSE: nothing
to see to) */
static boolean bot_investigate(
	struct bot *bot,
	struct bots_nav *nav,
	real_point3d const *position,
	struct player_action *action)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_combat *combat = &brain->combat;
	long now = game_time_get();
	real dx = combat->heard_position.x - position->x;
	real dy = combat->heard_position.y - position->y;

	if (bot->difficulty == _bot_difficulty_peaceful || now - combat->heard_tick > BOT_INVESTIGATE_FORGET_TICKS)
		return FALSE;
	if (now < combat->look_until_tick)
	{
		/* (a quick turn, as to a shot nearby; standing to look is getting
		somewhere) */
		bot_turn_to(brain, (real)atan2(dy, dx));
		bot_turn_to(brain, (real)atan2(dy, dx));
		bot_action_idle(action, brain->yaw, brain->pitch);
		brain->check_position = *position;
		brain->progress_ticks = 0;
		return TRUE;
	}
	if (!nav)
		return FALSE;
	if (combat->heard_new)
	{
		long node = bots_nav_locate(nav, combat->heard_position.x, combat->heard_position.y, combat->heard_position.z);

		combat->heard_new = FALSE;
		if (node == NONE || !(nav->nodes[node].reach & _bots_nav_reached_bit))
		{
			combat->heard_tick = -BOT_INVESTIGATE_FORGET_TICKS;
			return FALSE;
		}
		brain->goal_node = node;
		brain->path_next = NONE;
		brain->pickup_object = NONE;
	}
	/* (there, or its way lost: seen to) */
	if (dx * dx + dy * dy < 2.0f * 2.0f || brain->goal_node == NONE)
	{
		combat->heard_tick = -BOT_INVESTIGATE_FORGET_TICKS;
		return FALSE;
	}
	bot_roam(brain, nav, position, action);
	return TRUE;
}

/* ---------- getting nowhere */

static void bot_stuck(
	struct bot_brain *brain,
	struct bots_nav *nav);

static void bot_check_progress(
	struct bot_brain *brain,
	struct bots_nav *nav,
	real_point3d const *position)
{
	real dx = position->x - brain->check_position.x;
	real dy = position->y - brain->check_position.y;
	/* (up or down counts: a ladder climbed is getting somewhere) */
	real dz = position->z - brain->check_position.z;

	if (brain->avoided_ticks > 0)
		brain->avoided_ticks--;
	/* (standing its ground in a fight: not stuck) */
	if (brain->holding)
	{
		brain->check_position = *position;
		brain->check_ticks = BOT_CHECK_TICKS;
		brain->progress_ticks = 0;
		return;
	}
	/* (a way it goes back and forth on, getting no further: stuck too) */
	if (nav && brain->path_next != NONE)
	{
		long step = brain->path_next * 2 + (brain->edge_begun ? 0 : 1);

		if (step != brain->progress_step)
		{
			brain->progress_step = step;
			brain->progress_ticks = 0;
		}
		else if (++brain->progress_ticks > 4 * TICKS_PER_SECOND)
		{
			brain->progress_ticks = 0;
			bot_stuck(brain, nav);
		}
	}
	if (--brain->check_ticks > 0)
		return;
	brain->check_ticks = BOT_CHECK_TICKS;
	brain->check_position = *position;
	/* (at its goal, or between goals, it is not stuck) */
	if (dx * dx + dy * dy + dz * dz >= BOT_PROGRESS * BOT_PROGRESS || (nav && brain->path_next == NONE))
	{
		brain->stuck_count = 0;
		return;
	}
	brain->statistics.stuck_ticks += BOT_CHECK_TICKS;
	bot_stuck(brain, nav);
}

/* getting nowhere: a jump; again, a new way; again, another goal, keeping
off the edge it was on */
static void bot_stuck(
	struct bot_brain *brain,
	struct bots_nav *nav)
{
	brain->stuck_count++;
	brain->statistics.stuck++;
	brain->jump = TRUE;
	if (!nav)
		return;
	/* (on a walk: a step aside too, one way, then the other) */
	if (brain->path_next != NONE && nav->edges[brain->path[brain->path_next]].type == _bots_nav_walk)
	{
		brain->sidestep_side = brain->stuck_count % 2 ? (bot_random_real(brain) < 0.5f ? -1.0f : 1.0f) : -brain->sidestep_side;
		brain->sidestep_ticks = BOT_SIDESTEP_TICKS;
	}
	if (bot_trace)
	{
		struct bots_nav_edge const *edge = brain->path_next != NONE ? &nav->edges[brain->path[brain->path_next]] : NULL;

		struct collision_result collision;
		real_point3d from = brain->last_position;
		real_vector3d vector = { 0.0f, 0.0f, 0.0f };
		boolean hit = FALSE;

		if (edge)
		{
			struct bots_nav_point const *to = &nav->nodes[edge->target].point;

			from.z += 0.35f;
			vector.i = to->x - from.x;
			vector.j = to->y - from.y;
			vector.k = to->z + 0.35f - from.z;
			hit = collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_back_facing_surfaces_bit) |
				FLAG(_collision_test_structure_bit) | FLAG(_collision_test_objects_bit) |
				_collision_test_objects_all_types_flags, &from, &vector, brain->unit_index, &collision);
		}
		platform_log("bots: stuck %p at (%.2f %.2f %.2f) count %ld edge %ld type %d begun %d from (%.2f %.2f %.2f) to (%.2f %.2f %.2f)"
			" pickup %lx; line to node %s %d at (%.2f %.2f %.2f) object %s",
			(void *)brain, brain->last_position.x, brain->last_position.y, brain->last_position.z, brain->stuck_count,
			edge ? brain->path[brain->path_next] : -1L, edge ? (int)edge->type : -1, (int)brain->edge_begun,
			edge ? edge->from.x : 0.0f, edge ? edge->from.y : 0.0f, edge ? edge->from.z : 0.0f,
			edge ? edge->to.x : 0.0f, edge ? edge->to.y : 0.0f, edge ? edge->to.z : 0.0f, brain->pickup_object,
			hit ? "hits" : "clear", hit ? (int)collision.type : -1, hit ? collision.point.x : 0.0f, hit ? collision.point.y : 0.0f,
			hit ? collision.point.z : 0.0f,
			hit && collision.type == _collision_result_object && object_try_and_get(collision.object_index) ?
				tag_get_name(object_get(collision.object_index)->definition_index) : "-");
	}
	if (brain->stuck_count == 2)
	{
		/* (a new way to the same goal) */
		brain->path_next = NONE;
	}
	else if (brain->stuck_count >= 3)
	{
		/* (another goal, keeping off the edge it was on) */
		if (brain->path_next != NONE)
		{
			bot_edge_failed(nav, brain->path[brain->path_next]);
			brain->avoided_edge = brain->path[brain->path_next];
			brain->avoided_ticks = BOT_AVOID_TICKS;
		}
		brain->path_next = NONE;
		brain->goal_node = NONE;
		brain->stuck_count = 0;
		brain->pickup_object = NONE;
	}
}

void bot_brain_think(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct player_action *action)
{
	struct bot_brain *brain = &bot->brain;
	struct object_datum *object = object_get(unit_index);
	struct bots_nav *nav = bots_nav_current();
	real_point3d position = object->object.position;

	if (!bot_trace_checked)
	{
		bot_trace_checked = TRUE;
		bot_trace = config_boolean("debug.bot_trace") != 0;
	}

	/* (a new life: it faces where its unit spawned facing, and plans afresh) */
	if (brain->unit_index != unit_index)
	{
		brain->unit_index = unit_index;
		brain->yaw = bot_yaw_normalize((real)atan2(object->object.forward.j, object->object.forward.i));
		brain->pitch = 0.0f;
		brain->target_yaw = brain->yaw;
		brain->turn_ticks = bot_random_range(brain, BOT_WANDER_TURN_TICKS_MINIMUM, BOT_WANDER_TURN_TICKS_RANGE);
		brain->check_position = position;
		brain->check_ticks = BOT_CHECK_TICKS;
		brain->last_position = position;
		brain->path_next = NONE;
		brain->goal_node = NONE;
		brain->stuck_count = 0;
	}
	{
		real dx = position.x - brain->last_position.x, dy = position.y - brain->last_position.y;
		real step = (real)sqrt(dx * dx + dy * dy);

		/* (a teleporter's move is no walk) */
		if (step < 2.0f)
			brain->statistics.travelled += step;
		brain->last_position = position;
		brain->last_step = step;
	}
	bot_check_progress(brain, nav, &position);
	if (bot_trace)
		bot_grenade_follow(bot);
	/* (a live grenade near: noticed, by any bot) */
	if ((game_time_get() + bot->slot) % 3 == 0)
		bot_sense_danger(bot, unit_index);
	/* (the game type's objective, a few times a second) */
	if ((game_time_get() + bot->slot) % 10 == 0)
		bot_objective_update(bot, player_index, unit_index, &brain->objective);
	if (bot_combat_engaged(bot, player_index, unit_index, bot->slot))
	{
		struct bots_nav_point goal_point;
		struct bots_nav_point const *goal = bot_combat_goal(bot, &position, &goal_point);
		boolean moving = FALSE;

		goal = bot_objective_combat_goal(bot, unit_index, &position, goal);
		brain->holding = !goal || !nav;
		brain->pickup_object = NONE;
		/* (to where it last saw its target, along the graph: the goal given
		again at most once a second, as the target moves) */
		if (goal && nav)
		{
			long node = bots_nav_locate(nav, goal->x, goal->y, goal->z);

			if (node != NONE && node != brain->combat_goal_node &&
				game_time_get() - brain->combat_goal_tick >= TICKS_PER_SECOND)
			{
				brain->combat_goal_node = node;
				brain->combat_goal_tick = game_time_get();
				brain->goal_node = node;
				brain->path_next = NONE;
			}
			bot_roam(brain, nav, &position, action);
			moving = TRUE;
		}
		else
		{
			bot_action_idle(action, brain->yaw, brain->pitch);
		}
		bot_combat_act(bot, unit_index, action, moving);
	}
	else
	{
		brain->combat_goal_node = NONE;
		brain->holding = FALSE;
		/* (an objective that cannot wait - a flag to carry home or to take, a
		ball, a carrier to hunt - first; then an item it wants; then the
		objective's place to be; then a noise it heard; then roaming) */
		boolean urgent = brain->objective.kind == _bot_objective_carry || brain->objective.kind == _bot_objective_take ||
			brain->objective.kind == _bot_objective_hunt;

		if (nav && !urgent)
			bot_pickup_goal(bot, nav, unit_index, &position);
		else
			brain->pickup_object = NONE;
		if (urgent && bot_objective_move(bot, nav, unit_index, &position, action))
		{
			/* (playing the game type) */
		}
		else if (nav && brain->pickup_object != NONE)
		{
			if (!bot_pickup_act(brain, unit_index, &position, action))
				bot_roam(brain, nav, &position, action);
		}
		else if (bot_objective_move(bot, nav, unit_index, &position, action))
		{
			/* (playing the game type) */
		}
		else if (bot_investigate(bot, nav, &position, action))
		{
			/* (going to see what it heard) */
		}
		else if (nav)
		{
			bot_roam(brain, nav, &position, action);
		}
		else
		{
			bot_wander(brain, &position, action);
		}
		bot_combat_idle(bot, unit_index, action);
		/* (a grenade it began to throw at a target now gone: the throw
		finished with the aim it had) */
		if (brain->combat.throwing)
			bot_grenade_act(bot, unit_index, action);
	}
	if (!nav && brain->jump)
	{
		SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
		brain->jump = FALSE;
	}
	/* (and away from a grenade, whatever else it does) */
	bot_dodge(bot, unit_index, action);
}
