/* bots_grenades.c: a bot's grenades (bots_internal.h)

A bot that fights thinks of a grenade now and then (bot_skill_grenade_chance
at each look, at least bot_skill_grenade_cooldown apart, more often at a
group, or at an enemy who went behind cover), never a peaceful one:
- at the target it sees, where it will be when the grenade lands, or at
  where it last saw one that went out of sight (around a corner, behind a
  wall: a lob over it);
- plasma at one in view (it sticks), a frag grenade at one out of sight or a
  group (it bounces);
- aimed as the game throws a player's grenade (units.c's
  unit_throw_grenade_release: from the camera and the globals' grenade
  origin, at the unit's grenade velocity along the aim, falling at the
  grenade projectile's gravity), the arc's lower angle, else the higher;
- only when the flight, followed through the level as the game would, lands
  near the point, far enough from the bot itself and from every team mate
  (the projectile's danger radius and a margin): no suicides, no team kills.
The throw: the grenade chosen (desired_grenade_index), the aim turned onto
the arc (no faster than a person turns) with the difficulty's error, the
button pressed once the unit's own aim is there, and the aim held until the
grenade has left the hand. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/game_engine.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "items/projectile_definitions.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "units/unit_definitions.h"
#include "units/units.h"

#include "bots_internal.h"

#include <math.h>

/* physics.c's */
extern real global_gravity;
/* the platform layer's (port_config.c, the port's log) */
int config_boolean(const char *name);
void platform_log(const char *format, ...);

/* the nearest and furthest a bot throws at (world units) */
#define BOT_GRENADE_RANGE_MINIMUM 7.0f
#define BOT_GRENADE_RANGE_MAXIMUM 28.0f
/* how near the point the grenade must land (a frag grenade rolls on a
little), and the margin beyond its danger radius it keeps from the bot and
its team mates */
#define BOT_GRENADE_LAND_NEAR 3.0f
#define BOT_GRENADE_SAFETY 1.0f
/* the least radius a grenade's blast is taken to hurt within */
#define BOT_GRENADE_DANGER_RADIUS 4.5f
/* how long a target out of sight is still thrown at (ticks), and how long a
throw may take before it is given up */
#define BOT_GRENADE_MEMORY_TICKS (3 * TICKS_PER_SECOND)
#define BOT_GRENADE_THROW_TICKS (2 * TICKS_PER_SECOND)
/* the flight followed: each step (ticks), and the longest */
#define BOT_GRENADE_STEP_TICKS 2
#define BOT_GRENADE_FLIGHT_TICKS 120
/* the margin beyond that kept from team mates, who move, and how far ahead
their moves are followed (ticks); how long a bot comes no nearer its target
after a throw (it does not walk into its own grenade) */
#define BOT_GRENADE_MATE_MARGIN 1.5f
#define BOT_GRENADE_MATE_TICKS (1.5f * TICKS_PER_SECOND)
#define BOT_GRENADE_HOLD_TICKS (5 * TICKS_PER_SECOND / 2)
/* the time a grenade takes to go off after it lands, about (ticks) */
#define BOT_GRENADE_FUSE_TICKS 15
/* the enemies near the point that make a group, and how near */
#define BOT_GRENADE_GROUP_RADIUS 4.0f

/* ---------- the game's throw */

/* the grenade projectile of a kind (NULL if the map has none) */
static struct projectile_definition *bot_grenade_projectile(
	short type)
{
	struct game_globals *globals = scenario_get_game_globals();
	struct game_globals_grenade *grenade;

	if (!globals || type < 0 || type >= globals->grenades.count)
		return NULL;
	grenade = TAG_BLOCK_GET_ELEMENT(&globals->grenades, type, struct game_globals_grenade);
	return grenade->projectile.index != NONE ? projectile_definition_get(grenade->projectile.index) : NULL;
}

real bot_grenade_danger_radius(
	short type)
{
	struct projectile_definition *projectile = bot_grenade_projectile(type);

	/* (the tag's danger radius is the campaign's warning to its AI, 2.25
	world units for a frag grenade: the blast reaches further) */
	return MAX(projectile ? projectile->projectile.danger_radius : 0.0f, BOT_GRENADE_DANGER_RADIUS);
}

/* where the grenade leaves the hand thrown with an aim, and its velocity
(world units a tick), as units.c's unit_throw_grenade_release has them */
static void bot_grenade_launch(
	long unit_index,
	real yaw,
	real pitch,
	real_point3d *origin,
	real_vector3d *velocity)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct unit_definition *definition = unit_definition_get(unit->definition_index);
	struct game_globals *globals = scenario_get_game_globals();
	real_vector3d forward, up, left;
	real speed = definition->unit.grenade_velocity / TICKS_PER_SECOND;

	forward.i = (real)(cos(yaw) * cos(pitch));
	forward.j = (real)(sin(yaw) * cos(pitch));
	forward.k = (real)sin(pitch);
	/* (as the game makes them: its "up" is global up across the aim, which
	is level and to the left, and its "left" the aim across that, which is
	up from the aim) */
	up.i = -(real)sin(yaw);
	up.j = (real)cos(yaw);
	up.k = 0.0f;
	left.i = -forward.k * (real)cos(yaw);
	left.j = -forward.k * (real)sin(yaw);
	left.k = (real)cos(pitch);
	unit_get_camera_position(unit_index, origin);
	if (globals && globals->player_information.count > 0)
	{
		struct game_globals_player_information *information =
			TAG_BLOCK_GET_ELEMENT(&globals->player_information, 0, struct game_globals_player_information);

		origin->x += forward.i * information->grenade_origin.i + up.i * information->grenade_origin.j +
			left.i * information->grenade_origin.k;
		origin->y += forward.j * information->grenade_origin.i + up.j * information->grenade_origin.j +
			left.j * information->grenade_origin.k;
		origin->z += forward.k * information->grenade_origin.i + up.k * information->grenade_origin.j +
			left.k * information->grenade_origin.k;
	}
	velocity->i = forward.i * speed;
	velocity->j = forward.j * speed;
	velocity->k = forward.k * speed;
}

/* the grenade's flight with the aim, through the level, its scenery and
its objects (the thrower's unit not): where it first hits; FALSE if it is
still flying at the end */
static boolean bot_grenade_flight(
	long unit_index,
	short type,
	real yaw,
	real pitch,
	real_point3d *impact,
	real *impact_up)
{
	struct projectile_definition *projectile = bot_grenade_projectile(type);
	real gravity = global_gravity * (projectile ? projectile->projectile.air_gravity_scale : 1.0f);
	real_point3d position;
	real_vector3d velocity;
	long ticks;

	bot_grenade_launch(unit_index, yaw, pitch, &position, &velocity);
	for (ticks = 0; ticks < BOT_GRENADE_FLIGHT_TICKS; ticks += BOT_GRENADE_STEP_TICKS)
	{
		real step = (real)BOT_GRENADE_STEP_TICKS;
		real_vector3d move;
		struct collision_result collision;

		move.i = velocity.i * step;
		move.j = velocity.j * step;
		move.k = velocity.k * step - 0.5f * gravity * step * step;
		velocity.k -= gravity * step;
		if (collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit) |
			FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) |
			FLAG(_collision_test_objects_machines_bit) | FLAG(_collision_test_objects_vehicles_bit) |
			FLAG(_collision_test_objects_bipeds_bit), &position, &move, unit_index, &collision))
		{
			*impact = collision.point;
			*impact_up = collision.plane.n.k;
			return TRUE;
		}
		position.x += move.i;
		position.y += move.j;
		position.z += move.k;
	}
	return FALSE;
}

/* the pitches that throw a grenade from the bot to the point, the arc's
lower first (FALSE: out of reach); the camera stands in for the hand */
static boolean bot_grenade_pitches(
	long unit_index,
	short type,
	real_point3d const *point,
	real pitches[2])
{
	struct unit_datum *unit = unit_get(unit_index);
	struct unit_definition *definition = unit_definition_get(unit->definition_index);
	struct projectile_definition *projectile = bot_grenade_projectile(type);
	real gravity = global_gravity * (projectile ? projectile->projectile.air_gravity_scale : 1.0f);
	real speed = definition->unit.grenade_velocity / TICKS_PER_SECOND;
	real_point3d eye;
	real x, z, discriminant, root;

	unit_get_camera_position(unit_index, &eye);
	x = (real)sqrt((point->x - eye.x) * (point->x - eye.x) + (point->y - eye.y) * (point->y - eye.y));
	z = point->z - eye.z;
	if (speed <= 0.0f || x < 0.5f)
		return FALSE;
	if (gravity <= 0.0f)
	{
		pitches[0] = pitches[1] = (real)atan2(z, x);
		return TRUE;
	}
	discriminant = speed * speed * speed * speed - gravity * (gravity * x * x + 2.0f * z * speed * speed);
	if (discriminant < 0.0f)
		return FALSE;
	root = (real)sqrt(discriminant);
	pitches[0] = (real)atan((speed * speed - root) / (gravity * x));
	pitches[1] = (real)atan((speed * speed + root) / (gravity * x));
	return TRUE;
}

/* whether a grenade going off at the point spares the bot and its team
mates (anyone of its team, a person too, alive) */
static boolean bot_grenade_safe(
	long player_index,
	long unit_index,
	short type,
	real_point3d const *impact)
{
	struct unit_datum *unit = unit_get(unit_index);
	real keep = bot_grenade_danger_radius(type) + BOT_GRENADE_SAFETY;
	struct data_iterator iterator;
	struct player_datum *other;
	real dx = unit->object.position.x - impact->x;
	real dy = unit->object.position.y - impact->y;
	real dz = unit->object.position.z - impact->z;

	if (dx * dx + dy * dy + dz * dz < keep * keep)
		return FALSE;
	data_iterator_new(&iterator, player_data);
	while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct object_datum *mate;

		if (iterator.datum_index == player_index || other->unit_index == NONE)
			continue;
		mate = object_get(other->unit_index);
		if (TEST_FLAG(mate->object.damage_flags, _object_dead_bit) ||
			game_team_is_enemy(unit->object.owner_team_index, mate->object.owner_team_index))
		{
			continue;
		}
		/* (where they are, and where they will be when it goes off: a
		wider margin, they move as they please) */
		dx = mate->object.position.x - impact->x;
		dy = mate->object.position.y - impact->y;
		dz = mate->object.position.z - impact->z;
		if (dx * dx + dy * dy + dz * dz < (keep + BOT_GRENADE_MATE_MARGIN) * (keep + BOT_GRENADE_MATE_MARGIN))
			return FALSE;
		dx += mate->object.translational_velocity.i * BOT_GRENADE_MATE_TICKS;
		dy += mate->object.translational_velocity.j * BOT_GRENADE_MATE_TICKS;
		if (dx * dx + dy * dy < (keep + BOT_GRENADE_MATE_MARGIN) * (keep + BOT_GRENADE_MATE_MARGIN))
			return FALSE;
	}
	return TRUE;
}

/* how many of the bot's enemies, alive, are near the point */
static long bot_grenade_group(
	long player_index,
	long unit_index,
	real_point3d const *point)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct data_iterator iterator;
	struct player_datum *other;
	long count = 0;

	data_iterator_new(&iterator, player_data);
	while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct object_datum *enemy;
		real dx, dy, dz;

		if (iterator.datum_index == player_index || other->unit_index == NONE)
			continue;
		enemy = object_get(other->unit_index);
		if (TEST_FLAG(enemy->object.damage_flags, _object_dead_bit) ||
			!game_team_is_enemy(unit->object.owner_team_index, enemy->object.owner_team_index))
		{
			continue;
		}
		dx = enemy->object.position.x - point->x;
		dy = enemy->object.position.y - point->y;
		dz = enemy->object.position.z - point->z;
		if (dx * dx + dy * dy + dz * dz < BOT_GRENADE_GROUP_RADIUS * BOT_GRENADE_GROUP_RADIUS)
			count++;
	}
	return count;
}

/* ---------- deciding */

/* a throw at the point that lands near it and safely: its aim (FALSE: none) */
static boolean bot_grenade_plan(
	struct bot *bot,
	long player_index,
	long unit_index,
	short type,
	real_point3d const *point,
	real *yaw,
	real *pitch)
{
	real_point3d eye;
	real pitches[2];
	short arc;

	unit_get_camera_position(unit_index, &eye);
	if (!bot_grenade_pitches(unit_index, type, point, pitches))
		return FALSE;
	*yaw = (real)atan2(point->y - eye.y, point->x - eye.x);
	for (arc = 0; arc < 2; arc++)
	{
		real_point3d impact;
		real dx, dy, dz, up;

		/* (no throw at the feet, nor straight up) */
		if (pitches[arc] > DEGREES_TO_RADIANS(BOT_MAXIMUM_PITCH_DEGREES - 10.0f) ||
			pitches[arc] < -DEGREES_TO_RADIANS(BOT_MAXIMUM_PITCH_DEGREES - 10.0f))
		{
			continue;
		}
		if (!bot_grenade_flight(unit_index, type, *yaw, pitches[arc], &impact, &up))
			continue;
		dx = impact.x - point->x;
		dy = impact.y - point->y;
		dz = impact.z - point->z;
		if (dx * dx + dy * dy + dz * dz > BOT_GRENADE_LAND_NEAR * BOT_GRENADE_LAND_NEAR ||
			!bot_grenade_safe(player_index, unit_index, type, &impact))
		{
			continue;
		}
		/* (a frag grenade that strikes a wall bounces back: only from well
		away; a plasma grenade sticks where it strikes) */
		dx = impact.x - eye.x;
		dy = impact.y - eye.y;
		if (type == _unit_grenade_human_fragmentation && up < 0.6f &&
			dx * dx + dy * dy < 4.0f * bot_grenade_danger_radius(type) * bot_grenade_danger_radius(type))
		{
			continue;
		}
		*pitch = pitches[arc];
		(void)bot;
		return TRUE;
	}
	return FALSE;
}

/* whether the bot begins a throw now (at a look, its grenades ready) */
static boolean bot_grenade_decide(
	struct bot *bot,
	long unit_index)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_combat *combat = &brain->combat;
	struct unit_datum *unit = unit_get(unit_index);
	struct player_datum *target_player = player_try_and_get(combat->target_player);
	long player_index = unit->unit.player_index;
	long now = game_time_get();
	real chance = bot_skill_grenade_chance(bot);
	real_point3d point, eye;
	real distance;
	short type = NONE;
	long group;
	real yaw, pitch;

	if (chance <= 0.0f || now < combat->grenade_after_tick || !target_player || target_player->unit_index == NONE ||
		unit->unit.grenade_throw_state != _unit_grenade_throw_idle || unit->object.parent_object_index != NONE)
	{
		return FALSE;
	}
	if (unit->unit.grenade_counts[_unit_grenade_human_fragmentation] <= 0 &&
		unit->unit.grenade_counts[_unit_grenade_covenant_plasma] <= 0)
	{
		return FALSE;
	}
	/* (where: the target where it will be, or where it went out of sight) */
	if (combat->target_visible)
	{
		struct object_datum *target = object_get(target_player->unit_index);

		struct unit_definition *definition = unit_definition_get(unit->definition_index);
		real speed = definition->unit.grenade_velocity / TICKS_PER_SECOND;
		real ticks;

		point = target->object.position;
		/* (led by the flight, its speed along the ground a little less than
		the throw's, and the fuse after it lands) */
		unit_get_camera_position(unit_index, &eye);
		ticks = (real)sqrt((point.x - eye.x) * (point.x - eye.x) + (point.y - eye.y) * (point.y - eye.y));
		ticks = speed > 0.01f ? ticks / (speed * 0.85f) : (real)TICKS_PER_SECOND;
		ticks = MIN(ticks + BOT_GRENADE_FUSE_TICKS, 2.5f * TICKS_PER_SECOND);
		point.x += target->object.translational_velocity.i * ticks;
		point.y += target->object.translational_velocity.j * ticks;
	}
	else if (now - combat->last_seen_tick <= BOT_GRENADE_MEMORY_TICKS)
	{
		point = combat->last_seen_position;
	}
	else
	{
		return FALSE;
	}
	unit_get_camera_position(unit_index, &eye);
	distance = (real)sqrt((point.x - eye.x) * (point.x - eye.x) + (point.y - eye.y) * (point.y - eye.y));
	if (distance < BOT_GRENADE_RANGE_MINIMUM || distance > BOT_GRENADE_RANGE_MAXIMUM)
		return FALSE;
	/* (more likely at a group, or at one hiding) */
	group = bot_grenade_group(player_index, unit_index, &point);
	if (group > 1)
		chance *= 1.0f + (real)(group - 1) * 1.5f;
	if (!combat->target_visible)
		chance *= 1.5f;
	if (bot_random_real(brain) >= chance)
		return FALSE;
	/* (plasma at one in view that is not one of a group, else a frag grenade) */
	if (combat->target_visible && group <= 1 && unit->unit.grenade_counts[_unit_grenade_covenant_plasma] > 0)
		type = _unit_grenade_covenant_plasma;
	else if (unit->unit.grenade_counts[_unit_grenade_human_fragmentation] > 0)
		type = _unit_grenade_human_fragmentation;
	else
		type = _unit_grenade_covenant_plasma;
	/* (where it aims first, else a little short of it, else, for one in
	view, where the target is now) */
	if (!bot_grenade_plan(bot, player_index, unit_index, type, &point, &yaw, &pitch))
	{
		real_point3d short_of = point;
		real length = (real)sqrt((point.x - eye.x) * (point.x - eye.x) + (point.y - eye.y) * (point.y - eye.y));

		short_of.x -= (point.x - eye.x) / MAX(length, 0.1f) * 1.5f;
		short_of.y -= (point.y - eye.y) / MAX(length, 0.1f) * 1.5f;
		if (bot_grenade_plan(bot, player_index, unit_index, type, &short_of, &yaw, &pitch))
		{
			point = short_of;
		}
		else if (combat->target_visible && bot_grenade_plan(bot, player_index, unit_index, type,
			&object_get(target_player->unit_index)->object.position, &yaw, &pitch))
		{
			point = object_get(target_player->unit_index)->object.position;
		}
		else
		{
			/* (no throw from here: not again for a little) */
			combat->grenade_after_tick = now + TICKS_PER_SECOND;
			return FALSE;
		}
	}
	combat->throwing = TRUE;
	combat->throw_type = type;
	combat->throw_yaw = yaw + bot_gaussian(brain) * bot_skill_grenade_error(bot);
	combat->throw_pitch = pitch + bot_gaussian(brain) * bot_skill_grenade_error(bot) * 0.6f;
	combat->throw_pitch = PIN(combat->throw_pitch, -DEGREES_TO_RADIANS(BOT_MAXIMUM_PITCH_DEGREES),
		DEGREES_TO_RADIANS(BOT_MAXIMUM_PITCH_DEGREES));
	combat->throw_tick = now;
	combat->throw_pressed = FALSE;
	combat->grenade_aimed = point;
	if (config_boolean("debug.bot_trace"))
	{
		real_point3d impact;
		real up;

		bot_grenade_flight(unit_index, type, combat->throw_yaw, combat->throw_pitch, &impact, &up);
		platform_log("bots: grenade %p throws %d from (%.1f %.1f %.1f) at (%.1f %.1f %.1f) %s: lands (%.1f %.1f %.1f) up %.2f",
			(void *)bot, (int)type, unit->object.position.x, unit->object.position.y, unit->object.position.z, point.x, point.y,
			point.z, combat->target_visible ? "seen" : "unseen", impact.x, impact.y, impact.z, up);
	}
	return TRUE;
}

/* ---------- following a grenade (debug.bot_trace) */

void bot_grenade_follow(
	struct bot *bot)
{
	struct bot_combat *combat = &bot->brain.combat;
	struct object_datum *grenade;
	struct player_datum *target = player_try_and_get(combat->target_player);

	if (combat->grenade_object == NONE)
		return;
	grenade = object_try_and_get(combat->grenade_object);
	if (grenade && grenade->object.type == _object_type_projectile)
	{
		combat->grenade_last = grenade->object.position;
		return;
	}
	if (target && target->unit_index != NONE)
	{
		real_point3d const *at = &object_get(target->unit_index)->object.position;

		platform_log("bots: grenade %p went off at (%.1f %.1f %.1f), aimed at (%.1f %.1f %.1f), %.1f from it; its target at (%.1f %.1f %.1f), %.1f from it",
			(void *)bot, combat->grenade_last.x, combat->grenade_last.y, combat->grenade_last.z, combat->grenade_aimed.x,
			combat->grenade_aimed.y, combat->grenade_aimed.z,
			(real)sqrt((combat->grenade_last.x - combat->grenade_aimed.x) * (combat->grenade_last.x - combat->grenade_aimed.x) +
				(combat->grenade_last.y - combat->grenade_aimed.y) * (combat->grenade_last.y - combat->grenade_aimed.y)),
			at->x, at->y, at->z,
			(real)sqrt((combat->grenade_last.x - at->x) * (combat->grenade_last.x - at->x) +
				(combat->grenade_last.y - at->y) * (combat->grenade_last.y - at->y)));
	}
	combat->grenade_object = NONE;
}

/* ---------- throwing */

static real bot_grenade_angle(
	real from,
	real to)
{
	real difference = (real)fmod(to - from, 2.0f * _pi);

	if (difference > _pi)
		difference -= 2.0f * _pi;
	if (difference < -_pi)
		difference += 2.0f * _pi;
	return difference;
}

boolean bot_grenade_act(
	struct bot *bot,
	long unit_index,
	struct player_action *action)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_combat *combat = &brain->combat;
	struct unit_datum *unit = unit_get(unit_index);
	long now = game_time_get();
	real move_yaw, move_amount, turn, aim_yaw, aim_pitch, off;

	if (!combat->throwing)
	{
		if ((now + bot->slot) % 3 != 0 || !bot_grenade_decide(bot, unit_index))
			return FALSE;
	}
	/* (given up: too long, or its grenades gone) */
	if (now - combat->throw_tick > BOT_GRENADE_THROW_TICKS ||
		(!combat->throw_pressed && unit->unit.grenade_counts[combat->throw_type] <= 0))
	{
		combat->throwing = FALSE;
		combat->grenade_after_tick = now + TICKS_PER_SECOND;
		return FALSE;
	}
	/* (thrown: the grenade has left the hand) */
	if (combat->throw_pressed && (unit->unit.grenade_throw_state == _unit_grenade_throw_ending ||
		(unit->unit.grenade_throw_state == _unit_grenade_throw_idle && now - combat->throw_tick > 10)))
	{
		combat->throwing = FALSE;
		combat->grenade_after_tick = now + bot_skill_grenade_cooldown(bot);
		brain->statistics.grenades_thrown++;
		/* (no nearer the target while it goes off; a plasma grenade at one in
		view may stick: away from them, lest they come close with it) */
		combat->hold_until_tick = now + BOT_GRENADE_HOLD_TICKS;
		if (combat->throw_type == _unit_grenade_covenant_plasma)
			combat->back_off_until_tick = now + 3 * TICKS_PER_SECOND;
		return FALSE;
	}
	/* the aim onto the arc, at twice its usual turn (a throw is quick) */
	if (!combat->aim_set)
	{
		combat->aim_yaw = brain->yaw;
		combat->aim_pitch = brain->pitch;
		combat->aim_set = TRUE;
	}
	turn = bot_grenade_angle(combat->aim_yaw, combat->throw_yaw);
	combat->aim_yaw += PIN(turn, -0.35f, 0.35f);
	combat->aim_pitch += PIN(combat->throw_pitch - combat->aim_pitch, -0.35f, 0.35f);
	/* (its moves kept, as the walking set them) */
	move_yaw = brain->yaw + (real)atan2(action->throttle.j, action->throttle.i);
	move_amount = MIN(1.0f, (real)sqrt(action->throttle.i * action->throttle.i + action->throttle.j * action->throttle.j));
	aim_yaw = (real)fmod(combat->aim_yaw + 4.0f * _pi, 2.0f * _pi);
	if (aim_yaw < 0.0f || aim_yaw >= 2.0f * _pi)
		aim_yaw = 0.0f;
	aim_pitch = PIN(combat->aim_pitch, -DEGREES_TO_RADIANS(BOT_MAXIMUM_PITCH_DEGREES),
		DEGREES_TO_RADIANS(BOT_MAXIMUM_PITCH_DEGREES));
	action->desired_facing.yaw = aim_yaw;
	action->desired_facing.pitch = aim_pitch;
	action->throttle.i = (real)cos(move_yaw - aim_yaw) * move_amount;
	action->throttle.j = (real)sin(move_yaw - aim_yaw) * move_amount;
	action->primary_trigger = 0.0f;
	SET_FLAG(action->control_flags, _unit_control_weapon_primary_trigger_bit, FALSE);
	action->desired_grenade_index = combat->throw_type;
	/* the button, once its grenade is chosen and the unit aims along the arc
	(held until the throw begins: a press is a tick or two) */
	off = (real)fabs(bot_grenade_angle((real)atan2(unit->unit.aiming_vector.j, unit->unit.aiming_vector.i), combat->throw_yaw)) +
		(real)fabs((real)asin(PIN(unit->unit.aiming_vector.k, -1.0f, 1.0f)) - combat->throw_pitch);
	if (!combat->throw_pressed && unit->unit.current_grenade_index == combat->throw_type && off < 0.04f)
	{
		combat->throw_pressed = TRUE;
		combat->throw_tick = now;
	}
	if (unit->unit.grenade_object_index != NONE)
		combat->grenade_object = unit->unit.grenade_object_index;
	if (combat->throw_pressed && unit->unit.grenade_throw_state == _unit_grenade_throw_idle && now - combat->throw_tick < 4)
		SET_FLAG(action->control_flags, _unit_control_throw_grenade_bit, TRUE);
	return TRUE;
}
