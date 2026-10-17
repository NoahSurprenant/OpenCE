/* bots_brain.c: what a bot does each tick (bots_internal.h)

For now a bot wanders: it walks ahead, turns now and then, and turns away
and jumps when it has not got anywhere for a while. Its input is a player's
(players.h's player_action): where it faces, how it moves, its buttons,
always within what player_control.c lets a person give (the aim's pitch
within 85 degrees, the yaw within a turn), since every machine takes it as
it takes a person's. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "units/units.h"

#include "bots_internal.h"

#include <math.h>

/* the ticks between a wanderer's turns, and between its looks at how far it
has come; how far it must have come; how fast it turns (radians a tick) */
#define BOT_WANDER_TURN_TICKS_MINIMUM 45
#define BOT_WANDER_TURN_TICKS_RANGE 75
#define BOT_WANDER_CHECK_TICKS 45
#define BOT_WANDER_PROGRESS 0.4f
#define BOT_TURN_RATE (_pi / 30.0f)

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

void bot_brain_reset(
	struct bot_brain *brain,
	unsigned long seed)
{
	csmemset(brain, 0, sizeof(*brain));
	brain->random = seed ? seed : 1;
	brain->unit_index = NONE;
}

void bot_brain_think(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct player_action *action)
{
	struct bot_brain *brain = &bot->brain;
	struct object_datum *object = object_get(unit_index);
	real_point3d position = object->object.position;
	real turn;

	/* (a new life: it faces where its unit spawned facing) */
	if (brain->unit_index != unit_index)
	{
		brain->unit_index = unit_index;
		brain->yaw = bot_yaw_normalize((real)atan2(object->object.forward.j, object->object.forward.i));
		brain->pitch = 0.0f;
		brain->target_yaw = brain->yaw;
		brain->turn_ticks = bot_random_range(brain, BOT_WANDER_TURN_TICKS_MINIMUM, BOT_WANDER_TURN_TICKS_RANGE);
		brain->check_position = position;
		brain->check_ticks = BOT_WANDER_CHECK_TICKS;
	}
	if (--brain->turn_ticks <= 0)
	{
		brain->target_yaw = bot_yaw_normalize(brain->yaw + (bot_random_real(brain) - 0.5f) * 2.0f * _pi / 3.0f);
		brain->turn_ticks = bot_random_range(brain, BOT_WANDER_TURN_TICKS_MINIMUM, BOT_WANDER_TURN_TICKS_RANGE);
	}
	/* (stuck: turned away, and a jump) */
	if (--brain->check_ticks <= 0)
	{
		real dx = position.x - brain->check_position.x;
		real dy = position.y - brain->check_position.y;

		if (dx * dx + dy * dy < BOT_WANDER_PROGRESS * BOT_WANDER_PROGRESS)
		{
			brain->target_yaw = bot_yaw_normalize(brain->yaw + _pi * (0.5f + bot_random_real(brain)));
			brain->jump = TRUE;
		}
		brain->check_position = position;
		brain->check_ticks = BOT_WANDER_CHECK_TICKS;
	}
	turn = bot_yaw_difference(brain->yaw, brain->target_yaw);
	brain->yaw = bot_yaw_normalize(brain->yaw + PIN(turn, -BOT_TURN_RATE, BOT_TURN_RATE));

	bot_action_idle(action, brain->yaw, brain->pitch);
	action->throttle.i = 1.0f;
	if (brain->jump)
	{
		/* (a press: held one tick, as a button is) */
		SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
		brain->jump = FALSE;
	}
	(void)player_index;
}
