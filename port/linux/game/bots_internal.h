/* bots_internal.h: what bots.c and the bots' brain (bots_brain.c) share */

#ifndef __BOTS_INTERNAL_H
#define __BOTS_INTERNAL_H

#include "bots.h"

/* the most a bot looks up or down: the game's own limit for a player's
aim is 85.5 degrees (player_control.c's valid_euler_angles2d) */
#define BOT_MAXIMUM_PITCH_DEGREES 85.0f

/* a bot's thinking, from one tick to the next */
struct bot_brain
{
	unsigned long random;
	/* the unit it last drove: another is a new life */
	long unit_index;
	/* the facing it last gave, and the one it turns to */
	real yaw;
	real pitch;
	real target_yaw;
	/* wandering: the ticks to the next turn, and where it was when it last
	looked at how far it had come */
	long turn_ticks;
	real_point3d check_position;
	long check_ticks;
	boolean jump;
};

/* a bot of the host's (bots.c) */
struct bot
{
	boolean used;
	long machine_index;
	long controller_index;
	short difficulty;
	struct bot_brain brain;
};

/* (bots_brain.c) a brain afresh, with its own random numbers */
void bot_brain_reset(
	struct bot_brain *brain,
	unsigned long seed);
/* (bots_brain.c) the bot's input for this tick, its player and unit
alive: an action fit for update_server_take_bot_action */
void bot_brain_think(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct player_action *action);
/* an action that does nothing (no buttons, no weapon change) */
void bot_action_idle(
	struct player_action *action,
	real yaw,
	real pitch);
/* a number in [0, 1) from the brain's own random numbers */
real bot_random_real(
	struct bot_brain *brain);

#endif
