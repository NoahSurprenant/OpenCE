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
	long targets_seen;
	long ticks_fired;
	long weapons_taken;
};

/* a bot's fighting (bots_combat.c) */
struct bot_combat
{
	/* its target (a player datum, NONE for none), whether it sees it now,
	and where and when it last saw it */
	long target_player;
	boolean target_visible;
	long last_seen_tick;
	real_point3d last_seen_position;
	/* the tick from which it fires (its reaction), its bursts, its strikes
	and reloads */
	long fire_after_tick;
	long burst_ticks;
	long melee_after_tick;
	long reload_after_tick;
	/* its aim, and the aim's error */
	boolean aim_set;
	real aim_yaw, aim_pitch;
	real error_yaw, error_pitch, error_scale;
	/* its strafing */
	real strafe_direction;
	long strafe_until_tick;
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
	struct bot_combat combat;
	/* a weapon it goes to take (NONE for none), until when it tries, and
	when it looks for one next */
	long pickup_object;
	long pickup_until_tick;
	long pickup_look_tick;
	/* the last weapon it gave up on (not tried again) */
	long pickup_given_up;
	/* the goal it was given fighting, and when */
	long combat_goal_node;
	long combat_goal_tick;
};

/* a bot of the host's (bots.c) */
struct bot
{
	boolean used;
	long machine_index;
	long controller_index;
	short difficulty;
	/* its index among the bots (the ticks it looks around on) */
	short slot;
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
/* (bots_combat.c) a fight afresh; whether the bot fights now (it looks
around every few ticks); the goal it heads for fighting (NULL: it holds its
ground); its aim, fire and moves fighting (moving: the walking set its
moves), or its reloading when it fights nobody */
void bot_combat_reset(
	struct bot_combat *combat);
boolean bot_combat_engaged(
	struct bot *bot,
	long player_index,
	long unit_index,
	long bot_slot);
struct bots_nav_point const *bot_combat_goal(
	struct bot *bot,
	real_point3d const *position,
	struct bots_nav_point *goal);
void bot_combat_act(
	struct bot *bot,
	long unit_index,
	struct player_action *action,
	boolean moving);
void bot_combat_idle(
	struct bot *bot,
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
