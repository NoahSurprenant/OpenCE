/* bots_vehicle.c: bots that drive vehicles (bots_internal.h;
port/linux/README.md, "Bots in vehicles")

A bot drives a Warthog or a Ghost to travel, as its driver, on a map whose
vehicles have a drive graph (bots_nav_game.c's, bots_nav_vehicle_parameters).
Everything goes through the bot's input, as a person's does: the action
button gets it in, out, and flips a vehicle over; its facing steers the
vehicle (units.c copies the driver's facing and throttle into the vehicle,
vehicles.c steers toward it) and its throttle drives it on or back. So every
machine sees a bot drive as it sees anyone drive, and nothing is new on the
wire.

- It thinks of a ride a few times a second, when its brain's goal is far
  (40 world units, about 120 m), no fight is on, it holds no flag or ball:
  the nearest free Warthog or Ghost within 25 world units that no other bot
  has, that nobody rides, that no person stands by (a person may want it),
  on the drive graph's main part, if riding (walking there, driving at about
  three times walking speed, walking on from where the drive graph ends) is
  quicker than walking.
- It walks to the driver's door along the walking graph, stops (the game
  takes no one in moving), and presses the action button while the game
  offers it the driver's seat.
- It drives the drive graph to the node nearest its goal: it steers for a
  point a few world units ahead along the way (further the faster), slows
  for turns and near its goal, holds off the throttle in the air, brakes for
  anyone on foot ahead (bots, people, skaters: running someone over is an
  accident, never a bot's aim) and slows for vehicles ahead.
- Getting nowhere, it backs up a second with the wheels turned the other way
  and drives on; stuck three times on one part of the way it keeps off that
  part, and six times in 20 seconds it gets out and walks.
- It gets out at its goal (stopped first), when the vehicle has turned over,
  or stuck; then walks on to its goal. A vehicle on its side or roof it
  flips back over (the action button beside it, as a person does) and gets
  back in.
- A bot in a vehicle it never chose (one put there) gets out at once
  (bots_brain.c's bot_vehicle_keep_out).

server.bot_vehicles limits how many bots ride at once (0 any number, -1
none); debug.bot_vehicle sets each bot's way for the tests. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "objects/objects.h"
#include "tag_files/tag_files.h"
#include "units/units.h"
#include "units/unit_definitions.h"

#include "bots_internal.h"

#include <math.h>
#include <string.h>

void platform_log(const char *format, ...);
const char *config_string(const char *name);
long config_integer(const char *name);
int config_boolean(const char *name);

/* how a bot goes about vehicles (debug.bot_vehicle, for the tests) */
enum
{
	_bot_vehicle_normal,
	/* it rides whenever its goal is a little way off and a vehicle is near */
	_bot_vehicle_drive,
	/* it never gets into one */
	_bot_vehicle_never,
	/* it stands still on foot (something for a driver to brake for) */
	_bot_vehicle_stand,
	/* it takes only a gunner's seat, whenever a team mate drives near */
	_bot_vehicle_gun,
	/* it drives only a Scorpion, whenever it can */
	_bot_vehicle_tank
};

/* the seat a bot takes */
enum
{
	_bot_vehicle_role_driver,
	_bot_vehicle_role_gunner
};

/* what a bot is about with a vehicle */
enum
{
	_bot_vehicle_phase_none,
	/* walking to the driver's door */
	_bot_vehicle_phase_approach,
	/* at the door, getting in */
	_bot_vehicle_phase_board,
	/* driving */
	_bot_vehicle_phase_ride,
	/* getting out */
	_bot_vehicle_phase_leave,
	/* flipping the vehicle back over */
	_bot_vehicle_phase_flip
};

/* the furthest its goal must be for a ride (world units: 40, about 120 m;
for a bot that drives whenever it can, 10), the furthest a vehicle may be
from it (for one that drives whenever it can, 60), and how much quicker than walking a ride must be (a fraction) */
#define BOT_VEHICLE_TRIP 40.0f
#define BOT_VEHICLE_DRIVER_TRIP 10.0f
#define BOT_VEHICLE_REACH 25.0f
#define BOT_VEHICLE_DRIVER_REACH 60.0f
/* ... in a fight (a Ghost to fight from), and a team mate's vehicle whose gun
it takes; the fastest a vehicle whose driver is a person may go for it to
get in (world units a second) */
#define BOT_VEHICLE_FIGHT_REACH 15.0f
#define BOT_VEHICLE_GUNNER_REACH 15.0f
#define BOT_VEHICLE_PERSON_GUNNER_REACH 25.0f
#define BOT_VEHICLE_GUNNER_BOARD_SPEED 1.0f
/* a driver waits this long for a team mate going for its gun */
#define BOT_VEHICLE_CREW_WAIT_TICKS (6 * TICKS_PER_SECOND)
/* a crew that fights from its vehicle drives to a fight it knows of (a
target seen, a noise heard) this lately (ticks), when it is this far from
where it drives (world units) */
#define BOT_VEHICLE_FIGHT_MEMORY_TICKS (8 * TICKS_PER_SECOND)
#define BOT_VEHICLE_FIGHT_REGOAL 6.0f
/* a driver with a gunner aboard drives on to a place at least this far off
when it gets to its goal */
#define BOT_VEHICLE_ROAM 30.0f
/* running people over (bot_vehicle_may_ram): a normal bot's target ahead
within this range and angle (radians), a hard one's within this range; none
higher or lower than this (world units) */
#define BOT_VEHICLE_RAM_RANGE 12.0f
#define BOT_VEHICLE_RAM_CONE 0.35f
#define BOT_VEHICLE_HUNT_RANGE 25.0f
#define BOT_VEHICLE_RAM_HEIGHT 2.0f
/* fighting: a Warthog's driver alone gets out for an enemy this near; one
with a gunner stops this near (or circles at this range, this far round a
step); a Ghost closes past far and backs off inside near */
#define BOT_VEHICLE_FIGHT_ON_FOOT 15.0f
#define BOT_VEHICLE_STANDOFF 15.0f
#define BOT_VEHICLE_ORBIT_RANGE 14.0f
#define BOT_VEHICLE_ORBIT_STEP 0.7f
#define BOT_VEHICLE_GHOST_FAR 14.0f
#define BOT_VEHICLE_GHOST_NEAR 7.0f
/* ... and a Scorpion (its cannon's blast kept off itself) */
#define BOT_VEHICLE_TANK_FAR 25.0f
#define BOT_VEHICLE_TANK_NEAR 10.0f
/* a gunner gets out after this long with no driver, or standing still with
a person driving and nobody to shoot at */
#define BOT_VEHICLE_NO_DRIVER_TICKS (3 * TICKS_PER_SECOND)
#define BOT_VEHICLE_PARKED_TICKS (20 * TICKS_PER_SECOND)
#define BOT_VEHICLE_QUICKER 0.75f
/* a vehicle drives about this many times as fast as a bot walks, and getting
in and out costs about this much (world units of walking) */
#define BOT_VEHICLE_SPEED_FACTOR 3.0f
#define BOT_VEHICLE_OVERHEAD 6.0f
/* a board goes about this many times as fast as a bot walks (9 m/s against
a run's 6.8: bots_skate.c's fast speed), its getting on and off reckoned in */
#define BOT_VEHICLE_SKATE_FACTOR 1.3f
/* nobody (a person) this near a vehicle: they may want it; nor this near
and coming to it faster than this (world units a second) */
#define BOT_VEHICLE_COURTESY 6.0f
#define BOT_VEHICLE_COURTESY_FAR 15.0f
#define BOT_VEHICLE_COURTESY_CLOSING 0.5f
/* a person this near its driver's door this long, the vehicle slow: a bot
gives them the seat */
#define BOT_VEHICLE_DOOR_WAITING 1.5f
#define BOT_VEHICLE_DOOR_WAITING_TICKS (3 * TICKS_PER_SECOND / 2)
/* the ticks between its thoughts of a ride, and after giving one up */
#define BOT_VEHICLE_LOOK_TICKS (TICKS_PER_SECOND / 2)
#define BOT_VEHICLE_AVOID_TICKS (20 * TICKS_PER_SECOND)
/* walking to a door: how near it walks along the walking graph (then
straight on), how near it stands; how long it may take (ticks, and more a
world unit), how long it tries at the door */
#define BOT_VEHICLE_DOOR_NEAR 4.0f
#define BOT_VEHICLE_DOOR_STAND 0.6f
#define BOT_VEHICLE_APPROACH_TICKS (10 * TICKS_PER_SECOND)
#define BOT_VEHICLE_APPROACH_TICKS_PER_UNIT TICKS_PER_SECOND
#define BOT_VEHICLE_BOARD_TICKS (6 * TICKS_PER_SECOND)
/* how far outside a vehicle's door it steps first (world units) */
#define BOT_VEHICLE_DOOR_OUTSIDE 1.0f
/* ... and how far off the line out through the door counts as in front of it */
#define BOT_VEHICLE_DOOR_ACROSS 0.5f
/* the ticks between presses of the action button (one press is a press) */
#define BOT_VEHICLE_PRESS_TICKS 10
/* driving: a node this near is passed; the point it steers for, at least
this far ahead and further the faster (seconds of its speed), at most this
far; how near its goal it gets out */
#define BOT_VEHICLE_NODE_PASSED 2.5f
/* how many nodes ahead along its way it looks for the nearest (one passed
wide is passed) */
#define BOT_VEHICLE_SKIP_NODES 10
#define BOT_VEHICLE_LOOKAHEAD 3.0f
#define BOT_VEHICLE_LOOKAHEAD_SECONDS 0.6f
#define BOT_VEHICLE_LOOKAHEAD_MAXIMUM 10.0f
#define BOT_VEHICLE_ARRIVED 4.0f
#define BOT_VEHICLE_SLOW_NEAR 8.0f
/* the most it turns its facing in a tick (radians) */
#define BOT_VEHICLE_TURN_RATE (_pi / 10.0f)
/* getting nowhere: how often it looks (ticks) and how far it must have
come; how long it backs up; how many times on a part of the way before it
keeps off it, and in the window before it gets out */
#define BOT_VEHICLE_CHECK_TICKS 45
#define BOT_VEHICLE_PROGRESS 0.6f
#define BOT_VEHICLE_REVERSE_TICKS 36
/* ... facing this far to a side (radians), then on straight this long */
#define BOT_VEHICLE_REVERSE_TURN 1.0f
#define BOT_VEHICLE_DETOUR_TICKS 25
#define BOT_VEHICLE_EDGE_STUCK 3
#define BOT_VEHICLE_STUCK_LIMIT 6
#define BOT_VEHICLE_STUCK_WINDOW (20 * TICKS_PER_SECOND)
/* overturned: a vehicle whose up is lower than this, this many ticks */
#define BOT_VEHICLE_OVERTURNED 0.35f
/* leaning: eased off */
#define BOT_VEHICLE_LEANING 0.9f
#define BOT_VEHICLE_OVERTURNED_TICKS 30
/* ... and upright again (and enterable) */
#define BOT_VEHICLE_UPRIGHT 0.7f
/* flipping one: how near it walks along the graph, how long it tries */
#define BOT_VEHICLE_FLIP_NEAR 2.5f
#define BOT_VEHICLE_FLIP_TICKS (8 * TICKS_PER_SECOND)
/* getting out: the most it tries */
#define BOT_VEHICLE_LEAVE_TICKS (5 * TICKS_PER_SECOND)
/* braking for someone on foot ahead: how wide the way ahead is (world units
either side), how far ahead (and seconds of its speed more) */
#define BOT_VEHICLE_CORRIDOR 1.0f
#define BOT_VEHICLE_AHEAD 1.5f
#define BOT_VEHICLE_AHEAD_SECONDS 1.2f

/* ---------- small things */

static real bot_vehicle_angle(
	real angle)
{
	while (angle > _pi)
		angle -= 2.0f * _pi;
	while (angle < -_pi)
		angle += 2.0f * _pi;
	return angle;
}

static real bot_vehicle_distance2d(
	real_point3d const *a,
	real x,
	real y)
{
	real dx = a->x - x, dy = a->y - y;

	return (real)sqrt(dx * dx + dy * dy);
}

/* the bot's way with vehicles: its entry of debug.bot_vehicle ("drive,never":
one each, in the order they join; "*drive": every bot) */
static short bot_vehicle_mode(
	struct bot const *bot)
{
	char const *list = config_string("debug.bot_vehicle");
	char name[16];
	short index = 0;
	boolean every = list && *list == '*';

	if (every)
		list++;
	while (list && *list)
	{
		short length = 0;

		while (*list == ',' || *list == ' ')
			list++;
		while (*list && *list != ',' && length < (short)sizeof(name) - 1)
			name[length++] = *list++;
		name[length] = 0;
		if (length > 0 && (every || index++ == bot->slot))
		{
			if (!strcmp(name, "drive"))
				return _bot_vehicle_drive;
			if (!strcmp(name, "never"))
				return _bot_vehicle_never;
			if (!strcmp(name, "stand"))
				return _bot_vehicle_stand;
			if (!strcmp(name, "gun"))
				return _bot_vehicle_gun;
			if (!strcmp(name, "tank"))
				return _bot_vehicle_tank;
			return _bot_vehicle_normal;
		}
	}
	return _bot_vehicle_normal;
}

boolean bot_vehicle_gunner_escort(
	struct bot const *bot,
	long unit_index,
	real_point3d *at)
{
	struct data_iterator iterator;
	struct player_datum *other;
	short team = object_get(unit_index)->object.owner_team_index;

	if (bot_vehicle_mode(bot) != _bot_vehicle_gun || !game_engine_has_teams())
		return FALSE;
	data_iterator_new(&iterator, player_data);
	while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct bot const *mate = other->unit_index != NONE ? bots_bot_of_unit(other->unit_index) : NULL;
		struct object_datum *object;

		if (!mate || mate == bot || bot_vehicle_mode(mate) != _bot_vehicle_drive)
			continue;
		object = object_get(other->unit_index);
		if (TEST_FLAG(object->object.damage_flags, _object_dead_bit) || object->object.parent_object_index != NONE ||
			game_team_is_enemy(team, object->object.owner_team_index))
		{
			continue;
		}
		*at = object->object.position;
		return TRUE;
	}
	return FALSE;
}

/* the vehicles bots drive: a Warthog, a rocket Warthog, a Ghost or a
Scorpion, by its tag's name, as game_engine.c's vehicle types fall back to
(NULL: none of them: a Banshee, a turret) */
static char const *bot_vehicle_kind(
	long vehicle_index)
{
	char const *name = tag_get_name(object_get(vehicle_index)->definition_index);

	if (!name)
		return NULL;
	if (strstr(name, "rwarthog") || (strstr(name, "rocket") && strstr(name, "warthog")))
		return "rocket warthog";
	if (strstr(name, "warthog"))
		return "warthog";
	if (strstr(name, "ghost"))
		return "ghost";
	if (strstr(name, "scorpion"))
		return "scorpion";
	return NULL;
}

/* whether the vehicle is a tank (a Scorpion: it drives the tanks' graph) */
static boolean bot_vehicle_is_tank(
	long vehicle_index)
{
	char const *kind = bot_vehicle_kind(vehicle_index);

	return kind && !strcmp(kind, "scorpion");
}

/* the graph the vehicle drives (NULL: none on this map) */
static struct bots_nav *bot_vehicle_graph(
	long vehicle_index)
{
	return bot_vehicle_is_tank(vehicle_index) ? bots_nav_tank_current() : bots_nav_drive_current();
}

/* the vehicle's driver's seat (NONE: none) */
static short bot_vehicle_driver_seat(
	long vehicle_index)
{
	short count = unit_definition_get(object_get(vehicle_index)->definition_index)->unit.seats.count;
	short seat;

	for (seat = 0; seat < count; seat++)
	{
		if (unit_seat_is_driver(vehicle_index, seat))
			return seat;
	}
	return NONE;
}

/* the vehicle's gunner's seat that is no driver's (a Warthog's chaingun;
NONE: none, a Ghost's driver is its gunner) */
static short bot_vehicle_gunner_seat(
	long vehicle_index)
{
	short count = unit_definition_get(object_get(vehicle_index)->definition_index)->unit.seats.count;
	short seat;

	for (seat = 0; seat < count; seat++)
	{
		if (unit_seat_is_gunner(vehicle_index, seat) && !unit_seat_is_driver(vehicle_index, seat))
			return seat;
	}
	return NONE;
}

/* who sits in the vehicle's seat (NONE: nobody) */
static long bot_vehicle_seat_rider(
	long vehicle_index,
	short seat_index)
{
	long child = object_get(vehicle_index)->object.first_child_object_index;

	while (child != NONE)
	{
		struct object_datum *object = object_get(child);

		if (TEST_FLAG(_object_mask_unit, object->object.type) && unit_get(child)->unit.parent_seat_index == seat_index)
			return child;
		child = object->object.next_object_index;
	}
	return NONE;
}

/* someone aboard the vehicle a bot (its unit) would drive: in its driver's
seat, or in any seat but a team mate (a team mate at the gun waits for a
driver: the bot drives on with them, whichever of the two got there first) */
static boolean bot_vehicle_ridden_for_driver(
	long vehicle_index,
	long unit_index)
{
	long child = object_get(vehicle_index)->object.first_child_object_index;
	short driver_seat = bot_vehicle_driver_seat(vehicle_index);
	short team = object_get(unit_index)->object.owner_team_index;

	while (child != NONE)
	{
		struct object_datum *object = object_get(child);

		if (TEST_FLAG(_object_mask_unit, object->object.type) && child != unit_index &&
			(unit_get(child)->unit.parent_seat_index == driver_seat || !game_engine_has_teams() ||
			game_team_is_enemy(team, object->object.owner_team_index)))
		{
			return TRUE;
		}
		child = object->object.next_object_index;
	}
	return FALSE;
}

/* a vehicle still there and not destroyed */
static boolean bot_vehicle_alive(
	long vehicle_index)
{
	struct object_datum *object = vehicle_index != NONE ? object_try_and_get(vehicle_index) : NULL;

	return object && object->object.type == _object_type_vehicle && !TEST_FLAG(object->object.damage_flags, _object_dead_bit);
}

/* whether a person (not a bot) on foot or riding is this near the point */
static boolean bot_vehicle_person_near(
	real_point3d const *point,
	real distance)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		real_point3d position;

		if (player->unit_index == NONE || bots_player_is_bot(player))
			continue;
		object_get_origin(player->unit_index, &position);
		if (distance_squared3d(&position, point) < distance * distance)
			return TRUE;
		/* (further, but coming to it on foot: they may want it too) */
		if (distance_squared3d(&position, point) < BOT_VEHICLE_COURTESY_FAR * BOT_VEHICLE_COURTESY_FAR &&
			object_get(player->unit_index)->object.parent_object_index == NONE)
		{
			real_vector3d const *velocity = &object_get(player->unit_index)->object.translational_velocity;
			real dx = point->x - position.x, dy = point->y - position.y;
			real length = (real)sqrt(dx * dx + dy * dy);

			if (length > 0.01f && (velocity->i * dx + velocity->j * dy) / length * TICKS_PER_SECOND > BOT_VEHICLE_COURTESY_CLOSING)
				return TRUE;
		}
	}
	return FALSE;
}

/* a person (not a bot) who rides the vehicle (NONE: none) */
static long bot_vehicle_person_aboard(
	long vehicle_index)
{
	long child = object_get(vehicle_index)->object.first_child_object_index;

	while (child != NONE)
	{
		struct object_datum *object = object_get(child);

		if (TEST_FLAG(_object_mask_unit, object->object.type) && unit_get(child)->unit.player_index != NONE &&
			!bots_player_is_bot(player_get(unit_get(child)->unit.player_index)))
		{
			return unit_get(child)->unit.player_index;
		}
		child = object->object.next_object_index;
	}
	return NONE;
}

/* a person (not a bot) on foot standing at the vehicle's driver's door, of
the bot's team in a team game (anyone, else) */
static boolean bot_vehicle_person_at_door(
	long unit_index,
	long vehicle_index,
	short seat_index)
{
	struct data_iterator iterator;
	struct player_datum *player;
	real_point3d door, seat;

	if (!unit_get_seat_entrance_point(unit_index, vehicle_index, seat_index, &door, &seat, NULL))
		return FALSE;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct object_datum *object;

		if (player->unit_index == NONE || bots_player_is_bot(player))
			continue;
		object = object_get(player->unit_index);
		if (object->object.parent_object_index != NONE || TEST_FLAG(object->object.damage_flags, _object_dead_bit) ||
			(game_engine_has_teams() && game_team_is_enemy(object_get(unit_index)->object.owner_team_index,
			object->object.owner_team_index)))
		{
			continue;
		}
		if (distance_squared3d(&object->object.position, &door) < BOT_VEHICLE_DOOR_WAITING * BOT_VEHICLE_DOOR_WAITING)
			return TRUE;
	}
	return FALSE;
}

/* the drive graph's node of the main part nearest the point (NONE: none) */
static long bot_vehicle_nearest_node(
	struct bots_nav const *drive,
	real x,
	real y,
	real z)
{
	long best = NONE;
	real best_distance = 0.0f;
	long node;

	for (node = 0; node < drive->node_count; node++)
	{
		struct bots_nav_node const *here = &drive->nodes[node];
		real dx, dy, dz, distance;

		if (here->reach != (_bots_nav_reached_bit | _bots_nav_reaches_bit))
			continue;
		dx = here->point.x - x;
		dy = here->point.y - y;
		dz = here->point.z - z;
		distance = dx * dx + dy * dy + dz * dz * 4.0f;
		if (best == NONE || distance < best_distance)
		{
			best = node;
			best_distance = distance;
		}
	}
	return best;
}

/* the throttle it drives at, at most (a fraction of the vehicle's own) */
static real bot_vehicle_throttle_cap(
	struct bot const *bot)
{
	switch (bot->difficulty)
	{
	case _bot_difficulty_peaceful:
	case _bot_difficulty_easy:
		return 0.55f;
	case _bot_difficulty_normal:
		return 0.65f;
	case _bot_difficulty_hard:
		return 0.75f;
	default:
		return 0.85f;
	}
}

/* ---------- phases */

static void bot_vehicle_set_phase(
	struct bot_vehicle *vehicle,
	short phase)
{
	vehicle->phase = phase;
	vehicle->phase_tick = game_time_get();
	vehicle->press_tick = game_time_get() - BOT_VEHICLE_PRESS_TICKS;
}

/* the vehicle given up: kept off a while, and on foot */
static void bot_vehicle_give_up(
	struct bot_vehicle *vehicle,
	long ticks,
	char const *reason)
{
	if (config_boolean("debug.bot_trace"))
		platform_log("bots: trace vehicle %lx given up (%s), phase %d", vehicle->vehicle_index, reason, vehicle->phase);
	vehicle->avoided_vehicle = vehicle->vehicle_index;
	vehicle->avoided_until_tick = game_time_get() + ticks;
	vehicle->vehicle_index = NONE;
	bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_none);
}

/* on foot again: the walking brain heads for the goal the ride was for */
static void bot_vehicle_walk_on(
	struct bot_brain *brain)
{
	struct bots_nav *nav = bots_nav_current();

	brain->path_next = NONE;
	brain->stuck_count = 0;
	if (nav)
	{
		long node = bots_nav_locate(nav, brain->vehicle.goal.x, brain->vehicle.goal.y, brain->vehicle.goal.z + 0.5f);

		if (node != NONE)
			brain->goal_node = node;
	}
}

/* the walking brain heads for the point: its goal the walking graph's node
there (FALSE: no node there) */
static boolean bot_vehicle_head_for(
	struct bot_brain *brain,
	real_point3d const *point)
{
	struct bots_nav *nav = bots_nav_current();
	long node = nav ? bots_nav_locate(nav, point->x, point->y, point->z + 0.5f) : NONE;

	if (node == NONE)
		return FALSE;
	if (node != brain->goal_node)
	{
		brain->goal_node = node;
		brain->path_next = NONE;
	}
	/* (no weapon on the way to fetch first) */
	brain->pickup_object = NONE;
	brain->pickup_look_tick = game_time_get() + TICKS_PER_SECOND;
	return TRUE;
}

/* facing the point, standing still, or stepping toward it */
static void bot_vehicle_step_to(
	struct bot_brain *brain,
	real_point3d const *position,
	real_point3d const *point,
	real throttle,
	struct player_action *action)
{
	real yaw = (real)atan2(point->y - position->y, point->x - position->x);
	real turn = bot_vehicle_angle(yaw - brain->yaw);

	if (turn > BOT_VEHICLE_TURN_RATE)
		turn = BOT_VEHICLE_TURN_RATE;
	else if (turn < -BOT_VEHICLE_TURN_RATE)
		turn = -BOT_VEHICLE_TURN_RATE;
	brain->yaw = bot_vehicle_angle(brain->yaw + turn);
	brain->pitch = 0.0f;
	bot_action_idle(action, brain->yaw, brain->pitch);
	/* (forward along where it means to go, whichever way it faces yet) */
	if (throttle > 0.0f)
	{
		real off = bot_vehicle_angle(yaw - brain->yaw);

		action->throttle.i = (real)cos(off) * throttle;
		action->throttle.j = (real)sin(off) * throttle;
	}
}

/* where a bot near a vehicle steps for its door: the door from outside it
(a point a world unit out from the vehicle through the door), round the
vehicle's body when it stands on the other side; the door itself from there */
static void bot_vehicle_door_way(
	long vehicle_index,
	real_point3d const *position,
	real_point3d const *door,
	real_point3d *target)
{
	struct object_datum *vehicle = object_get(vehicle_index);
	real_point3d const *center = &vehicle->object.position;
	real radius = vehicle->object.bounding_sphere_radius * 0.8f;
	real ox = door->x - center->x, oy = door->y - center->y;
	real length = (real)sqrt(ox * ox + oy * oy);
	real_point3d outside;
	real sx, sy, dx, dy, t, cx, cy;

	*target = *door;
	if (length < 0.01f)
		return;
	ox /= length;
	oy /= length;
	outside = *door;
	outside.x += ox * BOT_VEHICLE_DOOR_OUTSIDE;
	outside.y += oy * BOT_VEHICLE_DOOR_OUTSIDE;
	/* (at the door's outside, or straight out in front of the door: in) */
	if (bot_vehicle_distance2d(position, outside.x, outside.y) < 0.5f ||
		((position->x - door->x) * ox + (position->y - door->y) * oy > 0.2f &&
		fabs((position->x - door->x) * -oy + (position->y - door->y) * ox) < BOT_VEHICLE_DOOR_ACROSS))
	{
		return;
	}
	*target = outside;
	/* (the way to the outside crosses the vehicle: round its side, the
	bot's side of the line from its middle out through the door) */
	sx = position->x;
	sy = position->y;
	dx = outside.x - sx;
	dy = outside.y - sy;
	t = dx * dx + dy * dy > 0.0001f ? ((center->x - sx) * dx + (center->y - sy) * dy) / (dx * dx + dy * dy) : 0.0f;
	if (t < 0.0f)
		t = 0.0f;
	else if (t > 1.0f)
		t = 1.0f;
	cx = sx + dx * t - center->x;
	cy = sy + dy * t - center->y;
	if (cx * cx + cy * cy < radius * radius)
	{
		real side = (position->x - center->x) * -oy + (position->y - center->y) * ox >= 0.0f ? 1.0f : -1.0f;

		target->x = center->x - oy * side * (radius + 0.6f);
		target->y = center->y + ox * side * (radius + 0.6f);
		target->z = position->z;
	}
}

/* the vehicle's driver is the bot's team mate: one who sits in its
driver's seat, or a bot that goes to it */
static boolean bot_vehicle_team_driver(
	long unit_index,
	long vehicle_index)
{
	short seat = bot_vehicle_driver_seat(vehicle_index);
	long driver = seat != NONE ? bot_vehicle_seat_rider(vehicle_index, seat) : NONE;
	short team = object_get(unit_index)->object.owner_team_index;

	if (!game_engine_has_teams() || seat == NONE)
		return FALSE;
	if (driver != NONE)
		return !game_team_is_enemy(team, object_get(driver)->object.owner_team_index);
	return bots_vehicle_team_mate_drives(vehicle_index, team);
}

/* a gunner's seat chosen: a Warthog near that a team mate drives (or a bot
goes to drive), its gun free and nobody else going for it, slow (a driver
who is a bot waits for its gunner), on the walking graph's reach */
static boolean bot_vehicle_choose_gun(
	struct bot *bot,
	long unit_index,
	real_point3d const *position,
	short mode,
	boolean people_only,
	struct bots_nav_point const *goal)
{
	struct bot_vehicle *vehicle = &bot->brain.vehicle;
	struct object_iterator iterator;
	long best = NONE;
	short best_seat = NONE;
	real best_distance = 0.0f;
	real reach = mode == _bot_vehicle_gun ? BOT_VEHICLE_DRIVER_REACH : BOT_VEHICLE_GUNNER_REACH;

	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *object = object_get(iterator.index);
		real distance = (real)sqrt(distance_squared3d(position, &object->object.position));
		real speed = (real)sqrt(magnitude_squared3d(&object->object.translational_velocity)) * TICKS_PER_SECOND;
		short seat;

		/* (a person driving: from further, and a vehicle it gave up on before
		is another matter now) */
		boolean person = bot_vehicle_alive(iterator.index) && bot_vehicle_person_aboard(iterator.index) != NONE;

		if (distance > (person ? BOT_VEHICLE_PERSON_GUNNER_REACH : reach) || (best != NONE && distance >= best_distance) ||
			!bot_vehicle_alive(iterator.index) || !bot_vehicle_kind(iterator.index) || object->object.up.k < BOT_VEHICLE_UPRIGHT ||
			(!person && iterator.index == vehicle->avoided_vehicle && game_time_get() < vehicle->avoided_until_tick))
		{
			continue;
		}
		seat = bot_vehicle_gunner_seat(iterator.index);
		if (seat == NONE || bot_vehicle_seat_rider(iterator.index, seat) != NONE ||
			bots_vehicle_taken(iterator.index, seat, bot) || !bot_vehicle_team_driver(unit_index, iterator.index))
		{
			continue;
		}
		/* (a person driving it, for one that otherwise only drives) */
		if (people_only && !person)
			continue;
		/* (moving off: a person's that drives away is not run after; a bot
		driver waits) */
		if (speed > BOT_VEHICLE_GUNNER_BOARD_SPEED && bot_vehicle_seat_rider(iterator.index,
			bot_vehicle_driver_seat(iterator.index)) != NONE && !bots_vehicle_bot_drives(iterator.index))
		{
			continue;
		}
		best = iterator.index;
		best_seat = seat;
		best_distance = distance;
	}
	if (best == NONE)
		return FALSE;
	vehicle->vehicle_index = best;
	vehicle->seat_index = best_seat;
	vehicle->role = _bot_vehicle_role_gunner;
	vehicle->goal.x = goal->x;
	vehicle->goal.y = goal->y;
	vehicle->goal.z = goal->z;
	vehicle->no_driver_ticks = 0;
	vehicle->parked_ticks = 0;
	bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_approach);
	vehicle->approach_until_tick = game_time_get() + BOT_VEHICLE_APPROACH_TICKS +
		(long)(best_distance * BOT_VEHICLE_APPROACH_TICKS_PER_UNIT);
	return TRUE;
}

/* a ride chosen, if one is worth it: the vehicle, its driver's seat, the goal
it rides toward; its walk to the door begun (TRUE) */
static boolean bot_vehicle_choose(
	struct bot *bot,
	long unit_index,
	real_point3d const *position,
	short mode)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_vehicle *vehicle = &brain->vehicle;
	struct bots_nav *nav = bots_nav_current();
	struct bots_nav *drive = NULL;
	long limit = config_integer("server.bot_vehicles");
	struct object_iterator iterator;
	struct bots_nav_point goal;
	/* (debug.bot_vehicle "tank": a Scorpion whenever it can) */
	boolean tanks_only = mode == _bot_vehicle_tank;
	long best = NONE;
	short best_seat = NONE;
	real best_distance = 0.0f;
	real trip, ride;
	long goal_node;
	struct bots_nav_node const *end;
	boolean fighting;

	if (tanks_only)
		mode = _bot_vehicle_drive;
	if (mode == _bot_vehicle_never || limit < 0 || !nav || (!bots_nav_drive_current() && !bots_nav_tank_current()) ||
		brain->goal_node == NONE || brain->goal_node >= nav->node_count)
	{
		return FALSE;
	}
	if (limit > 0 && bots_vehicle_riders() >= limit)
		return FALSE;
	if (bot_holds_flag(unit_get(unit_index)))
		return FALSE;
	goal = nav->nodes[brain->goal_node].point;
	/* (a team mate's vehicle near with its gun free: its gunner's seat) */
	if (bot_vehicle_choose_gun(bot, unit_index, position, mode, mode == _bot_vehicle_drive, &goal))
		return TRUE;
	if (mode == _bot_vehicle_gun)
		return FALSE;
	/* (fighting, a bot that may fight takes a Ghost near, to fight from: its
	guns are its driver's) */
	fighting = brain->combat.target_visible && bot->difficulty != _bot_difficulty_peaceful && mode != _bot_vehicle_drive;
	trip = bot_vehicle_distance2d(position, goal.x, goal.y);
	if (!fighting && trip < (mode == _bot_vehicle_drive ? BOT_VEHICLE_DRIVER_TRIP : BOT_VEHICLE_TRIP))
		return FALSE;
	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *object = object_get(iterator.index);
		real distance = (real)sqrt(distance_squared3d(position, &object->object.position));
		short seat;
		long node;

		if (distance > (mode == _bot_vehicle_drive ? BOT_VEHICLE_DRIVER_REACH : fighting ? BOT_VEHICLE_FIGHT_REACH :
			BOT_VEHICLE_REACH) || (best != NONE && distance >= best_distance))
			continue;
		/* (in a fight, a Ghost; a Warthog only for a hard or legendary bot,
		who runs people over with it: another's driver alone fights on foot. A
		peaceful bot fights nobody: an enemy in sight is no fight, and any
		vehicle takes it on its way) */
		if ((fighting || brain->combat.target_visible) && bot->difficulty < _bot_difficulty_hard &&
			bot->difficulty != _bot_difficulty_peaceful &&
			(!bot_vehicle_kind(iterator.index) || strcmp(bot_vehicle_kind(iterator.index), "ghost")))
		{
			continue;
		}
		if (tanks_only && !bot_vehicle_is_tank(iterator.index))
			continue;
		/* (one in the hill, the flag's stand it holds: no ride, it is there) */
		if (brain->objective.kind == _bot_objective_hold &&
			bot_vehicle_distance2d(&object->object.position, brain->objective.point.x, brain->objective.point.y) <
			MAX(brain->objective.radius, 3.0f) + 3.0f)
		{
			continue;
		}
		if (!bot_vehicle_alive(iterator.index) || !bot_vehicle_kind(iterator.index) || !bot_vehicle_graph(iterator.index) ||
			(iterator.index == vehicle->avoided_vehicle && game_time_get() < vehicle->avoided_until_tick))
		{
			continue;
		}
		seat = bot_vehicle_driver_seat(iterator.index);
		if (seat == NONE || bot_vehicle_ridden_for_driver(iterator.index, unit_index) ||
			bots_vehicle_taken(iterator.index, seat, bot) ||
			bot_vehicle_person_near(&object->object.position, BOT_VEHICLE_COURTESY))
		{
			continue;
		}
		/* (one standing near its goal already: no ride there - it would get
		out as soon as it got in) */
		if (bot_vehicle_distance2d(&object->object.position, goal.x, goal.y) < BOT_VEHICLE_DRIVER_TRIP)
			continue;
		drive = bot_vehicle_graph(iterator.index);
		node = bots_nav_locate(drive, object->object.position.x, object->object.position.y,
			object->object.position.z + 0.5f);
		if (node == NONE || drive->nodes[node].reach != (_bots_nav_reached_bit | _bots_nav_reaches_bit))
			continue;
		best = iterator.index;
		best_seat = seat;
		best_distance = distance;
	}
	if (best == NONE)
		return FALSE;
	drive = bot_vehicle_graph(best);
	goal_node = bot_vehicle_nearest_node(drive, goal.x, goal.y, goal.z);
	if (goal_node == NONE)
		return FALSE;
	end = &drive->nodes[goal_node];
	{
		real_point3d const *at = &object_get(best)->object.position;

		ride = best_distance + bot_vehicle_distance2d(at, end->point.x, end->point.y) / BOT_VEHICLE_SPEED_FACTOR +
			(real)sqrt((end->point.x - goal.x) * (end->point.x - goal.x) + (end->point.y - goal.y) * (end->point.y - goal.y)) +
			BOT_VEHICLE_OVERHEAD;
	}
	/* (its other way: a board, where it may skate, is quicker than walking) */
	{
		real foot = bot_skate_possible(bot) ? trip / BOT_VEHICLE_SKATE_FACTOR : trip;

		if (mode != _bot_vehicle_drive && !fighting && ride > BOT_VEHICLE_QUICKER * foot)
			return FALSE;
		if (config_boolean("debug.bot_trace") || bot_skate_possible(bot))
			platform_log("bots: player %ld takes vehicle %lx%s: %.0f world units of walking by it, %.0f %s",
				(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(unit_get(unit_index)->unit.player_index), best,
				fighting ? " in a fight" : mode == _bot_vehicle_drive ? " (debug.bot_vehicle)" : "", ride, foot,
				bot_skate_possible(bot) ? "on a board" : "on foot");
	}
	vehicle->vehicle_index = best;
	vehicle->seat_index = best_seat;
	vehicle->role = _bot_vehicle_role_driver;
	vehicle->goal.x = goal.x;
	vehicle->goal.y = goal.y;
	vehicle->goal.z = goal.z;
	vehicle->drive_goal_node = goal_node;
	vehicle->path_next = NONE;
	vehicle->overturned_ticks = 0;
	vehicle->stuck_events = 0;
	vehicle->edge_stuck = 0;
	vehicle->stuck_edge = NONE;
	vehicle->reverse_until_tick = 0;
	bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_approach);
	vehicle->approach_until_tick = game_time_get() + BOT_VEHICLE_APPROACH_TICKS +
		(long)(best_distance * BOT_VEHICLE_APPROACH_TICKS_PER_UNIT);
	return TRUE;
}

/* getting out, for a reason (said), and then on foot (or to flip it over) */
static void bot_vehicle_leave(
	struct bot_vehicle *vehicle,
	long player_index,
	char const *reason,
	boolean flip)
{
	platform_log("bots: player %ld gets out of vehicle %lx (%s) at tick %ld",
		(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), vehicle->vehicle_index, reason, game_time_get());
	vehicle->flip_after = flip;
	bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_leave);
}

/* whether someone on foot (or a vehicle) is in the way ahead of the vehicle,
within the corridor it drives; a vehicle: slowed for, not braked */
static boolean bot_vehicle_way_blocked(
	long vehicle_index,
	real speed,
	long rammed_unit,
	boolean *vehicle_ahead)
{
	struct object_datum *vehicle = object_get(vehicle_index);
	real ahead = BOT_VEHICLE_AHEAD + speed * BOT_VEHICLE_AHEAD_SECONDS + vehicle->object.bounding_sphere_radius;
	real fx = vehicle->object.forward.i, fy = vehicle->object.forward.j;
	real length = (real)sqrt(fx * fx + fy * fy);
	struct object_iterator iterator;
	boolean person = FALSE;

	*vehicle_ahead = FALSE;
	if (length < 0.01f)
		return FALSE;
	fx /= length;
	fy /= length;
	object_iterator_new(&iterator, _object_mask_biped | _object_mask_vehicle, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *object = object_get(iterator.index);
		real dx, dy, along, across;

		if (iterator.index == vehicle_index || iterator.index == rammed_unit || object->object.parent_object_index != NONE ||
			TEST_FLAG(object->object.damage_flags, _object_dead_bit))
		{
			continue;
		}
		dx = object->object.position.x - vehicle->object.position.x;
		dy = object->object.position.y - vehicle->object.position.y;
		if (fabs(object->object.position.z - vehicle->object.position.z) > 2.0f)
			continue;
		along = dx * fx + dy * fy;
		across = (real)fabs(dy * fx - dx * fy);
		if (along <= 0.0f || along > ahead + object->object.bounding_sphere_radius ||
			across > BOT_VEHICLE_CORRIDOR + object->object.bounding_sphere_radius)
		{
			continue;
		}
		if (object->object.type == _object_type_biped)
			person = TRUE;
		else
			*vehicle_ahead = TRUE;
	}
	return person;
}

/* whether the bot's game type objective (bots_objectives.c) has a point to
go to more than 5 world units off the goal it drives to */
static boolean bot_vehicle_objective_moved(
	struct bot_brain const *brain)
{
	short kind = brain->objective.kind;

	return (kind == _bot_objective_go || kind == _bot_objective_take || kind == _bot_objective_hold ||
		kind == _bot_objective_hunt) &&
		bot_vehicle_distance2d((real_point3d const *)&brain->objective.point, brain->vehicle.goal.x,
		brain->vehicle.goal.y) > 5.0f;
}

/* a new goal for a driver with a gunner aboard (its own goal reached): a
node of the drive graph's main part a good way off, at random; FALSE: none
found */
static boolean bot_vehicle_new_goal(
	struct bot_brain *brain,
	struct bots_nav *drive,
	real_point3d const *at)
{
	struct bot_vehicle *vehicle = &brain->vehicle;
	short tries;

	for (tries = 0; tries < 30; tries++)
	{
		long node = (long)(bot_random_real(brain) * (real)drive->node_count);
		struct bots_nav_node const *here;

		if (node < 0 || node >= drive->node_count)
			continue;
		here = &drive->nodes[node];
		if (here->reach != (_bots_nav_reached_bit | _bots_nav_reaches_bit) ||
			bot_vehicle_distance2d(at, here->point.x, here->point.y) < BOT_VEHICLE_ROAM)
		{
			continue;
		}
		vehicle->drive_goal_node = node;
		vehicle->goal.x = here->point.x;
		vehicle->goal.y = here->point.y;
		vehicle->goal.z = here->point.z;
		vehicle->path_next = NONE;
		return TRUE;
	}
	return FALSE;
}

/* a point on the drive graph to steer for, circling the target at a
fighting range (its side kept, else the other); FALSE: none on the graph */
static boolean bot_vehicle_orbit(
	struct bot_vehicle *vehicle,
	struct bots_nav *drive,
	real_point3d const *at,
	real_point3d const *target_at,
	struct bots_nav_point *point)
{
	real dx = at->x - target_at->x, dy = at->y - target_at->y;
	real around = (real)atan2(dy, dx);
	short tries;

	if (vehicle->orbit_side == 0.0f)
		vehicle->orbit_side = 1.0f;
	for (tries = 0; tries < 2; tries++)
	{
		real angle = around + vehicle->orbit_side * BOT_VEHICLE_ORBIT_STEP;

		point->x = target_at->x + (real)cos(angle) * BOT_VEHICLE_ORBIT_RANGE;
		point->y = target_at->y + (real)sin(angle) * BOT_VEHICLE_ORBIT_RANGE;
		point->z = target_at->z;
		if (bots_nav_locate(drive, point->x, point->y, point->z + 0.5f) != NONE)
			return TRUE;
		vehicle->orbit_side = -vehicle->orbit_side;
	}
	return FALSE;
}

/* whether the bot runs its target over on purpose, by its difficulty (the
owner's rule): peaceful and easy bots never (they brake for people, and only
accidents happen); normal ones a target on foot already ahead (within 20
degrees of the vehicle's heading, 12 world units); hard and legendary ones
hunt a target on foot within 25 world units. Never a team mate, nor anyone
riding */
static boolean bot_vehicle_may_ram(
	struct bot const *bot,
	long vehicle_index,
	long target_unit,
	real_point3d const *at,
	real heading,
	real target_distance)
{
	struct object_datum *target = target_unit != NONE ? object_try_and_get(target_unit) : NULL;
	real_point3d target_at;

	if (!target || target->object.type != _object_type_biped || target->object.parent_object_index != NONE ||
		bot->difficulty <= _bot_difficulty_easy || !bot->brain.combat.target_visible)
	{
		return FALSE;
	}
	if (game_engine_has_teams() &&
		!game_team_is_enemy(object_get(vehicle_index)->object.owner_team_index, target->object.owner_team_index))
	{
		return FALSE;
	}
	if (fabs(target->object.position.z - at->z) > BOT_VEHICLE_RAM_HEIGHT)
		return FALSE;
	if (bot->difficulty >= _bot_difficulty_hard)
		return target_distance < BOT_VEHICLE_HUNT_RANGE;
	object_get_origin(target_unit, &target_at);
	return target_distance < BOT_VEHICLE_RAM_RANGE &&
		fabs(bot_vehicle_angle((real)atan2(target_at.y - at->y, target_at.x - at->x) - heading)) < BOT_VEHICLE_RAM_CONE;
}

/* a Ghost's driver fighting: its guns aimed as it steers (bots_combat.c's
aim, the error and reaction of its difficulty), closing on a far target,
backing off a near one, strafing side to side (normal and up); at one it
may run over (bot_vehicle_may_ram), full on */
static void bot_vehicle_ghost_fight(
	struct bot *bot,
	long player_index,
	real_point3d const *target_at,
	real target_distance,
	real forward_speed,
	boolean ram,
	boolean tank,
	struct player_action *action)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_vehicle *vehicle = &brain->vehicle;
	long now = game_time_get();
	real along, side = 0.0f;
	boolean vehicle_ahead;

	(void)player_index;
	(void)target_at;
	along = target_distance > (tank ? BOT_VEHICLE_TANK_FAR : BOT_VEHICLE_GHOST_FAR) ? 0.6f :
		target_distance < (tank ? BOT_VEHICLE_TANK_NEAR : BOT_VEHICLE_GHOST_NEAR) ? -0.5f : 0.0f;
	/* (one it may run over: at them, full on, no strafing) */
	if (ram)
		along = 1.0f;
	if (bot->difficulty >= _bot_difficulty_normal)
	{
		if (now >= vehicle->strafe_until_tick)
		{
			vehicle->strafe_side = bot_random_real(brain) < 0.5f ? -1.0f : 1.0f;
			vehicle->strafe_until_tick = now + 20 + (long)(bot_random_real(brain) * 25.0f);
		}
		side = vehicle->strafe_side * (bot->difficulty >= _bot_difficulty_hard ? 0.8f : 0.4f);
	}
	/* (a tank strafes not; one it runs over, straight at them) */
	if (ram || tank)
		side = 0.0f;
	bot_action_idle(action, brain->yaw, 0.0f);
	action->throttle.i = along;
	action->throttle.j = side;
	/* (someone on foot ahead: braked for) */
	if (!ram && bot_vehicle_way_blocked(vehicle->vehicle_index, (real)fabs(forward_speed), NONE, &vehicle_ahead) &&
		forward_speed > 0.3f)
	{
		action->throttle.i = -1.0f;
	}
	bot_combat_act(bot, vehicle->unit_index, action, TRUE);
	brain->yaw = brain->combat.aim_yaw;
}

/* a gunner: its aim and fire at what it sees from the seat (bots_combat.c);
seeing nobody, it looks ahead and about. A peaceful bot only looks. It gets
out when the vehicle has had no driver a few seconds, or has stood still a
long while with a person driving and nobody to shoot at */
static void bot_vehicle_gun(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct player_action *action)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_vehicle *vehicle = &brain->vehicle;
	struct object_datum *object = object_get(vehicle->vehicle_index);
	real heading = (real)atan2(object->object.forward.j, object->object.forward.i);
	real speed = (real)sqrt(magnitude_squared3d(&object->object.translational_velocity)) * TICKS_PER_SECOND;
	long now = game_time_get();
	boolean engaged;

	brain->statistics.gunner_ticks++;
	engaged = bot->difficulty != _bot_difficulty_peaceful && bot_combat_engaged(bot, player_index, unit_index, bot->slot) &&
		brain->combat.target_player != NONE;
	vehicle->no_driver_ticks = unit_get(vehicle->vehicle_index)->unit.driver_object_index == NONE ?
		vehicle->no_driver_ticks + 1 : 0;
	vehicle->parked_ticks = speed < 0.3f && !engaged ? vehicle->parked_ticks + 1 : 0;
	if (vehicle->no_driver_ticks > BOT_VEHICLE_NO_DRIVER_TICKS)
	{
		bot_vehicle_leave(vehicle, player_index, "nobody drives", FALSE);
		bot_action_idle(action, brain->yaw, 0.0f);
		return;
	}
	if (vehicle->parked_ticks > BOT_VEHICLE_PARKED_TICKS && !bots_vehicle_bot_drives(vehicle->vehicle_index))
	{
		bot_vehicle_leave(vehicle, player_index, "parked", FALSE);
		bot_action_idle(action, brain->yaw, 0.0f);
		return;
	}
	if (engaged)
	{
		bot_action_idle(action, brain->yaw, brain->pitch);
		bot_combat_act(bot, unit_index, action, TRUE);
		brain->yaw = brain->combat.aim_yaw;
		brain->pitch = brain->combat.aim_pitch;
		return;
	}
	/* (nobody to shoot at: ahead, and a look about now and then) */
	{
		real look = bot_vehicle_angle(heading + (real)sin((real)(now + bot->slot * 37) / 45.0f) * 1.2f);
		real turn = bot_vehicle_angle(look - brain->yaw);

		brain->combat.aim_set = FALSE;
		brain->yaw = bot_vehicle_angle(brain->yaw + PIN(turn, -0.12f, 0.12f));
		brain->pitch = 0.0f;
		bot_action_idle(action, brain->yaw, 0.0f);
	}
}

/* where the fight a vehicle's crew knows of is, that its driver drives to:
the newest of a target its driver or its bot gunner saw lately (not now: a
target in sight is fought) and a noise either heard lately (bots_hearing.c's
gunfire and blasts, no better than an ear has it). Only a crew that fights
from it: its driver's own guns (a Ghost, a Scorpion), or a gunner who fires.
FALSE for none */
static boolean bot_vehicle_fight_known(
	struct bot const *bot,
	long vehicle_index,
	boolean driver_guns,
	real_point3d *point)
{
	struct bot const *crew[2];
	short index;
	long now = game_time_get();
	long newest = NONE;
	long gunner = unit_get(vehicle_index)->unit.gunner_object_index;

	crew[0] = driver_guns && bot->difficulty != _bot_difficulty_peaceful ? bot : NULL;
	crew[1] = !driver_guns && gunner != NONE ? bots_bot_of_unit(gunner) : NULL;
	if (crew[1] && crew[1]->difficulty == _bot_difficulty_peaceful)
		crew[1] = NULL;
	for (index = 0; index < 2; index++)
	{
		struct bot_combat const *combat = crew[index] ? &crew[index]->brain.combat : NULL;

		if (!combat)
			continue;
		if (combat->target_player != NONE && now - combat->last_seen_tick <= BOT_VEHICLE_FIGHT_MEMORY_TICKS &&
			now >= combat->last_seen_tick && combat->last_seen_tick > newest)
		{
			newest = combat->last_seen_tick;
			*point = combat->last_seen_position;
		}
		if (combat->heard_tick > 0 && now - combat->heard_tick <= BOT_VEHICLE_FIGHT_MEMORY_TICKS &&
			now >= combat->heard_tick && combat->heard_tick > newest)
		{
			newest = combat->heard_tick;
			*point = combat->heard_position;
		}
	}
	return newest != NONE;
}

/* driving: the bot's input as its vehicle's driver */
static void bot_vehicle_drive(
	struct bot *bot,
	long player_index,
	struct player_action *action)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_vehicle *vehicle = &brain->vehicle;
	struct bots_nav *drive = bot_vehicle_graph(vehicle->vehicle_index);
	struct object_datum *object = object_get(vehicle->vehicle_index);
	real_point3d at = object->object.position;
	real speed = (real)sqrt(object->object.translational_velocity.i * object->object.translational_velocity.i +
		object->object.translational_velocity.j * object->object.translational_velocity.j) * TICKS_PER_SECOND;
	real forward_speed = (object->object.translational_velocity.i * object->object.forward.i +
		object->object.translational_velocity.j * object->object.forward.j) * TICKS_PER_SECOND;
	real heading = (real)atan2(object->object.forward.j, object->object.forward.i);
	long now = game_time_get();
	real throttle, yaw, off, turn, remaining = 1.0e30f;
	boolean vehicle_ahead = FALSE;
	boolean blocked;
	struct bots_nav_point target;
	/* (a fight: its target, where, how far; a gunner aboard; whether it
	steers straight for a point off the graph's way (circling its target),
	or holds still for its gunner) */
	boolean fighting;
	long target_unit = NONE;
	real_point3d target_at = at;
	real target_distance = 1.0e30f;
	boolean gunner_aboard = object_get(vehicle->vehicle_index) && unit_get(vehicle->vehicle_index)->unit.gunner_object_index != NONE &&
		unit_get(vehicle->vehicle_index)->unit.gunner_object_index != vehicle->unit_index;
	boolean ghost = !strcmp(bot_vehicle_kind(vehicle->vehicle_index) ? bot_vehicle_kind(vehicle->vehicle_index) : "", "ghost");
	/* (its guns its driver's: a Ghost's, a Scorpion's cannon) */
	boolean driver_guns = unit_get(vehicle->vehicle_index)->unit.gunner_object_index == vehicle->unit_index;
	boolean tank = bot_vehicle_is_tank(vehicle->vehicle_index);
	boolean direct = FALSE;
	boolean hold = FALSE;
	boolean ram = FALSE;
	/* (a person aboard - its gunner, a passenger: it drives them, and gets
	out only when it must) */
	long person = bot_vehicle_person_aboard(vehicle->vehicle_index);
	struct bots_nav_point direct_point;
	real_point3d known;
	boolean on_hill;

	/* (how far, how long) */
	{
		real dx = at.x - vehicle->last_position.x, dy = at.y - vehicle->last_position.y;
		real step = (real)sqrt(dx * dx + dy * dy);

		if (step < 3.0f)
			brain->statistics.vehicle_travelled += step;
		vehicle->last_position = at;
		brain->statistics.vehicle_ticks++;
	}
	/* (no graph for it on this map: out) */
	if (!drive)
	{
		bot_vehicle_leave(vehicle, player_index, "no way on", FALSE);
		bot_action_idle(action, brain->yaw, 0.0f);
		return;
	}
	/* (turned over: out, and flipped back) */
	vehicle->overturned_ticks = object->object.up.k < BOT_VEHICLE_OVERTURNED ? vehicle->overturned_ticks + 1 : 0;
	if (vehicle->overturned_ticks > BOT_VEHICLE_OVERTURNED_TICKS)
	{
		bot_vehicle_leave(vehicle, player_index, "it turned over", TRUE);
		bot_action_idle(action, brain->yaw, 0.0f);
		return;
	}
	if ((person != NONE) != vehicle->person_aboard)
	{
		vehicle->person_aboard = person != NONE;
		platform_log("bots: player %ld drives %s at tick %ld", (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index),
			person != NONE ? "with a person aboard" : "with no person aboard", now);
	}
	if (person != NONE)
		brain->statistics.person_ticks++;
	/* (a person waiting at its door, the vehicle slow: given the seat) */
	if (person == NONE && speed < 1.0f &&
		bot_vehicle_person_at_door(vehicle->unit_index, vehicle->vehicle_index, vehicle->seat_index))
	{
		if (++vehicle->door_waiting_ticks > BOT_VEHICLE_DOOR_WAITING_TICKS)
		{
			vehicle->yielded = TRUE;
			bot_vehicle_leave(vehicle, player_index, "a person wants it", FALSE);
			bot_action_idle(action, brain->yaw, 0.0f);
			return;
		}
	}
	else
	{
		vehicle->door_waiting_ticks = 0;
	}
	/* (what it sees from its seat, fighting: a bot that may fight) */
	fighting = bot->difficulty != _bot_difficulty_peaceful &&
		bot_combat_engaged(bot, player_index, vehicle->unit_index, bot->slot) && brain->combat.target_visible;
	if (fighting)
	{
		struct player_datum *target_player = player_try_and_get(brain->combat.target_player);

		if (target_player && target_player->unit_index != NONE)
		{
			target_unit = target_player->unit_index;
			object_get_origin(target_unit, &target_at);
			target_distance = (real)sqrt(distance_squared3d(&at, &target_at));
		}
		else
		{
			fighting = FALSE;
		}
	}
	else
	{
		brain->combat.aim_set = FALSE;
	}
	/* (a team mate going for its gun: waited for, a while) */
	if (!gunner_aboard && !driver_guns && bots_vehicle_gunner_coming(vehicle->vehicle_index) &&
		now - vehicle->phase_tick < BOT_VEHICLE_CREW_WAIT_TICKS)
	{
		bot_action_idle(action, brain->yaw, 0.0f);
		action->throttle.i = forward_speed > 0.5f ? -1.0f : 0.0f;
		vehicle->check_tick = now;
		vehicle->check_position = at;
		return;
	}
	/* (one on foot it may run over, by its difficulty: bot_vehicle_may_ram) */
	ram = fighting && (driver_guns || !gunner_aboard) &&
		bot_vehicle_may_ram(bot, vehicle->vehicle_index, target_unit, &at, heading, target_distance);
	if (ram != vehicle->ramming)
	{
		vehicle->ramming = ram;
		if (ram)
		{
			brain->statistics.vehicle_rams++;
			platform_log("bots: player %ld drives at player %ld at tick %ld", (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index),
				(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(brain->combat.target_player), now);
		}
	}
	/* (a Ghost, a Scorpion fight: their guns are their driver's) */
	if (fighting && driver_guns)
	{
		bot_vehicle_ghost_fight(bot, player_index, &target_at, target_distance, forward_speed, ram, tank, action);
		vehicle->path_next = NONE;
		vehicle->check_tick = now;
		vehicle->check_position = at;
		return;
	}
	if (fighting && config_boolean("debug.bot_trace"))
		platform_log("bots: trace fight: player %ld in %lx driver %lx gunner %lx (me %lx) gunner seat %d rider %lx, target %.1f away",
			(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), vehicle->vehicle_index,
			unit_get(vehicle->vehicle_index)->unit.driver_object_index, unit_get(vehicle->vehicle_index)->unit.gunner_object_index,
			vehicle->unit_index, bot_vehicle_gunner_seat(vehicle->vehicle_index),
			bot_vehicle_gunner_seat(vehicle->vehicle_index) == NONE ? NONE :
			bot_vehicle_seat_rider(vehicle->vehicle_index, bot_vehicle_gunner_seat(vehicle->vehicle_index)), target_distance);
	/* (a Warthog alone: at one it may run over, a little ahead of where they
	go; else, an enemy near, out to fight on foot) */
	if (ram && !gunner_aboard)
	{
		real lead = target_distance / MAX(speed, 3.0f);
		struct object_datum *rammed = object_get(target_unit);

		direct = TRUE;
		direct_point.x = target_at.x + rammed->object.translational_velocity.i * TICKS_PER_SECOND * MIN(lead, 1.5f);
		direct_point.y = target_at.y + rammed->object.translational_velocity.j * TICKS_PER_SECOND * MIN(lead, 1.5f);
		direct_point.z = target_at.z;
	}
	else if (fighting && !gunner_aboard && person == NONE && target_distance < BOT_VEHICLE_FIGHT_ON_FOOT)
	{
		bot_vehicle_leave(vehicle, player_index, "a fight", FALSE);
		bot_action_idle(action, brain->yaw, 0.0f);
		return;
	}
	/* (a gunner aboard: circling the target (hard, legendary), else near it
	and still, for the gunner's shots) */
	if (fighting && gunner_aboard)
	{
		if (bot->difficulty >= _bot_difficulty_hard)
			direct = bot_vehicle_orbit(vehicle, drive, &at, &target_at, &direct_point);
		if (!direct)
		{
			if (bot_vehicle_distance2d(&target_at, vehicle->goal.x, vehicle->goal.y) > 6.0f)
			{
				long node = bot_vehicle_nearest_node(drive, target_at.x, target_at.y, target_at.z);

				vehicle->goal.x = target_at.x;
				vehicle->goal.y = target_at.y;
				vehicle->goal.z = target_at.z;
				if (node != NONE)
				{
					vehicle->drive_goal_node = node;
					vehicle->path_next = NONE;
				}
			}
			hold = target_distance < BOT_VEHICLE_STANDOFF;
		}
	}
	if (direct)
	{
		target = direct_point;
		vehicle->path_next = NONE;
		goto steer;
	}
	/* (its game type's objective - a race's next flag, a flag or a ball to
	take, a hill or a flag's stand to hold, someone to hunt - moved on: its
	goal; with a person aboard, theirs is the ride) */
	if (person == NONE && bot_vehicle_objective_moved(brain))
	{
		long node = bot_vehicle_nearest_node(drive, brain->objective.point.x, brain->objective.point.y,
			brain->objective.point.z);

		vehicle->goal.x = brain->objective.point.x;
		vehicle->goal.y = brain->objective.point.y;
		vehicle->goal.z = brain->objective.point.z;
		if (node != NONE)
		{
			vehicle->drive_goal_node = node;
			vehicle->path_next = NONE;
		}
	}
	/* (a crew that fights from its vehicle, its game type's objective not
	moving it on: to the fight it knows of - a target seen lately, a noise
	heard. A vehicle's guns find fights only so: it sees as far as its crew,
	no further) */
	else if (person == NONE && !fighting && brain->objective.kind == _bot_objective_none &&
		bot_vehicle_fight_known(bot, vehicle->vehicle_index, driver_guns, &known) &&
		bot_vehicle_distance2d(&known, vehicle->goal.x, vehicle->goal.y) > BOT_VEHICLE_FIGHT_REGOAL)
	{
		long node = bot_vehicle_nearest_node(drive, known.x, known.y, known.z);

		if (node != NONE)
		{
			vehicle->goal.x = known.x;
			vehicle->goal.y = known.y;
			vehicle->goal.z = known.z;
			vehicle->drive_goal_node = node;
			vehicle->path_next = NONE;
			if (config_boolean("debug.bot_trace"))
				platform_log("bots: trace player %ld drives to a fight it knows of at (%.1f %.1f)",
					(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), known.x, known.y);
		}
	}
	/* (a hill, a flag's stand it holds: there, out, on foot within it - but
	in a vehicle whose guns are its driver's (a Ghost, a Scorpion), which
	holds it better than a bot on foot: there it stays, still) */
	on_hill = person == NONE && brain->objective.kind == _bot_objective_hold &&
		bot_vehicle_distance2d(&at, brain->objective.point.x, brain->objective.point.y) < MAX(brain->objective.radius, 3.0f);
	if (on_hill && !gunner_aboard && !driver_guns)
	{
		bot_vehicle_leave(vehicle, player_index, "its objective", FALSE);
		bot_action_idle(action, brain->yaw, 0.0f);
		return;
	}
	/* (its way along the drive graph) */
	if (vehicle->path_next == NONE)
	{
		long from = bots_nav_locate(drive, at.x, at.y, at.z + 0.5f);
		long count;

		if (from == NONE)
			from = bot_vehicle_nearest_node(drive, at.x, at.y, at.z);
		count = from == NONE ? -1 : bots_nav_find_path(drive, from, vehicle->drive_goal_node, vehicle->path,
			BOT_PATH_MAXIMUM);
		if (count < 0)
		{
			bot_vehicle_leave(vehicle, player_index, "no way on", FALSE);
			bot_action_idle(action, brain->yaw, 0.0f);
			return;
		}
		vehicle->path_next = count - 1;
		vehicle->path_count = count;
	}
	/* (the nearest of the next few nodes along its way: one it went past
	wide, fast, is not turned back for) */
	{
		long index, best = vehicle->path_next;
		real best_distance = 1.0e30f;

		for (index = vehicle->path_next; index >= 0 && index > vehicle->path_next - BOT_VEHICLE_SKIP_NODES; index--)
		{
			struct bots_nav_point const *point = &drive->nodes[drive->edges[vehicle->path[index]].target].point;
			real distance = bot_vehicle_distance2d(&at, point->x, point->y);

			if (distance < best_distance)
			{
				best_distance = distance;
				best = index;
			}
		}
		vehicle->path_next = best;
	}
	while (vehicle->path_next >= 0)
	{
		struct bots_nav_point const *point = &drive->nodes[drive->edges[vehicle->path[vehicle->path_next]].target].point;

		if (bot_vehicle_distance2d(&at, point->x, point->y) >= BOT_VEHICLE_NODE_PASSED)
			break;
		vehicle->path_next--;
	}
	{
		struct bots_nav_point const *end = &drive->nodes[vehicle->drive_goal_node].point;

		remaining = bot_vehicle_distance2d(&at, end->x, end->y);
		if (vehicle->path_next < 0 || remaining < BOT_VEHICLE_ARRIVED)
		{
			/* (its gunner fighting: there it stays, still; a gunner aboard and
			nobody to fight: on to another place, its gunner's ride goes on) */
			/* (... or there, at a hill it holds in a vehicle whose guns are its
			own: as near as its way arrives) */
			if ((fighting && gunner_aboard) || (driver_guns && person == NONE && brain->objective.kind == _bot_objective_hold &&
				bot_vehicle_distance2d(&at, brain->objective.point.x, brain->objective.point.y) <
				MAX(brain->objective.radius, 3.0f) + BOT_VEHICLE_ARRIVED))
			{
				hold = TRUE;
				vehicle->path_next = vehicle->path_next < 0 ? 0 : vehicle->path_next;
			}
			else if ((gunner_aboard || person != NONE) && bot_vehicle_new_goal(brain, drive, &at))
			{
				bot_action_idle(action, brain->yaw, 0.0f);
				return;
			}
			else
			{
				bot_vehicle_leave(vehicle, player_index, "there", FALSE);
				bot_action_idle(action, brain->yaw, 0.0f);
				return;
			}
		}
	}
	/* (the point it steers for: the first node ahead along its way at least
	the look-ahead off, else the last) */
	{
		real lookahead = BOT_VEHICLE_LOOKAHEAD + BOT_VEHICLE_LOOKAHEAD_SECONDS * speed;
		long index;

		if (lookahead > BOT_VEHICLE_LOOKAHEAD_MAXIMUM)
			lookahead = BOT_VEHICLE_LOOKAHEAD_MAXIMUM;
		target = drive->nodes[drive->edges[vehicle->path[0]].target].point;
		for (index = vehicle->path_next; index >= 0; index--)
		{
			struct bots_nav_point const *point = &drive->nodes[drive->edges[vehicle->path[index]].target].point;

			if (bot_vehicle_distance2d(&at, point->x, point->y) >= lookahead)
			{
				target = *point;
				break;
			}
		}
	}
steer:
	yaw = (real)atan2(target.y - at.y, target.x - at.x);
	off = bot_vehicle_angle(yaw - heading);
	/* (how fast: slower in a turn, near its goal; nothing in the air) */
	throttle = bot_vehicle_throttle_cap(bot);
	if (fabs(off) > 1.2f)
		throttle = 0.45f;
	else if (fabs(off) > 0.6f)
		throttle *= 0.7f;
	if (remaining < BOT_VEHICLE_SLOW_NEAR && throttle > 0.5f)
		throttle = 0.5f;
	/* (leaning - over a bump, on a side slope, a wheel up: eased off, before
	it rolls) */
	if (object->object.up.k < BOT_VEHICLE_LEANING && throttle > 0.4f)
		throttle = 0.4f;
	/* (a Ghost hovers: never on the ground, and driven all the same) */
	if (!ghost && !TEST_FLAG(object->object.flags, _object_on_ground_bit))
		throttle = 0.0f;
	/* (running someone over: as fast as it goes, turns aside) */
	if (ram && throttle > 0.0f && fabs(off) < 0.6f)
		throttle = 1.0f;
	/* (still for its gunner: braked to a stop) */
	if (hold)
		throttle = forward_speed > 0.5f ? -0.6f : 0.0f;
	/* (someone on foot ahead: braked for, whoever they are; a vehicle:
	slowed for) */
	blocked = bot_vehicle_way_blocked(vehicle->vehicle_index, speed, ram ? target_unit : NONE, &vehicle_ahead);
	if (blocked)
	{
		if (!vehicle->braking)
			brain->statistics.vehicle_braked++;
		vehicle->braking = TRUE;
		throttle = forward_speed > 0.5f ? -1.0f : 0.0f;
	}
	else
	{
		vehicle->braking = FALSE;
		if (vehicle_ahead && throttle > 0.3f)
			throttle = 0.3f;
	}
	/* (getting nowhere: back up, the wheels the other way) */
	if (now - vehicle->check_tick >= BOT_VEHICLE_CHECK_TICKS)
	{
		real moved = bot_vehicle_distance2d(&at, vehicle->check_position.x, vehicle->check_position.y);

		if (moved < BOT_VEHICLE_PROGRESS && vehicle->last_throttle > 0.3f && !blocked && now >= vehicle->reverse_until_tick)
		{
			long edge = vehicle->path[vehicle->path_next];

			brain->statistics.vehicle_reverses++;
			vehicle->reverse_until_tick = now + BOT_VEHICLE_REVERSE_TICKS;
			/* (each time the other way round what is in the way) */
			vehicle->reverse_side = vehicle->reverse_side > 0.0f ? -1.0f : 1.0f;
			if (now - vehicle->stuck_window_tick > BOT_VEHICLE_STUCK_WINDOW)
			{
				vehicle->stuck_window_tick = now;
				vehicle->stuck_events = 0;
			}
			vehicle->stuck_events++;
			vehicle->edge_stuck = edge == vehicle->stuck_edge ? vehicle->edge_stuck + 1 : 1;
			vehicle->stuck_edge = edge;
			if (vehicle->stuck_events >= BOT_VEHICLE_STUCK_LIMIT)
			{
				bot_vehicle_leave(vehicle, player_index, "stuck", FALSE);
				bot_action_idle(action, brain->yaw, 0.0f);
				return;
			}
			/* (stuck again and again on this part of the way: kept off it) */
			if (vehicle->edge_stuck >= BOT_VEHICLE_EDGE_STUCK && edge >= 0 && edge < drive->edge_count)
			{
				if (drive->edges[edge].cost < 500.0f)
					drive->edges[edge].cost += drive->edges[edge].failed ? 1000.0f : 40.0f;
				drive->edges[edge].failed = 1;
				vehicle->path_next = NONE;
				vehicle->edge_stuck = 0;
			}
		}
		vehicle->check_tick = now;
		vehicle->check_position = at;
	}
	if (now < vehicle->reverse_until_tick)
	{
		/* (backing up, facing to a side: the vehicle steers as its driver
		looks going back too, so its nose comes round that way; then on
		straight a while, past what was in the way) */
		yaw = bot_vehicle_angle(heading + vehicle->reverse_side * BOT_VEHICLE_REVERSE_TURN);
		throttle = -1.0f;
		vehicle->detour_until_tick = vehicle->reverse_until_tick + BOT_VEHICLE_DETOUR_TICKS;
	}
	else if (now < vehicle->detour_until_tick && !blocked)
	{
		yaw = heading;
		if (throttle > 0.6f)
			throttle = 0.6f;
	}
	vehicle->last_throttle = throttle;
	if (config_boolean("debug.bot_trace") && now % TICKS_PER_SECOND == 0)
		platform_log("bots: trace drive: player %ld at (%.1f %.1f %.1f) up %.2f speed %.1f (forward %.1f) throttle %.2f off %.0f step %ld/%ld %.0f to go%s%s",
			(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), at.x, at.y, at.z, object->object.up.k, speed, forward_speed,
			throttle, off * 180.0f / _pi, vehicle->path_next, vehicle->path_count, remaining, blocked ? " braking" : "",
			now < vehicle->reverse_until_tick ? " reversing" : "");
	/* (the facing turned toward where it steers, a turn a tick at most) */
	turn = bot_vehicle_angle(yaw - brain->yaw);
	if (turn > BOT_VEHICLE_TURN_RATE)
		turn = BOT_VEHICLE_TURN_RATE;
	else if (turn < -BOT_VEHICLE_TURN_RATE)
		turn = -BOT_VEHICLE_TURN_RATE;
	brain->yaw = bot_vehicle_angle(brain->yaw + turn);
	brain->pitch = 0.0f;
	bot_action_idle(action, brain->yaw, brain->pitch);
	action->throttle.i = throttle;
}

/* ---------- the bot's turn */

boolean bot_vehicle_think(
	struct bot *bot,
	long player_index,
	long unit_index,
	real_point3d const *position,
	struct player_action *action)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_vehicle *vehicle = &brain->vehicle;
	struct player_datum *player = player_get(player_index);
	long parent = object_get(unit_index)->object.parent_object_index;
	long now = game_time_get();
	short mode = bot_vehicle_mode(bot);

	/* (a life, a game: afresh) */
	if (vehicle->unit_index != unit_index || now < vehicle->phase_tick)
	{
		csmemset(vehicle, 0, sizeof(*vehicle));
		vehicle->unit_index = unit_index;
		vehicle->vehicle_index = NONE;
		vehicle->avoided_vehicle = NONE;
		vehicle->seat_index = NONE;
		vehicle->stuck_edge = NONE;
		vehicle->path_next = NONE;
		vehicle->look_tick = now + TICKS_PER_SECOND + bot->slot % BOT_VEHICLE_LOOK_TICKS;
		bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_none);
	}
	if (mode == _bot_vehicle_stand)
	{
		if (parent != NONE)
			return FALSE;
		bot_action_idle(action, brain->yaw, brain->pitch);
		return TRUE;
	}
	/* (its vehicle gone, or destroyed: on foot) */
	if (vehicle->phase != _bot_vehicle_phase_none && !bot_vehicle_alive(vehicle->vehicle_index))
	{
		if (parent != NONE)
		{
			bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_none);
			return FALSE;
		}
		vehicle->vehicle_index = NONE;
		bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_none);
		bot_vehicle_walk_on(brain);
		return FALSE;
	}
	switch (vehicle->phase)
	{
	case _bot_vehicle_phase_none:
		/* (in a vehicle it never chose: bot_vehicle_keep_out takes it out) */
		if (parent != NONE || now < vehicle->look_tick)
			return FALSE;
		vehicle->look_tick = now + BOT_VEHICLE_LOOK_TICKS;
		if (!bot_vehicle_choose(bot, unit_index, position, mode))
			return FALSE;
		/* (on to the door) */

	case _bot_vehicle_phase_approach:
	{
		real_point3d door, seat;
		real distance;

		if (parent != NONE)
		{
			bot_vehicle_give_up(vehicle, BOT_VEHICLE_AVOID_TICKS, "in a seat walking there");
			return FALSE;
		}
		/* (on its way to drive, a person's vehicle near with its gun free: that
		gun first, a person's ride before its own) */
		if (vehicle->role == _bot_vehicle_role_driver && (now + bot->slot) % BOT_VEHICLE_LOOK_TICKS == 0)
		{
			struct bots_nav_point goal;

			goal = vehicle->goal;
			if (bot_vehicle_choose_gun(bot, unit_index, position, mode, TRUE, &goal))
				return FALSE;
		}
		{
			char const *reason = NULL;

			boolean gunner = vehicle->role == _bot_vehicle_role_gunner;

			if (gunner ? bot_vehicle_seat_rider(vehicle->vehicle_index, vehicle->seat_index) != NONE :
				bot_vehicle_ridden_for_driver(vehicle->vehicle_index, unit_index))
			{
				reason = "someone got in";
			}
			else if (gunner && !bot_vehicle_team_driver(unit_index, vehicle->vehicle_index))
				reason = "no team mate drives it";
			else if (gunner && object_get(vehicle->vehicle_index)->object.up.k < BOT_VEHICLE_UPRIGHT)
				reason = "not upright";
			else if (object_get(vehicle->vehicle_index)->object.up.k < BOT_VEHICLE_UPRIGHT)
			{
				/* (on its side or roof: flipped back over first) */
				bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_flip);
				vehicle->flip_presses = 0;
				return FALSE;
			}
			else if (!unit_get_seat_entrance_point(unit_index, vehicle->vehicle_index, vehicle->seat_index, &door, &seat, NULL))
				reason = "no door";
			/* (a fight: on to a Ghost or a gun, which fight; not to a Warthog to
			drive, but for a hard or legendary bot, who runs people over) */
			else if (brain->combat.target_visible && bot->difficulty != _bot_difficulty_peaceful && mode == _bot_vehicle_normal &&
				!gunner && strcmp(bot_vehicle_kind(vehicle->vehicle_index), "ghost") && bot->difficulty < _bot_difficulty_hard)
			{
				reason = "a fight";
			}
			else if (!gunner && bot_vehicle_person_near(&object_get(vehicle->vehicle_index)->object.position,
				BOT_VEHICLE_COURTESY))
			{
				reason = "a person near it";
			}
			if (reason)
			{
				bot_vehicle_give_up(vehicle, BOT_VEHICLE_AVOID_TICKS / 3, reason);
				bot_vehicle_walk_on(brain);
				return FALSE;
			}
		}
		distance = bot_vehicle_distance2d(position, door.x, door.y);
		if (now > vehicle->approach_until_tick && vehicle->phase == _bot_vehicle_phase_approach)
		{
			bot_vehicle_give_up(vehicle, BOT_VEHICLE_AVOID_TICKS, "too long getting there");
			bot_vehicle_walk_on(brain);
			return FALSE;
		}
		if (distance > BOT_VEHICLE_DOOR_NEAR)
		{
			/* (along the walking graph: the brain walks there) */
			if (!bot_vehicle_head_for(brain, &door))
			{
				bot_vehicle_give_up(vehicle, BOT_VEHICLE_AVOID_TICKS, "no way there");
				bot_vehicle_walk_on(brain);
			}
			return FALSE;
		}
		bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_board);
	}
		/* (at the door) */

	case _bot_vehicle_phase_board:
	{
		real_point3d door, seat;
		real distance;

		if (parent == vehicle->vehicle_index && object_get(unit_index)->object.parent_object_index != NONE &&
			unit_get(unit_index)->unit.parent_seat_index == vehicle->seat_index)
		{
			if (vehicle->role == _bot_vehicle_role_gunner)
			{
				short driver_seat = bot_vehicle_driver_seat(vehicle->vehicle_index);
				long driver = driver_seat != NONE ? bot_vehicle_seat_rider(vehicle->vehicle_index, driver_seat) : NONE;
				long driver_player = driver != NONE ? unit_get(driver)->unit.player_index : NONE;

				brain->statistics.gunner_rides++;
				platform_log("bots: player %ld guns vehicle %lx (%s), %s at tick %ld",
					(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), vehicle->vehicle_index,
					bot_vehicle_kind(vehicle->vehicle_index), driver_player == NONE ? "its driver to come" :
					bots_player_is_bot(player_get(driver_player)) ? "a bot driving" : "a person driving", now);
				vehicle->no_driver_ticks = 0;
				vehicle->parked_ticks = 0;
			}
			else
			{
				brain->statistics.vehicle_rides++;
				platform_log("bots: player %ld drives vehicle %lx (%s) at tick %ld",
					(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), vehicle->vehicle_index,
					bot_vehicle_kind(vehicle->vehicle_index), now);
			}
			vehicle->last_position = object_get(vehicle->vehicle_index)->object.position;
			vehicle->check_position = vehicle->last_position;
			vehicle->check_tick = now;
			vehicle->path_next = NONE;
			vehicle->last_throttle = 0.0f;
			vehicle->braking = FALSE;
			brain->yaw = (real)atan2(object_get(vehicle->vehicle_index)->object.forward.j,
				object_get(vehicle->vehicle_index)->object.forward.i);
			bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_ride);
			bot_action_idle(action, brain->yaw, 0.0f);
			return TRUE;
		}
		if (parent != NONE)
		{
			/* (another seat, or another vehicle: out) */
			bot_vehicle_leave(vehicle, player_index, "the wrong seat", FALSE);
			bot_action_idle(action, brain->yaw, brain->pitch);
			return TRUE;
		}
		if (now - vehicle->phase_tick > BOT_VEHICLE_BOARD_TICKS ||
			(vehicle->role == _bot_vehicle_role_gunner ? bot_vehicle_seat_rider(vehicle->vehicle_index, vehicle->seat_index) != NONE :
			bot_vehicle_ridden_for_driver(vehicle->vehicle_index, unit_index)) ||
			!unit_get_seat_entrance_point(unit_index, vehicle->vehicle_index, vehicle->seat_index, &door, &seat, NULL))
		{
			bot_vehicle_give_up(vehicle, BOT_VEHICLE_AVOID_TICKS, "not in at the door");
			bot_vehicle_walk_on(brain);
			return FALSE;
		}
		distance = bot_vehicle_distance2d(position, door.x, door.y);
		if (config_boolean("debug.bot_trace") && (now - vehicle->phase_tick) % 10 == 0)
			platform_log("bots: trace door: player %ld result %d object %lx seat %d (wants %d) at %.2f from the door, speed %.3f",
				(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), player->action_result, player->action_object_index,
				player->action_seat_index, vehicle->seat_index, distance,
				(real)sqrt(magnitude_squared3d(&object_get(unit_index)->object.translational_velocity)));
		/* (still: the game takes nobody in moving; stepping closer while the
		game offers another seat or none) */
		if (player->action_result == _player_action_result_enter_vehicle &&
			player->action_object_index == vehicle->vehicle_index && player->action_seat_index == vehicle->seat_index)
		{
			bot_vehicle_step_to(brain, position, &object_get(vehicle->vehicle_index)->object.position, 0.0f, action);
			if (now - vehicle->press_tick >= BOT_VEHICLE_PRESS_TICKS)
			{
				SET_FLAG(action->control_flags, _unit_control_action_bit, TRUE);
				vehicle->press_tick = now;
			}
		}
		else
		{
			real_point3d target;

			bot_vehicle_door_way(vehicle->vehicle_index, position, &door, &target);
			bot_vehicle_step_to(brain, position, &target, distance > BOT_VEHICLE_DOOR_STAND ? 0.6f : 0.25f, action);
		}
		return TRUE;
	}

	case _bot_vehicle_phase_ride:
		if (parent != vehicle->vehicle_index)
		{
			/* (thrown out: turned over, the game puts its riders out) */
			boolean overturned = object_get(vehicle->vehicle_index)->object.up.k < BOT_VEHICLE_UPRIGHT;

			platform_log("bots: player %ld is out of vehicle %lx (%s) at tick %ld",
				(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), vehicle->vehicle_index,
				overturned ? "it turned over" : "put out", now);
			if (overturned && vehicle->role == _bot_vehicle_role_driver)
			{
				bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_flip);
				vehicle->flip_presses = 0;
			}
			else
			{
				bot_vehicle_give_up(vehicle, 3 * TICKS_PER_SECOND, "put out");
			}
			bot_vehicle_walk_on(brain);
			return FALSE;
		}
		if (vehicle->role == _bot_vehicle_role_gunner)
			bot_vehicle_gun(bot, player_index, unit_index, action);
		else
			bot_vehicle_drive(bot, player_index, action);
		return TRUE;

	case _bot_vehicle_phase_leave:
	{
		struct object_datum *object = object_get(vehicle->vehicle_index);
		real forward_speed = (object->object.translational_velocity.i * object->object.forward.i +
			object->object.translational_velocity.j * object->object.forward.j) * TICKS_PER_SECOND;

		if (parent == NONE)
		{
			platform_log("bots: player %ld got out of vehicle %lx at tick %ld",
				(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), vehicle->vehicle_index, now);
			if (vehicle->flip_after)
			{
				bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_flip);
				vehicle->flip_presses = 0;
			}
			else
			{
				bot_vehicle_give_up(vehicle, vehicle->yielded ? BOT_VEHICLE_AVOID_TICKS : 6 * TICKS_PER_SECOND, "got out");
				vehicle->yielded = FALSE;
			}
			bot_vehicle_walk_on(brain);
			return FALSE;
		}
		bot_action_idle(action, brain->yaw, 0.0f);
		/* (stopped first, braking against the way it goes) */
		if (parent == vehicle->vehicle_index && vehicle->role == _bot_vehicle_role_driver && fabs(forward_speed) > 1.0f &&
			now - vehicle->phase_tick < 3 * TICKS_PER_SECOND)
		{
			action->throttle.i = forward_speed > 0.0f ? -1.0f : 1.0f;
			return TRUE;
		}
		if (now - vehicle->press_tick >= BOT_VEHICLE_PRESS_TICKS / 2)
		{
			SET_FLAG(action->control_flags, _unit_control_action_bit, TRUE);
			vehicle->press_tick = now;
		}
		if (now - vehicle->phase_tick > BOT_VEHICLE_LEAVE_TICKS)
			platform_log("bots: player %ld cannot get out of vehicle %lx", (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index),
				vehicle->vehicle_index);
		return TRUE;
	}

	case _bot_vehicle_phase_flip:
	{
		struct object_datum *object = object_get(vehicle->vehicle_index);
		real distance;

		if (parent != NONE)
		{
			bot_vehicle_give_up(vehicle, BOT_VEHICLE_AVOID_TICKS, "in a seat flipping it");
			return FALSE;
		}
		/* (upright again: in again, if the ride is still worth it) */
		if (object->object.up.k > BOT_VEHICLE_UPRIGHT)
		{
			if (vehicle->flip_presses > 0)
			{
				brain->statistics.vehicle_flips++;
				platform_log("bots: player %ld flipped vehicle %lx back over at tick %ld",
					(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), vehicle->vehicle_index, now);
			}
			vehicle->avoided_vehicle = NONE;
			vehicle->vehicle_index = NONE;
			vehicle->look_tick = now;
			bot_vehicle_set_phase(vehicle, _bot_vehicle_phase_none);
			return FALSE;
		}
		if (now - vehicle->phase_tick > BOT_VEHICLE_FLIP_TICKS)
		{
			bot_vehicle_give_up(vehicle, BOT_VEHICLE_AVOID_TICKS, "not flipped");
			bot_vehicle_walk_on(brain);
			return FALSE;
		}
		if (player->action_result == _player_action_result_flip_vehicle &&
			player->action_object_index == vehicle->vehicle_index)
		{
			bot_vehicle_step_to(brain, position, &object->object.position, 0.0f, action);
			if (now - vehicle->press_tick >= 2 * BOT_VEHICLE_PRESS_TICKS)
			{
				SET_FLAG(action->control_flags, _unit_control_action_bit, TRUE);
				vehicle->press_tick = now;
				vehicle->flip_presses++;
			}
			return TRUE;
		}
		distance = bot_vehicle_distance2d(position, object->object.position.x, object->object.position.y);
		if (distance > BOT_VEHICLE_FLIP_NEAR && bot_vehicle_head_for(brain, &object->object.position))
			return FALSE;
		bot_vehicle_step_to(brain, position, &object->object.position, 0.6f, action);
		return TRUE;
	}
	}
	return FALSE;
}

boolean bot_vehicle_gunner_coming(
	struct bot const *bot,
	long vehicle_index)
{
	struct bot_vehicle const *vehicle = &bot->brain.vehicle;

	return vehicle->vehicle_index == vehicle_index && vehicle->role == _bot_vehicle_role_gunner &&
		(vehicle->phase == _bot_vehicle_phase_approach || vehicle->phase == _bot_vehicle_phase_board);
}

boolean bot_vehicle_drives(
	struct bot const *bot,
	long vehicle_index,
	short team)
{
	struct bot_vehicle const *vehicle = &bot->brain.vehicle;

	return vehicle->vehicle_index == vehicle_index && vehicle->role == _bot_vehicle_role_driver &&
		vehicle->phase != _bot_vehicle_phase_none && vehicle->phase != _bot_vehicle_phase_flip &&
		vehicle->phase != _bot_vehicle_phase_leave && (team == NONE || (bot->brain.unit_index != NONE &&
		!game_team_is_enemy(team, object_get(bot->brain.unit_index)->object.owner_team_index)));
}

boolean bot_vehicle_holds(
	struct bot const *bot,
	long vehicle_index)
{
	return bot->brain.vehicle.phase != _bot_vehicle_phase_none && bot->brain.vehicle.vehicle_index == vehicle_index;
}

boolean bot_vehicle_busy(
	struct bot const *bot)
{
	return bot->brain.vehicle.phase != _bot_vehicle_phase_none;
}

boolean bot_vehicle_riding(
	struct bot const *bot)
{
	return bot->brain.vehicle.phase == _bot_vehicle_phase_ride || bot->brain.vehicle.phase == _bot_vehicle_phase_leave;
}
