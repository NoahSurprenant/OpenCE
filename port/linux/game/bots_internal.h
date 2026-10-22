/* bots_internal.h: what bots.c and the bots' brain (bots_brain.c) share */

#ifndef __BOTS_INTERNAL_H
#define __BOTS_INTERNAL_H

#include "bots.h"
#include "bots_nav.h"

struct unit_datum;

/* the most a bot looks up or down: the game's own limit for a player's
aim is 85.5 degrees (player_control.c's valid_euler_angles2d) */
#define BOT_MAXIMUM_PITCH_DEGREES 85.0f

/* how long a bot goes on to see to a noise it heard (ticks) */
#define BOT_INVESTIGATE_FORGET_TICKS (6 * TICKS_PER_SECOND)

/* the most edges a bot's path has (a longer way is gone along in parts) */
#define BOT_PATH_MAXIMUM 256

/* what a bot has done, for the log (bots.c) */
struct bot_statistics
{
	long goals_reached;
	long paths_planned;
	long paths_failed;
	long stuck;
	/* the ticks it spent getting nowhere on its way (each look at how far
	it had come that found it had not) */
	long stuck_ticks;
	/* the furthest it got from a spawn of its (world units) */
	real farthest;
	real travelled;
	/* the drops, jumps, teleporters and ladders it went by (bots_nav.h's edge types) */
	long moves[NUMBER_OF_BOTS_NAV_EDGE_TYPES];
	long targets_seen;
	long ticks_fired;
	long weapons_taken;
	/* the equipment it took (grenades, powerups) */
	long items_taken;
	/* its grenades (bots_grenades.c): thrown, and the players they killed:
	enemies, itself and team mates (bots_note_kill) */
	long grenades_thrown;
	long grenade_hits;
	long grenade_kills;
	long grenade_suicides;
	long team_kills;
	/* the noises it went to see (bots_hearing.c), and the grenades it ran
	from */
	long noises_heard;
	long grenades_dodged;
	/* its falling back to cover (bots_cover.c): begun, and cover reached;
	the powerups it took (overshield, camouflage, health); the times it
	changed weapons for the range (bots_items.c) */
	long retreats;
	long covers_reached;
	long powerups_taken;
	long weapon_switches;
	long power_weapons_taken;
	/* the times it killed itself, by any means */
	long suicides;
	/* the race flags it touched (bots_objectives.c) */
	long race_flags;
	/* the furthest it carried the enemy's flag from its stand (world units) */
	real longest_carry;
	/* on a board (bots_skate.c): the times it got on, the ticks it rode, how
	far (world units), its ollies, its bails, and the people it rode at */
	long skate_boardings;
	long skate_ticks;
	real skate_travelled;
	long skate_ollies;
	long skate_bails;
	long skate_rams;
	/* ... the rails it lined up with, and its grinds */
	long skate_grind_tries;
	long skate_grinds;
	/* the times it found itself in a vehicle it never meant to get into
	(bots_brain.c's bot_vehicle_keep_out) */
	long vehicle_mistakes;
	/* in vehicles (bots_vehicle.c): the times it drove one, the ticks it
	drove, how far (world units), its backing up when stuck, the vehicles it
	flipped back over, the people on foot it braked for, and the people it
	drove at (never yet: bots run nobody over on purpose) */
	long vehicle_rides;
	long vehicle_ticks;
	real vehicle_travelled;
	long vehicle_reverses;
	long vehicle_flips;
	long vehicle_braked;
	long vehicle_rams;
	/* ... in a gunner's seat: the times, the ticks, and the ticks it fired
	from a vehicle (a gun's seat, a Ghost); the enemies it killed riding, and
	those a vehicle it drove ran over (by accident or not) */
	long gunner_rides;
	long gunner_ticks;
	long vehicle_ticks_fired;
	long vehicle_kills;
	long run_overs;
	/* the ticks it drove with a person aboard */
	long person_ticks;
};

/* a bot and a vehicle (bots_vehicle.c) */
struct bot_vehicle
{
	/* the unit it is (another: a new life, afresh) */
	long unit_index;
	/* what it is about (bots_vehicle.c's phases), since when; when it next
	thinks of a ride; when it last pressed the action button */
	short phase;
	long phase_tick;
	long look_tick;
	long press_tick;
	/* the tick by which it is at the door, or gives the vehicle up */
	long approach_until_tick;
	/* the vehicle and the seat it takes (its driver's or its gunner's: the
	role); the vehicle it keeps off, until when */
	long vehicle_index;
	short seat_index;
	short role;
	long avoided_vehicle;
	long avoided_until_tick;
	/* the goal it rides toward (its walking brain's), the drive graph's node
	nearest it, and its way there along the drive graph (edges backwards,
	the next at path_next) */
	struct bots_nav_point goal;
	long drive_goal_node;
	long path[BOT_PATH_MAXIMUM];
	long path_next;
	long path_count;
	/* driving: where the vehicle was last tick, and at the last look at how
	far it had come (when); the throttle it last gave; braking for someone */
	real_point3d last_position;
	real_point3d check_position;
	long check_tick;
	real last_throttle;
	boolean braking;
	/* getting nowhere: backing up until when; the times in the window
	(since when), the edge it was on last time and how many times running */
	long reverse_until_tick;
	real reverse_side;
	long detour_until_tick;
	long stuck_events;
	long stuck_window_tick;
	long stuck_edge;
	long edge_stuck;
	/* the ticks the vehicle has been on its side or roof; whether it flips
	it after getting out, and its presses at one */
	long overturned_ticks;
	boolean flip_after;
	long flip_presses;
	/* a gunner: the ticks its vehicle has had no driver, and stood still
	with nobody to shoot at; a driver fighting: the way round its target it
	circles (1 or -1), and a Ghost's strafe side and until when */
	long no_driver_ticks;
	long parked_ticks;
	real orbit_side;
	real strafe_side;
	long strafe_until_tick;
	/* it drives at someone on foot (bots_vehicle.c's bot_vehicle_may_ram) */
	boolean ramming;
	/* a person rides with it; the ticks a person has stood at its door; it
	got out to give them the seat */
	boolean person_aboard;
	long door_waiting_ticks;
	boolean yielded;
};

/* a bot on a board (bots_skate.c) */
struct bot_skate
{
	/* the unit it rides as (another: a new life, afresh) */
	long unit_index;
	/* it wants to be on a board; it is (its skater got on); it rides at a
	person; it bails */
	boolean on;
	boolean riding;
	boolean ramming;
	boolean bailing;
	/* it grinds; it lines up with a rail, and the tick before which it tries
	no other */
	boolean grinding;
	boolean lining_up;
	long grind_after_tick;
	/* when it last got on or off, and the tick before which it stays off */
	long changed_tick;
	long off_until_tick;
	/* the ticks it got nowhere on the board, stalled pushing, has left of an
	ollie, and walked off the board */
	long stuck_ticks;
	long stall_ticks;
	long ollie_ticks;
	long walking_ticks;
};

/* a game type's objective for a bot (bots_objectives.c) */
enum
{
	_bot_objective_none,
	/* go to the point (a race's flag) */
	_bot_objective_go,
	/* go and take the item there (a flag, a ball) */
	_bot_objective_take,
	/* go there and stay within the radius (a hill, a flag's stand, a team
	mate to escort) */
	_bot_objective_hold,
	/* carry the flag there (home), or the ball away from enemies (flee) */
	_bot_objective_carry,
	/* hunt the player there (a carrier, the juggernaut) */
	_bot_objective_hunt,
	NUMBER_OF_BOT_OBJECTIVES
};

struct bot_objective
{
	short kind;
	boolean flee;
	struct bots_nav_point point;
	real radius;
	long object_index;
	long hunted_player;
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
	/* what it heard (bots_hearing.c): where it reckons the noise was (no
	better than an ear: off by more the further it was), when, whose (NONE:
	nobody's it knows), until when it turns to look, and whether it has yet
	to go and see */
	real_point3d heard_position;
	long heard_tick;
	long heard_player;
	long look_until_tick;
	boolean heard_new;
	/* a live grenade near it: where, and until when it runs from it */
	real_point3d danger_position;
	long danger_until_tick;
	/* its grenades (bots_grenades.c): the earliest it thinks of throwing
	again; a throw under way: its kind, the aim it holds, when it began and
	whether the button went down */
	long grenade_after_tick;
	boolean throwing;
	short throw_type;
	real throw_yaw, throw_pitch;
	long throw_tick;
	boolean throw_pressed;
	/* until when it backs away from its target (a plasma grenade thrown at
	it may be stuck to it) */
	long back_off_until_tick;
	/* until when it comes no nearer its target (its grenade goes off) */
	long hold_until_tick;
	/* (debug.bot_trace) its grenade in the air, where it last was, and where
	it was thrown at */
	long grenade_object;
	real_point3d grenade_last;
	real_point3d grenade_aimed;
	/* falling back (bots_cover.c): until when, the cover it goes to,
	whether it is there, and when it looks for cover next */
	long cover_until_tick;
	struct bots_nav_point cover_point;
	boolean in_cover;
	long cover_look_tick;
	/* the weapon it wants in hand for the range (a slot, NONE for none),
	and when it thinks of it next */
	short weapon_slot;
	long weapon_look_tick;
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
	/* the highest a ladder has taken it, and the ticks since it rose */
	real climb_high_z;
	long climb_still_ticks;
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
	/* where this life began */
	real_point3d spawn_position;
	struct bot_statistics statistics;
	struct bot_combat combat;
	/* a weapon it goes to take (NONE for none), until when it tries, and
	when it looks for one next */
	long pickup_object;
	long pickup_at_ticks;
	/* the tick before which it swaps no weapon of its own for one on the
	ground (one swap, then a while) */
	long pickup_swap_tick;
	boolean pickup_is_powerup;
	real_point3d pickup_position;
	long pickup_until_tick;
	long pickup_look_tick;
	/* the last weapon it gave up on (not tried again) */
	long pickup_given_up;
	/* the goal it was given fighting, and when */
	long combat_goal_node;
	long combat_goal_tick;
	/* standing its ground in a fight (last tick): not getting nowhere */
	boolean holding;
	/* its game type's objective (bots_objectives.c), looked at every few
	ticks; the point it last planned its way to for it; when it last
	picked a place to stand within a hold, or to flee to */
	struct bot_objective objective;
	struct bots_nav_point objective_planned;
	long objective_plan_tick;
	long objective_wander_tick;
	/* a race's next flag; Team Slayer's team mate to go over to, and when
	it chooses again */
	long race_flag;
	long buddy_player;
	long buddy_tick;
	/* its approach to the point it heads for (the step of its way, the
	nearest it came, the ticks since it came nearer: bot_near_enough) */
	long approach_step;
	real approach_best;
	long approach_ticks;
	/* stepping aside, getting nowhere: the ticks left, and to which side */
	long sidestep_ticks;
	real sidestep_side;
	/* how far it went last tick, and the ticks it has pushed on a walk
	while hardly moving (a lip the graph walks over: a hop) */
	real last_step;
	long stall_ticks;
	/* a long jump's run-up: the step of its way it is for, and whether done */
	long run_up_step;
	boolean run_up_done;
	/* on a board (bots_skate.c) */
	struct bot_skate skate;
	/* in a vehicle's seat it never meant to take, last tick */
	boolean in_vehicle;
	/* a vehicle it goes to, drives, gets out of or flips (bots_vehicle.c) */
	struct bot_vehicle vehicle;
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
/* (bots_skate.c, last in bot_brain_think) the bot on a board or off it, and
on one its pad, its action made what a skater does (no fire, no move) */
/* (bots_skate.c) whether the bot may skate here (the host has Skate 3 mode,
its data and a session to spare; the bot is not kept off boards) */
boolean bot_skate_possible(
	struct bot const *bot);
void bot_skate_think(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct player_action *action);
/* (bots_vehicle.c, in bot_brain_think before it fights or walks) the bot on
its way to a vehicle, in one, getting out or flipping one: TRUE when its
action is that (on its way along the walking graph it walks as the brain
has it, its goal the vehicle's door: FALSE); whether the bot has the vehicle
(goes to it, gets in, drives it, gets out, flips it); whether it is about
any vehicle; whether it drives one */
boolean bot_vehicle_think(
	struct bot *bot,
	long player_index,
	long unit_index,
	real_point3d const *position,
	struct player_action *action);
boolean bot_vehicle_holds(
	struct bot const *bot,
	long vehicle_index);
boolean bot_vehicle_busy(
	struct bot const *bot);
boolean bot_vehicle_riding(
	struct bot const *bot);
/* (bots_vehicle.c) whether the bot goes to the vehicle's gunner's seat, or
is getting in there */
boolean bot_vehicle_gunner_coming(
	struct bot const *bot,
	long vehicle_index);
/* (bots_vehicle.c) whether the bot drives the vehicle, or goes to (of the
team, or any team for NONE) */
boolean bot_vehicle_drives(
	struct bot const *bot,
	long vehicle_index,
	short team);
/* (bots_combat.c) the vehicle the unit rides (NONE: none) */
long bot_unit_vehicle(
	long unit_index);
/* (bots.c) whether a bot other than except has the vehicle's seat (NONE:
any of it); how many bots ride vehicles */
boolean bots_vehicle_taken(
	long vehicle_index,
	short seat_index,
	struct bot const *except);
/* (bots.c) whether a bot goes to the vehicle's gunner's seat (getting in) */
boolean bots_vehicle_gunner_coming(
	long vehicle_index);
/* (bots.c) whether a bot of the team drives the vehicle or goes to; whether
a bot drives it (or goes to) */
boolean bots_vehicle_team_mate_drives(
	long vehicle_index,
	short team);
boolean bots_vehicle_bot_drives(
	long vehicle_index);
long bots_vehicle_riders(
	void);
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
/* (bots_grenades.c) a throw under way, or one begun now, at the target or
where it was last seen: the aim and the button for the tick (TRUE: it
throws, and neither fires nor aims otherwise) */
boolean bot_grenade_act(
	struct bot *bot,
	long unit_index,
	struct player_action *action);
/* (bots_items.c) how much the bot wants an item lying about (0: not at all;
1: as much as a weapon it has none of), for bots_brain.c's pickups */
real bot_item_value(
	struct bot const *bot,
	long unit_index,
	long object_index);
/* (bots_grenades.c, debug.bot_trace) where the bot's grenade went off */
void bot_grenade_follow(
	struct bot *bot);
/* (bots_items.c) a weapon's worth (0: a flag or a ball); whether an item is
a powerup; the slot of the weapon a bot drops for the item (NONE: none, a
free hand or not a weapon); the slot of the weapon it fights with at the
distance (NONE: none with ammunition); whether the weapon's blast would hurt
its holder at the distance */
real bot_weapon_power(
	long weapon_index);
boolean bot_item_is_powerup(
	long object_index);
short bot_pickup_swap(
	long unit_index,
	long object_index);
short bot_weapon_choose(
	long unit_index,
	real distance);
boolean bot_weapon_blasts_near(
	long weapon_index,
	real distance);
/* (bots_cover.c) falling back to cover, or no longer, a fighting bot; the
goal it falls back to (FALSE: none) */
void bot_cover_update(
	struct bot *bot,
	long unit_index);
boolean bot_cover_goal(
	struct bot *bot,
	real_point3d const *position,
	struct bots_nav_point *goal);
/* (bots_combat.c) whether the unit has a flag or a ball in hand */
boolean bot_holds_flag(
	struct unit_datum *unit);
/* (bots_objectives.c) the bot's objective in the game type being played */
void bot_objective_update(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct bot_objective *objective);
/* (bots.c) the bot's place among the bots of its team (0 for the first),
and how many there are */
short bots_team_rank(
	struct bot const *bot,
	short *count);
/* (bots_grenades.c) how near a grenade of the kind going off hurts */
real bot_grenade_danger_radius(
	short type);
/* (bots_hearing.c) what the bot hears of its enemies (gunfire, grenades)
when it sees nobody; the live grenades near it (any bot, a peaceful one
too: running from a grenade is no fighting), and its moves away from one */
void bot_hear(
	struct bot *bot,
	long player_index,
	long unit_index);
void bot_sense_danger(
	struct bot *bot,
	long unit_index);
void bot_dodge(
	struct bot *bot,
	long unit_index,
	struct player_action *action);
/* (bots_combat.c) the bot's skill at its difficulty: how far it hears, how
often it throws a grenade, how well, and how long between throws (ticks) */
real bot_skill_hearing_range(
	struct bot const *bot);
real bot_skill_grenade_chance(
	struct bot const *bot);
real bot_skill_grenade_error(
	struct bot const *bot);
long bot_skill_grenade_cooldown(
	struct bot const *bot);
/* a normally distributed number (mean 0, spread 1) from the brain's own
random numbers */
real bot_gaussian(
	struct bot_brain *brain);
/* the player datum of a unit (NONE if no player's) */
long bot_unit_player(
	long unit_index);
/* an action that does nothing (no buttons, no weapon change) */
void bot_action_idle(
	struct player_action *action,
	real yaw,
	real pitch);
/* (bots_nav_game.c) the graph of the map being played (NULL: none), made
when needed is (bots are in the game) or debug.bot_nav_check asks; and the
graph its vehicles drive (NULL: none, or a map without vehicles), made with
it when drive is (bots may drive) or debug.bot_nav_check asks */
struct bots_nav *bots_nav_current(
	void);
struct bots_nav *bots_nav_drive_current(
	void);
/* (bots_nav_game.c) the graph the map's tanks drive (NULL: none) */
struct bots_nav *bots_nav_tank_current(
	void);
void bots_nav_update(
	boolean needed,
	boolean drive);
/* a number in [0, 1) from the brain's own random numbers */
real bot_random_real(
	struct bot_brain *brain);

#endif
