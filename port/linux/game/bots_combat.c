/* bots_combat.c: what a bot sees, and how it fights (bots_internal.h)

Seeing: a few times a second (each bot on its own ticks), every player of
the other team (anyone, without teams) whose unit is alive: in front of the
bot (its field of view), near enough, and with nothing of the level or its
scenery between the bot's eyes and the target's head or middle (the game's
own line of sight test, which sees through two-sided surfaces: fences,
grates). No bot sees through walls. A player who hurt it in the last second
it knows of too, wherever they are, and turns to them. It remembers where it
last saw its target for a few seconds, and goes there.

Fighting, a person's way:
- It waits its reaction time after first seeing a target before it fires.
- Its aim turns toward the target at most so fast, and is off by an error
  that wanders and shrinks as it keeps the target in view, and grows when it
  is hit; it leads a target by the time its weapon's projectile takes to get
  there.
- It fires only when its aim is near enough the target, in bursts.
- It strafes while it fights (where the walking graph has floor beside it),
  closes on a far target, backs off a near one, and strikes one at hand.
- It reloads when its magazine is empty, or low with no target in view.
All of it set by the difficulty (bots_difficulty). Grenades and choosing a
weapon by the range are for later. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/players.h"
#include "items/projectile_definitions.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "bots_internal.h"

#include <math.h>

/* ---------- difficulty */

struct bot_skill
{
	/* the ticks from first seeing a target to firing at it: mean and spread */
	real reaction_ticks;
	real reaction_spread;
	/* how fast its aim turns (radians a tick) */
	real turn_rate;
	/* its aim's error (radians, its spread) and how far it sees */
	real aim_error;
	real field_of_view;
	real sight_range;
	/* how far off it may aim and still fire (times the target's own width
	as seen) */
	real fire_cone;
};

static struct bot_skill const bot_skills[NUMBER_OF_BOT_DIFFICULTIES] =
{
	/* peaceful (it never fights: bot_combat_engaged) */ { 18.0f, 4.5f, 0.07f, 0.10f, 1.57f, 25.0f, 1.6f },
	/* easy */ { 18.0f, 4.5f, 0.07f, 0.10f, 1.57f, 25.0f, 1.6f },
	/* normal */ { 12.0f, 3.0f, 0.12f, 0.06f, 1.92f, 35.0f, 1.3f },
	/* hard */ { 7.5f, 1.8f, 0.17f, 0.035f, 2.09f, 45.0f, 1.0f },
	/* legendary */ { 5.0f, 1.2f, 0.24f, 0.018f, 2.27f, 55.0f, 0.8f },
};

/* the ticks between looks, how long a target is remembered, and how long one
that hurt it is known of */
#define BOT_SIGHT_TICKS 3
#define BOT_MEMORY_TICKS (4 * TICKS_PER_SECOND)
#define BOT_HURT_TICKS TICKS_PER_SECOND
/* a burst: held, then let go (ticks) */
#define BOT_BURST_TICKS 9
#define BOT_BURST_PAUSE_TICKS 5
/* the range it keeps from a target, and at hand */
#define BOT_NEAR_RANGE 4.0f
#define BOT_FAR_RANGE 12.0f
#define BOT_MELEE_RANGE 1.1f

/* ---------- small things */

static real bot_gaussian(
	struct bot_brain *brain)
{
	/* (twelve uniforms: near enough a normal distribution) */
	real sum = 0.0f;
	short index;

	for (index = 0; index < 12; index++)
		sum += bot_random_real(brain);
	return sum - 6.0f;
}

static real bot_angle_difference(
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

/* the player datum of a unit (NONE if no player's) */
static long bot_unit_player(
	long unit_index)
{
	struct unit_datum *unit = unit_index != NONE ? (struct unit_datum *)object_try_and_get(unit_index) : NULL;

	return unit ? unit->unit.player_index : NONE;
}

/* whether nothing of the level or its scenery is between two points (the
game's line of sight test: two-sided surfaces are seen through) */
static boolean bot_line_clear(
	real_point3d const *from,
	real_point3d const *to,
	long ignore_object_index)
{
	struct collision_result collision;
	real_vector3d vector;

	vector.i = to->x - from->x;
	vector.j = to->y - from->y;
	vector.k = to->z - from->z;
	return !collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_ignore_two_sided_surfaces_bit) | FLAG(_collision_test_structure_bit) |
		FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) |
		FLAG(_collision_test_objects_machines_bit) | FLAG(_collision_test_objects_vehicles_bit),
		from, &vector, ignore_object_index, &collision);
}

/* the projectile speed of the unit's weapon (world units a tick; 0 for an
instant one, or none) */
static real bot_projectile_speed(
	struct unit_datum *unit)
{
	long weapon_index = unit->unit.current_weapon_index != NONE ?
		unit->unit.weapon_object_indices[unit->unit.current_weapon_index] : NONE;
	struct weapon_definition *weapon;

	if (weapon_index == NONE)
		return 0.0f;
	weapon = weapon_definition_get(object_get(weapon_index)->definition_index);
	if (weapon->weapon.triggers.count > 0)
	{
		struct weapon_trigger_definition *trigger =
			TAG_BLOCK_GET_ELEMENT(&weapon->weapon.triggers, 0, struct weapon_trigger_definition);

		if (trigger->projectile.index != NONE)
		{
			struct projectile_definition *projectile = projectile_definition_get(trigger->projectile.index);

			return MAX(projectile->projectile.initial_velocity, 0.0f);
		}
	}
	return 0.0f;
}

/* ---------- seeing */

/* whether the bot sees the target's unit now */
static boolean bot_sees(
	struct bot const *bot,
	struct unit_datum *unit,
	long unit_index,
	real_point3d const *eye,
	long target_unit_index)
{
	struct bot_skill const *skill = &bot_skills[bot->difficulty];
	struct object_datum *target = object_get(target_unit_index);
	real_point3d head;
	real dx = target->object.bounding_sphere_center.x - eye->x;
	real dy = target->object.bounding_sphere_center.y - eye->y;
	real dz = target->object.bounding_sphere_center.z - eye->z;
	real distance = (real)sqrt(dx * dx + dy * dy + dz * dz);
	real facing = (real)atan2(unit->unit.aiming_vector.j, unit->unit.aiming_vector.i);
	real range = skill->sight_range;

	/* (camouflage: seen only near) */
	if (TEST_FLAG(((struct unit_datum *)target)->unit.flags, _unit_active_camouflaged_bit))
		range *= 0.25f;
	if (distance > range)
		return FALSE;
	if (distance > 1.5f && fabs(bot_angle_difference(facing, (real)atan2(dy, dx))) > skill->field_of_view * 0.5f)
		return FALSE;
	unit_get_head_position(target_unit_index, &head);
	return bot_line_clear(eye, &head, unit_index) ||
		bot_line_clear(eye, &target->object.bounding_sphere_center, unit_index);
}

/* the target a bot fights: the nearest enemy it sees (its target so far,
unless another is much nearer), else one that hurt it just now, else the one
it remembers */
static void bot_perceive(
	struct bot *bot,
	long player_index,
	long unit_index)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_combat *combat = &brain->combat;
	struct unit_datum *unit = unit_get(unit_index);
	struct data_iterator iterator;
	struct player_datum *other;
	real_point3d eye;
	long now = game_time_get();
	long best_player = NONE;
	real best_distance = 1.0e30f;
	short attacker;

	unit_get_camera_position(unit_index, &eye);
	data_iterator_new(&iterator, player_data);
	while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct object_datum *target;
		real dx, dy, dz, distance;

		if (iterator.datum_index == player_index || other->unit_index == NONE || other->quit_out_of_game)
			continue;
		target = object_get(other->unit_index);
		if (TEST_FLAG(target->object.damage_flags, _object_dead_bit) ||
			!game_team_is_enemy(unit->object.owner_team_index, target->object.owner_team_index))
		{
			continue;
		}
		dx = target->object.position.x - eye.x;
		dy = target->object.position.y - eye.y;
		dz = target->object.position.z - eye.z;
		distance = (real)sqrt(dx * dx + dy * dy + dz * dz);
		/* (the target so far kept, against one not much nearer) */
		if (iterator.datum_index == combat->target_player)
			distance *= 0.7f;
		if (distance < best_distance && bot_sees(bot, unit, unit_index, &eye, other->unit_index))
		{
			best_distance = distance;
			best_player = iterator.datum_index;
		}
	}
	/* (one that hurt it, and it sees nobody: turned to) */
	if (best_player == NONE)
	{
		for (attacker = 0; attacker < MAXIMUM_ATTACKERS_PER_UNIT; attacker++)
		{
			struct unit_attacker const *hurt = &unit->unit.attackers[attacker];
			struct player_datum *hurter = hurt->player_index != NONE ? player_try_and_get(hurt->player_index) : NULL;

			if (hurter && hurt->player_index != player_index && hurter->unit_index != NONE &&
				(long)hurt->game_time_stamp + BOT_HURT_TICKS >= now && (long)hurt->game_time_stamp <= now)
			{
				combat->target_player = hurt->player_index;
				combat->target_visible = FALSE;
				combat->last_seen_tick = now;
				combat->last_seen_position = object_get(hurter->unit_index)->object.position;
				/* (hurt: its aim shaken) */
				combat->error_scale = 1.5f;
				return;
			}
		}
	}
	if (best_player == NONE)
	{
		combat->target_visible = FALSE;
		if (combat->target_player != NONE && now - combat->last_seen_tick > BOT_MEMORY_TICKS)
			combat->target_player = NONE;
		return;
	}
	if (best_player != combat->target_player || !combat->target_visible)
	{
		struct bot_skill const *skill = &bot_skills[bot->difficulty];

		/* (newly seen: its reaction time before it fires, its aim far off) */
		if (best_player != combat->target_player || now - combat->last_seen_tick > TICKS_PER_SECOND)
		{
			combat->fire_after_tick = now + (long)MAX(1.0f, skill->reaction_ticks + bot_gaussian(brain) * skill->reaction_spread);
			combat->error_scale = 1.0f;
			brain->statistics.targets_seen++;
		}
		combat->target_player = best_player;
	}
	combat->target_visible = TRUE;
	combat->last_seen_tick = now;
	combat->last_seen_position = object_get(player_get(best_player)->unit_index)->object.position;
}

/* ---------- fighting */

void bot_combat_reset(
	struct bot_combat *combat)
{
	csmemset(combat, 0, sizeof(*combat));
	combat->target_player = NONE;
	combat->error_scale = 1.0f;
}

boolean bot_combat_engaged(
	struct bot *bot,
	long player_index,
	long unit_index,
	long bot_slot)
{
	struct bot_combat *combat = &bot->brain.combat;

	/* (a peaceful bot sees nobody to fight: it never aims at, shoots or
	strikes a player) */
	if (bot->difficulty == _bot_difficulty_peaceful)
	{
		combat->target_player = NONE;
		combat->target_visible = FALSE;
		return FALSE;
	}
	if ((game_time_get() + bot_slot) % BOT_SIGHT_TICKS == 0)
		bot_perceive(bot, player_index, unit_index);
	if (combat->target_player != NONE)
	{
		struct player_datum *target = player_try_and_get(combat->target_player);

		if (!target || target->unit_index == NONE)
		{
			combat->target_player = NONE;
			combat->target_visible = FALSE;
		}
	}
	return combat->target_player != NONE;
}

/* where a bot heads, fighting: where it last saw its target (its goal on the
walking graph), unless it sees it near enough */
struct bots_nav_point const *bot_combat_goal(
	struct bot *bot,
	real_point3d const *position,
	struct bots_nav_point *goal)
{
	struct bot_combat const *combat = &bot->brain.combat;
	real dx = combat->last_seen_position.x - position->x;
	real dy = combat->last_seen_position.y - position->y;

	if (combat->target_visible && dx * dx + dy * dy < BOT_FAR_RANGE * BOT_FAR_RANGE)
		return NULL;
	goal->x = combat->last_seen_position.x;
	goal->y = combat->last_seen_position.y;
	goal->z = combat->last_seen_position.z;
	return goal;
}

void bot_combat_act(
	struct bot *bot,
	long unit_index,
	struct player_action *action,
	boolean moving)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_combat *combat = &brain->combat;
	struct bot_skill const *skill = &bot_skills[bot->difficulty];
	struct unit_datum *unit = unit_get(unit_index);
	struct player_datum *target_player = player_try_and_get(combat->target_player);
	struct object_datum *target;
	struct bots_nav *nav = bots_nav_current();
	real_point3d eye, aim_point;
	real dx, dy, dz, distance, horizontal, yaw, pitch, turn, cone, aim_off;
	real move_yaw, move_amount;
	long now = game_time_get();
	long weapon_index;

	if (!target_player || target_player->unit_index == NONE)
		return;
	target = object_get(target_player->unit_index);
	unit_get_camera_position(unit_index, &eye);
	/* (where it aims: the target's middle, led by the projectile's flight;
	one not in view, where it was last seen) */
	if (combat->target_visible)
	{
		real speed = bot_projectile_speed(unit);

		aim_point = target->object.bounding_sphere_center;
		if (speed > 0.01f)
		{
			real ticks;

			dx = aim_point.x - eye.x;
			dy = aim_point.y - eye.y;
			dz = aim_point.z - eye.z;
			ticks = MIN((real)sqrt(dx * dx + dy * dy + dz * dz) / speed, 2.0f * TICKS_PER_SECOND);
			aim_point.x += target->object.translational_velocity.i * ticks;
			aim_point.y += target->object.translational_velocity.j * ticks;
			aim_point.z += target->object.translational_velocity.k * ticks * 0.5f;
		}
	}
	else
	{
		aim_point = combat->last_seen_position;
		aim_point.z += 0.5f;
	}
	dx = aim_point.x - eye.x;
	dy = aim_point.y - eye.y;
	dz = aim_point.z - eye.z;
	horizontal = (real)sqrt(dx * dx + dy * dy);
	distance = (real)sqrt(dx * dx + dy * dy + dz * dz);
	yaw = (real)atan2(dy, dx);
	pitch = (real)atan2(dz, MAX(horizontal, 0.01f));

	/* (its error: wandering, shrinking while it keeps the target in view) */
	combat->error_yaw = combat->error_yaw * 0.9f + bot_gaussian(brain) * skill->aim_error * 0.25f * combat->error_scale;
	combat->error_pitch = combat->error_pitch * 0.9f + bot_gaussian(brain) * skill->aim_error * 0.15f * combat->error_scale;
	if (combat->target_visible)
		combat->error_scale = MAX(0.4f, combat->error_scale * 0.985f);
	yaw += combat->error_yaw;
	pitch += combat->error_pitch;

	/* (its aim turns at most so fast) */
	turn = bot_angle_difference(combat->aim_yaw, yaw);
	if (!combat->aim_set)
	{
		combat->aim_yaw = brain->yaw;
		combat->aim_pitch = brain->pitch;
		combat->aim_set = TRUE;
		turn = bot_angle_difference(combat->aim_yaw, yaw);
	}
	combat->aim_yaw += PIN(turn, -skill->turn_rate, skill->turn_rate);
	combat->aim_pitch += PIN(pitch - combat->aim_pitch, -skill->turn_rate, skill->turn_rate);

	/* (its body's way, as the walking set it, kept; facing its aim) */
	move_yaw = brain->yaw + (real)atan2(action->throttle.j, action->throttle.i);
	move_amount = MIN(1.0f, (real)sqrt(action->throttle.i * action->throttle.i + action->throttle.j * action->throttle.j));
	if (combat->target_visible && !moving)
	{
		/* strafing, closing on a far target, backing off a near one */
		real side, along;

		if (now >= combat->strafe_until_tick)
		{
			combat->strafe_direction = bot_random_real(brain) < 0.5f ? -1.0f : 1.0f;
			combat->strafe_until_tick = now + 12 + (long)(bot_random_real(brain) * 24.0f);
		}
		side = combat->strafe_direction;
		along = distance > BOT_FAR_RANGE * 0.7f ? 1.0f : distance < BOT_NEAR_RANGE ? -0.6f : 0.0f;
		/* (only onto floor: the walking graph beside it) */
		if (nav)
		{
			real step_yaw = yaw + side * _pi * 0.5f;
			real_point3d *position = &unit->object.position;

			if (bots_nav_locate(nav, position->x + (real)cos(step_yaw) * 1.0f, position->y + (real)sin(step_yaw) * 1.0f,
				position->z) == NONE)
			{
				side = -side;
				combat->strafe_direction = side;
				step_yaw = yaw + side * _pi * 0.5f;
				if (bots_nav_locate(nav, position->x + (real)cos(step_yaw) * 1.0f, position->y + (real)sin(step_yaw) * 1.0f,
					position->z) == NONE)
				{
					side = 0.0f;
				}
			}
		}
		move_yaw = yaw + (real)atan2(side, along);
		move_amount = side != 0.0f || along != 0.0f ? 1.0f : 0.0f;
	}
	action->desired_facing.yaw = (real)fmod(combat->aim_yaw + 4.0f * _pi, 2.0f * _pi);
	if (action->desired_facing.yaw < 0.0f || action->desired_facing.yaw >= 2.0f * _pi)
		action->desired_facing.yaw = 0.0f;
	action->desired_facing.pitch = PIN(combat->aim_pitch, -DEGREES_TO_RADIANS(BOT_MAXIMUM_PITCH_DEGREES),
		DEGREES_TO_RADIANS(BOT_MAXIMUM_PITCH_DEGREES));
	action->throttle.i = (real)cos(move_yaw - combat->aim_yaw) * move_amount;
	action->throttle.j = (real)sin(move_yaw - combat->aim_yaw) * move_amount;

	/* firing: in view, past its reaction, its aim near enough, in bursts */
	weapon_index = unit->unit.current_weapon_index != NONE ?
		unit->unit.weapon_object_indices[unit->unit.current_weapon_index] : NONE;
	cone = (real)atan2(0.35f, MAX(distance, 0.5f)) * skill->fire_cone;
	aim_off = (real)fabs(bot_angle_difference(combat->aim_yaw, (real)atan2(dy, dx))) +
		(real)fabs(combat->aim_pitch - (real)atan2(dz, MAX(horizontal, 0.01f)));
	if (combat->target_visible && now >= combat->fire_after_tick && aim_off < cone && weapon_index != NONE)
	{
		if (combat->burst_ticks < BOT_BURST_TICKS)
		{
			action->primary_trigger = 1.0f;
			SET_FLAG(action->control_flags, _unit_control_weapon_primary_trigger_bit, TRUE);
			brain->statistics.ticks_fired++;
		}
		if (++combat->burst_ticks >= BOT_BURST_TICKS + BOT_BURST_PAUSE_TICKS)
			combat->burst_ticks = 0;
	}
	else
	{
		combat->burst_ticks = 0;
	}
	/* (a target at hand, faced: struck) */
	if (combat->target_visible && distance < BOT_MELEE_RANGE + 0.5f && aim_off < 0.5f &&
		now >= combat->melee_after_tick)
	{
		SET_FLAG(action->control_flags, _unit_control_use_equipment_bit, TRUE);
		combat->melee_after_tick = now + TICKS_PER_SECOND;
	}
	/* (an empty magazine reloaded) */
	if (weapon_index != NONE)
	{
		struct weapon_datum *weapon = weapon_get(weapon_index);

		if (weapon->weapon.magazines[0].rounds_loaded == 0 && weapon->weapon.magazines[0].rounds_total > 0 &&
			now >= combat->reload_after_tick)
		{
			SET_FLAG(action->control_flags, _unit_control_weapon_reload_bit, TRUE);
			combat->reload_after_tick = now + TICKS_PER_SECOND;
		}
	}
}

/* reloading a magazine below a third when it fights nobody */
void bot_combat_idle(
	struct bot *bot,
	long unit_index,
	struct player_action *action)
{
	struct bot_combat *combat = &bot->brain.combat;
	struct unit_datum *unit = unit_get(unit_index);
	long weapon_index = unit->unit.current_weapon_index != NONE ?
		unit->unit.weapon_object_indices[unit->unit.current_weapon_index] : NONE;
	long now = game_time_get();

	combat->aim_set = FALSE;
	if (weapon_index != NONE && now >= combat->reload_after_tick)
	{
		struct weapon_datum *weapon = weapon_get(weapon_index);
		struct weapon_definition *definition = weapon_definition_get(weapon->definition_index);

		if (definition->weapon.magazines.count > 0 && weapon->weapon.magazines[0].rounds_total > 0)
		{
			struct weapon_magazine_definition *magazine =
				TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, 0, struct weapon_magazine_definition);

			if (weapon->weapon.magazines[0].rounds_loaded * 3 < magazine->rounds_loaded_maximum)
			{
				SET_FLAG(action->control_flags, _unit_control_weapon_reload_bit, TRUE);
				combat->reload_after_tick = now + 2 * TICKS_PER_SECOND;
			}
		}
	}
}
