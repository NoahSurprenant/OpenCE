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
#include "game/players.h"
#include "objects/objects.h"
#include "units/units.h"

#include "bots_internal.h"

#include <math.h>

/* the platform layer's (port_config.c, the port's log) */
int config_boolean(const char *name);
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
/* the ticks an edge a bot got stuck on is kept off */
#define BOT_AVOID_TICKS (10 * TICKS_PER_SECOND)

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

static real bot_distance2d(
	real_point3d const *position,
	struct bots_nav_point const *point)
{
	real dx = point->x - position->x, dy = point->y - position->y;

	return (real)sqrt(dx * dx + dy * dy);
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
		brain->goal_node = bot_random_goal(brain, nav);
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
		if (!along)
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
		if (bot_distance2d(position, target) < BOT_NODE_REACHED && fabs(position->z - target->z) < 1.0f)
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
		if (!brain->edge_begun && bot_distance2d(position, &edge->from) < 0.6f)
		{
			brain->edge_begun = TRUE;
			brain->climb_yaw = (real)atan2(edge->from.y - position->y, edge->from.x - position->x);
		}
		target = brain->edge_begun && position->z > edge->to.z - 0.05f ? &edge->to : &edge->from;
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
		/* a drop, a jump or a teleporter: to its start, then across */
		if (!brain->edge_begun && bot_distance2d(position, &edge->from) < BOT_MOVE_BEGUN)
		{
			brain->edge_begun = TRUE;
			jump = edge->type == _bots_nav_jump;
			brain->jump_ticks = 0;
			if (jump && bot_trace)
				platform_log("bots: trace %p jump from (%.2f %.2f %.2f) up %.2f to (%.2f %.2f %.2f)", (void *)brain, position->x,
					position->y, position->z, edge->to.z - edge->from.z, edge->to.x, edge->to.y, edge->to.z);
			if (jump && edge->to.z - edge->from.z > 0.5f)
				brain->crouch_ticks = 12;
		}
		target = brain->edge_begun ? &edge->to : &edge->from;
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
		/* (a jump that did not make it, after a second and a half: kept off,
		and another way) */
		if (brain->edge_begun && edge->type == _bots_nav_jump && ++brain->jump_ticks > 45)
		{
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

	if (brain->avoided_ticks > 0)
		brain->avoided_ticks--;
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
	if (dx * dx + dy * dy >= BOT_PROGRESS * BOT_PROGRESS || (nav && brain->path_next == NONE))
	{
		brain->stuck_count = 0;
		return;
	}
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
			brain->avoided_edge = brain->path[brain->path_next];
			brain->avoided_ticks = BOT_AVOID_TICKS;
		}
		brain->path_next = NONE;
		brain->goal_node = NONE;
		brain->stuck_count = 0;
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
	}
	bot_check_progress(brain, nav, &position);
	if (nav)
		bot_roam(brain, nav, &position, action);
	else
		bot_wander(brain, &position, action);
	if (!nav && brain->jump)
	{
		SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
		brain->jump = FALSE;
	}
	(void)player_index;
}
