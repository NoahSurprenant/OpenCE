/* bots_skate.c: bots on boards (bots_internal.h; port/skate/README.md, "Bots
on boards")

On a host with Skate 3 mode and its data (skate.c's skate_bots_available),
a bot rides a board to travel and steps off to fight: a skater's weapon is
holstered and the host takes no shot from one. Each tick, after the bot's
brain has chosen where to go (bots_brain.c: its path along the walking
graph, its objective, a weapon, a noise) and how to fight, this decides
whether the bot is on a board and, if so, the pad it holds (skate.c's
skate_bot_drive runs the skate engine for it):

- it gets on when the way ahead along its path rolls (walks and small drops,
  not too steep) for BOT_SKATE_BOARD_DISTANCE or more, it fights nobody, holds
  no flag or ball, and has not just stepped off;
- it steers the board toward where its brain wanted to walk (the brain's
  throttle, which is toward its next point along the path), pushes while
  slower than the speed the turn ahead allows, ollies when it stalls against a
  lip, and steps back on when a fall left it walking;
- it steps off for a fight (a target in sight), a ladder, jump or teleporter
  coming, its goal near, a flag or ball in hand, or being stuck;
- a bot that may fight (not peaceful) rides at a person on foot it sees
  near, to run them over (skate_authority.c: as a vehicle does), and a
  peaceful one never runs anyone over (skate_bot_drive's run_over). */

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "game/player_queues_new.h"
#include "objects/objects.h"
#include "units/units.h"

#include "bots_internal.h"
#include "skate.h"
#include "../../skate/include/halo_skate.h"

#include <math.h>
#include <string.h>

void platform_log(const char *format, ...);

/* how far along its path the way must roll for a bot to get on (world
units: 15, about 46 m); how near its goal (or a part of the way that does
not roll) it steps off */
#define BOT_SKATE_BOARD_DISTANCE 15.0f
#define BOT_SKATE_OFF_DISTANCE 2.5f
/* the steepest walk a board goes up and down (rise over run) */
#define BOT_SKATE_STEEPEST_UP 0.45f
#define BOT_SKATE_STEEPEST_DOWN 0.8f
/* the highest drop a board rolls off (world units) */
#define BOT_SKATE_HIGHEST_DROP 1.0f
/* the ticks a bot stays on foot after stepping off (or failing to get on) */
#define BOT_SKATE_OFF_TICKS (4 * TICKS_PER_SECOND)
/* the speeds it rides at: pushing up to the fast one on a straight way, the
slow one in a turn (world units a second: 7 m/s and 3 m/s) */
#define BOT_SKATE_FAST_SPEED (7.0f / 3.048f)
#define BOT_SKATE_SLOW_SPEED (3.0f / 3.048f)
/* the turn (radians off its heading) the left stick is pushed fully for */
#define BOT_SKATE_FULL_TURN 0.6f
/* pushes: the button held this many ticks of each cycle */
#define BOT_SKATE_PUSH_TICKS 15
#define BOT_SKATE_PUSH_CYCLE 30
/* stalled pushing (slower than this, world units a second) this many ticks:
an ollie (a lip in the way); stalled this many: off, stuck */
#define BOT_SKATE_STALL_SPEED 0.15f
#define BOT_SKATE_STALL_OLLIE_TICKS 20
#define BOT_SKATE_STUCK_TICKS (4 * TICKS_PER_SECOND)
/* the ollie: the right stick down so many ticks, then up (skate.c's
scripted pad's timings) */
#define BOT_SKATE_OLLIE_DOWN_TICKS 8
#define BOT_SKATE_OLLIE_UP_TICKS 2
/* off the board, walking, this many ticks: Y, back on */
#define BOT_SKATE_STEP_ON_TICKS 15
/* ramming: a person on foot it sees within this (world units), and as high
within this, is ridden at */
#define BOT_SKATE_RAM_DISTANCE 15.0f
#define BOT_SKATE_RAM_HEIGHT 1.2f
/* the pad's buttons (halo_skate.h: XINPUT's) */
#define BOT_SKATE_A 0x1000
#define BOT_SKATE_Y 0x8000
#define BOT_SKATE_FULL 32767

/* the bot's unit: alive, a biped on foot (none in a vehicle skates) */
static boolean bot_skate_unit_able(
	long unit_index)
{
	struct object_datum *object = object_try_and_get(unit_index);

	return object && object->object.type == _object_type_biped && object->object.parent_object_index == NONE &&
		!TEST_FLAG(object->object.damage_flags, _object_dead_bit);
}

/* whether an edge of the walking graph rolls: a walk not too steep, or a
small drop */
static boolean bot_skate_edge_rolls(
	struct bots_nav const *nav,
	struct bots_nav_edge const *edge,
	struct bots_nav_point const *from)
{
	struct bots_nav_point const *to = &nav->nodes[edge->target].point;
	real dx = to->x - from->x, dy = to->y - from->y;
	real run = (real)sqrt(dx * dx + dy * dy);
	real rise = to->z - from->z;

	if (edge->type == _bots_nav_drop)
		return from->z - to->z <= BOT_SKATE_HIGHEST_DROP;
	if (edge->type != _bots_nav_walk)
		return FALSE;
	if (run < 0.05f)
		return fabs(rise) < 0.1f;
	return rise <= run * BOT_SKATE_STEEPEST_UP && -rise <= run * BOT_SKATE_STEEPEST_DOWN;
}

/* along the bot's path from where it is: how far the way rolls before a part
that does not (or its end), and whether it ends there (the goal) rather than
at such a part */
static real bot_skate_rolling_distance(
	struct bot_brain const *brain,
	struct bots_nav const *nav,
	real_point3d const *position,
	boolean *to_goal)
{
	struct bots_nav_point from;
	real distance = 0.0f;
	long step;

	*to_goal = FALSE;
	if (!nav || brain->path_next == NONE || brain->path_next < 0)
		return 0.0f;
	from.x = position->x;
	from.y = position->y;
	from.z = position->z;
	for (step = brain->path_next; step >= 0; step--)
	{
		struct bots_nav_edge const *edge = &nav->edges[brain->path[step]];
		struct bots_nav_point const *to = &nav->nodes[edge->target].point;
		real dx = to->x - from.x, dy = to->y - from.y;

		if (!bot_skate_edge_rolls(nav, edge, &from))
			return distance;
		distance += (real)sqrt(dx * dx + dy * dy);
		from = *to;
	}
	*to_goal = TRUE;
	return distance;
}

static real bot_skate_angle(
	real angle)
{
	while (angle > _pi)
		angle -= 2.0f * _pi;
	while (angle < -_pi)
		angle += 2.0f * _pi;
	return angle;
}

/* the person on foot the bot rides at: its target, seen, near, about as high,
neither skating nor in a vehicle; NONE for none */
static long bot_skate_ram_target(
	struct bot const *bot,
	real_point3d const *position)
{
	struct player_datum *player;
	struct object_datum *object;
	real dx, dy;

	if (bot->difficulty == _bot_difficulty_peaceful || bot->brain.combat.target_player == NONE ||
		!bot->brain.combat.target_visible)
	{
		return NONE;
	}
	player = player_try_and_get(bot->brain.combat.target_player);
	if (!player || player->unit_index == NONE || !bot_skate_unit_able(player->unit_index) ||
		skate_unit_is_skating(player->unit_index))
	{
		return NONE;
	}
	object = object_get(player->unit_index);
	dx = object->object.position.x - position->x;
	dy = object->object.position.y - position->y;
	if (dx * dx + dy * dy > BOT_SKATE_RAM_DISTANCE * BOT_SKATE_RAM_DISTANCE ||
		fabs(object->object.position.z - position->z) > BOT_SKATE_RAM_HEIGHT)
	{
		return NONE;
	}
	return player->unit_index;
}

/* the pad that rides toward heading (radians) at up to speed (world units a
second) */
static void bot_skate_pad(
	struct bot_brain *brain,
	struct skate_bot_view const *view,
	real heading,
	real speed_wanted,
	struct halo_skate_pad *pad)
{
	struct bot_skate *skate = &brain->skate;
	real speed = (real)sqrt(view->velocity[0] * view->velocity[0] + view->velocity[1] * view->velocity[1]);
	/* (the way it goes; at a standstill, the way the board points) */
	real course = speed > 0.3f ? (real)atan2(view->velocity[1], view->velocity[0]) :
		(real)atan2(view->forward[1], view->forward[0]);
	real error = bot_skate_angle(heading - course);
	real steer = PIN(error / BOT_SKATE_FULL_TURN, -1.0f, 1.0f);

	memset(pad, 0, sizeof(*pad));
	/* (the left stick to the right turns right: clockwise, a yaw falling) */
	pad->left[0] = (short)(-steer * BOT_SKATE_FULL);
	/* off the board and walking (a fall): back on */
	if (view->off_board && !view->bailing)
	{
		if (++skate->walking_ticks >= BOT_SKATE_STEP_ON_TICKS)
		{
			pad->buttons |= BOT_SKATE_Y;
			skate->walking_ticks = 0;
		}
		return;
	}
	skate->walking_ticks = 0;
	if (view->bailing || view->airborne)
		return;
	/* an ollie under way */
	if (skate->ollie_ticks > 0)
	{
		skate->ollie_ticks--;
		pad->right[1] = (short)(skate->ollie_ticks >= BOT_SKATE_OLLIE_UP_TICKS ? -BOT_SKATE_FULL : BOT_SKATE_FULL);
		if (!skate->ollie_ticks)
			brain->statistics.skate_ollies++;
		return;
	}
	/* pushing, while slower than wanted and facing about the way to go */
	if (speed < speed_wanted && fabs(error) < 0.8f)
	{
		if (game_time_get() % BOT_SKATE_PUSH_CYCLE < BOT_SKATE_PUSH_TICKS)
			pad->buttons |= BOT_SKATE_A;
		/* (stalled pushing: a lip in the way - an ollie over it) */
		if (speed < BOT_SKATE_STALL_SPEED)
		{
			if (++skate->stall_ticks == BOT_SKATE_STALL_OLLIE_TICKS)
				skate->ollie_ticks = BOT_SKATE_OLLIE_DOWN_TICKS + BOT_SKATE_OLLIE_UP_TICKS;
		}
		else
			skate->stall_ticks = 0;
	}
	else if (speed >= BOT_SKATE_STALL_SPEED)
	{
		skate->stall_ticks = 0;
	}
}

/* (bots_brain.c's bot_brain_think, last) the bot on a board or off it, and
on one its pad, the action it chose made one a skater can do: nothing but its
facing (the board carries it; it neither fires nor throws) */
void bot_skate_think(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct player_action *action)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_skate *skate = &brain->skate;
	struct bots_nav *nav = bots_nav_current();
	struct skate_bot_view view;
	struct halo_skate_pad pad;
	real_point3d position = object_get(unit_index)->object.position;
	boolean has_skater;
	boolean want;
	boolean to_goal = FALSE;
	boolean fighting;
	long ram_unit;
	real rolling;
	long now = game_time_get();

	if (!skate_bots_available() || !bot_skate_unit_able(unit_index))
	{
		skate->on = FALSE;
		return;
	}
	has_skater = skate_bot_frame(player_index, &view);
	rolling = bot_skate_rolling_distance(brain, nav, &position, &to_goal);
	fighting = brain->combat.target_player != NONE && brain->combat.target_visible;
	ram_unit = view.skating ? bot_skate_ram_target(bot, &position) : NONE;
	/* (a life, a game: afresh) */
	if (skate->unit_index != unit_index || now < skate->changed_tick)
	{
		memset(skate, 0, sizeof(*skate));
		skate->unit_index = unit_index;
		skate->changed_tick = now;
		skate->off_until_tick = now + BOT_SKATE_OFF_TICKS;
	}
	if (skate->on)
	{
		/* off: a fight (but a person on foot to run over), a flag or ball in
		hand, its goal near, a part ahead that does not roll, stuck */
		char const *reason = NULL;

		if (fighting && ram_unit == NONE)
			reason = "a fight";
		else if (bot_holds_flag(unit_get(unit_index)))
			reason = "a flag in hand";
		else if (ram_unit == NONE && brain->path_next != NONE && rolling < BOT_SKATE_OFF_DISTANCE)
			reason = to_goal ? "its goal near" : "the way ahead does not roll";
		else if (skate->stuck_ticks > BOT_SKATE_STUCK_TICKS)
			reason = "stuck";
		else if (has_skater && !view.skating && !view.ready && now - skate->changed_tick > 30 * TICKS_PER_SECOND)
			reason = "its skater never got ready";
		if (reason)
		{
			skate->on = FALSE;
			skate->changed_tick = now;
			skate->off_until_tick = now + BOT_SKATE_OFF_TICKS;
			if (view.skating)
				platform_log("bots: player %ld steps off the board (%s) at tick %ld", DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index),
					reason, now);
		}
	}
	else if (now >= skate->off_until_tick && !fighting && !bot_holds_flag(unit_get(unit_index)) &&
		rolling >= BOT_SKATE_BOARD_DISTANCE)
	{
		skate->on = TRUE;
		skate->changed_tick = now;
		skate->stuck_ticks = 0;
		skate->stall_ticks = 0;
	}
	want = skate->on;
	if (!want)
	{
		/* (its skater, if it has one, off the board; a while on foot, it lets
		it go for another bot: skate.c gives up one not asked for) */
		if (has_skater && (view.skating || now - skate->changed_tick < 2 * TICKS_PER_SECOND))
			skate_bot_drive(player_index, unit_index, FALSE, NULL, FALSE);
		skate->riding = FALSE;
		return;
	}
	memset(&pad, 0, sizeof(pad));
	if (view.skating)
	{
		real heading;
		real speed_wanted = BOT_SKATE_FAST_SPEED;
		real moved = (real)sqrt(view.velocity[0] * view.velocity[0] + view.velocity[1] * view.velocity[1]);

		if (!skate->riding)
		{
			skate->riding = TRUE;
			brain->statistics.skate_boardings++;
			platform_log("bots: player %ld gets on a board at tick %ld, %.0f world units of the way rolling",
				DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), now, rolling);
		}
		brain->statistics.skate_ticks++;
		brain->statistics.skate_travelled += brain->last_step;
		if (view.bailing && !skate->bailing)
			brain->statistics.skate_bails++;
		skate->bailing = view.bailing;
		if (ram_unit != NONE)
		{
			/* at the person, a little ahead of where they go */
			struct object_datum *target = object_get(ram_unit);
			real tx = target->object.position.x + target->object.translational_velocity.i * 6.0f;
			real ty = target->object.position.y + target->object.translational_velocity.j * 6.0f;

			heading = (real)atan2(ty - position.y, tx - position.x);
			if (!skate->ramming)
			{
				brain->statistics.skate_rams++;
				platform_log("bots: player %ld rides at a person on foot at tick %ld", DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index),
					now);
			}
			skate->ramming = TRUE;
		}
		else
		{
			/* where the brain wanted to walk (toward its next point) */
			real along = (real)sqrt(action->throttle.i * action->throttle.i + action->throttle.j * action->throttle.j);

			skate->ramming = FALSE;
			if (along > 0.1f)
				heading = brain->yaw + (real)atan2(action->throttle.j, action->throttle.i);
			else
			{
				heading = (real)atan2(view.forward[1], view.forward[0]);
				speed_wanted = 0.0f;
			}
			/* (slower in a turn, and near the end of the rolling way) */
			if (moved > 0.3f && fabs(bot_skate_angle(heading - (real)atan2(view.velocity[1], view.velocity[0]))) > 0.5f)
				speed_wanted = BOT_SKATE_SLOW_SPEED;
			if (rolling < 2.0f * BOT_SKATE_OFF_DISTANCE + 2.0f)
				speed_wanted = MIN(speed_wanted, BOT_SKATE_SLOW_SPEED);
		}
		bot_skate_pad(brain, &view, heading, speed_wanted, &pad);
		/* (getting nowhere on the board: off, after a while) */
		if (speed_wanted > 0.0f && moved < BOT_SKATE_STALL_SPEED && !view.bailing)
			skate->stuck_ticks++;
		else
			skate->stuck_ticks = 0;
	}
	if (!skate_bot_drive(player_index, unit_index, TRUE, &pad, bot->difficulty != _bot_difficulty_peaceful))
	{
		/* (no skater to be had: on foot a while) */
		skate->on = FALSE;
		skate->off_until_tick = now + 10 * BOT_SKATE_OFF_TICKS;
		return;
	}
	if (!view.skating)
	{
		/* (its skater still being made, or not on yet: it walks on) */
		skate->riding = FALSE;
		return;
	}
	/* on the board: the brain's moves, fire and throws come to nothing (the
	board carries it, its weapon holstered); it faces the way it rides */
	{
		real yaw = (real)atan2(view.forward[1], view.forward[0]);

		bot_action_idle(action, yaw < 0.0f ? yaw + 2.0f * _pi : yaw, 0.0f);
	}
}
