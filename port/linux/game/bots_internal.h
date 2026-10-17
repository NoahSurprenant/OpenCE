/* bots_internal.h: what bots.c and the bots' brain (bots_brain.c) share */

#ifndef __BOTS_INTERNAL_H
#define __BOTS_INTERNAL_H

#include "bots.h"
#include "bots_nav.h"

/* the most a bot looks up or down: the game's own limit for a player's
aim is 85.5 degrees (player_control.c's valid_euler_angles2d) */
#define BOT_MAXIMUM_PITCH_DEGREES 85.0f

/* the most edges a bot's path has (a longer way is gone along in parts) */
#define BOT_PATH_MAXIMUM 256

/* what a bot has done, for the log (bots.c) */
struct bot_statistics
{
	long goals_reached;
	long paths_planned;
	long paths_failed;
	long stuck;
	real travelled;
	/* the drops, jumps, teleporters and ladders it went by (bots_nav.h's edge types) */
	long moves[NUMBER_OF_BOTS_NAV_EDGE_TYPES];
};

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
	/* the way it goes (bots_nav_find_path's edges, the next at path_next,
	counting down), its goal, and whether it has begun the next edge's
	drop, jump or teleporter */
	long path[BOT_PATH_MAXIMUM];
	long path_next;
	long goal_node;
	boolean edge_begun;
	/* the ticks it holds crouch for, in a jump (crouching in the air lifts
	its feet onto a higher ledge) */
	long crouch_ticks;
	/* the ticks since it last got a step further along its way */
	long progress_ticks;
	long progress_step;
	/* the way it faces climbing a ladder (into it) */
	real climb_yaw;
	/* on a teleporter that does not take it (it came out on that pad: the
	game sends nobody back until they have stepped off it): the ticks it
	has waited, and whether it steps off first */
	long teleport_wait;
	boolean step_off;
	/* the ticks since it began a jump */
	long jump_ticks;
	/* the plans it made this second */
	long plan_second;
	long plans_this_second;
	/* getting nowhere: how many times in a row, and an edge to keep off */
	long stuck_count;
	long avoided_edge;
	long avoided_ticks;
	real_point3d last_position;
	struct bot_statistics statistics;
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
/* (bots_nav_game.c) the graph of the map being played (NULL: none), made
when needed is (bots are in the game) or debug.bot_nav_check asks */
struct bots_nav *bots_nav_current(
	void);
void bots_nav_update(
	boolean needed);
/* a number in [0, 1) from the brain's own random numbers */
real bot_random_real(
	struct bot_brain *brain);

#endif
