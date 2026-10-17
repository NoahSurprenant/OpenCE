/* bots_hearing.c: what a bot hears, and the grenades it runs from
(bots_internal.h)

Hearing, when a bot sees nobody to fight (bot_hear, at its looks):
- an enemy firing (the weapon's last shot, weapons.c's
  game_time_last_fired, in the last few ticks) or an enemy's grenade or
  rocket in the air, within the difficulty's hearing range;
- it reckons where the noise was as an ear would: the further, the more it
  is off (a direction and a rough place, not the enemy's place), and no
  more than twice a second, so that it does not follow someone it cannot
  see;
- it turns to look that way (it may then see them), then goes there along
  the walking graph to see (bots_brain.c's bot_investigate).
Being shot is bots_combat.c's: the bot turns toward where the shots came
from, roughly, as a person does who is hit from behind.

Danger (bot_sense_danger, every bot, a peaceful one too): a live grenade
lying or flying near it (inside the projectile's danger radius and a
margin; not one in a thrower's hand, nor its own going away), and it runs
from it for a moment (bot_dodge), onto floor the graph has. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/players.h"
#include "items/projectile_definitions.h"
#include "items/projectiles.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "units/units.h"

#include "bots_internal.h"

#include <math.h>

/* a shot within the last ticks is heard; a noise is placed again at most
this often (ticks); the turn to look (ticks) */
#define BOT_HEAR_SHOT_TICKS 4
#define BOT_HEAR_AGAIN_TICKS (TICKS_PER_SECOND / 2)
#define BOT_LOOK_TICKS 12
/* how far off the ear is, for each world unit to the noise */
#define BOT_HEAR_ERROR 0.12f
/* the margin beyond a grenade's danger radius it keeps, and how long it
runs from one (ticks) */
#define BOT_DANGER_MARGIN 1.0f
#define BOT_DODGE_TICKS 20

/* whether a projectile is one that goes off with a blast (a grenade, a
rocket: one with a danger radius), and its radius */
static boolean bot_projectile_blasts(
	struct projectile_datum *projectile,
	real *radius)
{
	struct projectile_definition *definition = projectile_definition_get(projectile->definition_index);

	/* (as bots_grenades.c takes a grenade's blast: wider than the tag's
	danger radius) */
	*radius = MAX(definition->projectile.danger_radius, 4.0f);
	return definition->projectile.danger_radius > 0.5f;
}

/* whether the projectile is a grenade still in its thrower's hand */
static boolean bot_projectile_in_hand(
	long projectile_index,
	struct projectile_datum *projectile)
{
	struct unit_datum *parent = projectile->object.parent_object_index != NONE ?
		unit_try_and_get(projectile->object.parent_object_index) : NULL;

	return parent && parent->unit.grenade_object_index == projectile_index;
}

void bot_hear(
	struct bot *bot,
	long player_index,
	long unit_index)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_combat *combat = &brain->combat;
	struct unit_datum *unit = unit_get(unit_index);
	real range = bot_skill_hearing_range(bot);
	long now = game_time_get();
	long heard_player = NONE;
	real best = range * range;
	real_point3d heard;
	struct data_iterator iterator;
	struct player_datum *other;
	struct object_iterator objects;
	struct projectile_datum *projectile;

	if (range <= 0.0f || now - combat->heard_tick < BOT_HEAR_AGAIN_TICKS)
		return;
	/* (gunfire) */
	data_iterator_new(&iterator, player_data);
	while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct unit_datum *enemy;
		long weapon_index;
		real dx, dy, dz;

		if (iterator.datum_index == player_index || other->unit_index == NONE || other->quit_out_of_game)
			continue;
		enemy = unit_get(other->unit_index);
		if (TEST_FLAG(enemy->object.damage_flags, _object_dead_bit) ||
			!game_team_is_enemy(unit->object.owner_team_index, enemy->object.owner_team_index) ||
			enemy->unit.current_weapon_index == NONE)
		{
			continue;
		}
		weapon_index = enemy->unit.weapon_object_indices[enemy->unit.current_weapon_index];
		if (weapon_index == NONE || now - weapon_get(weapon_index)->weapon.game_time_last_fired > BOT_HEAR_SHOT_TICKS ||
			now < weapon_get(weapon_index)->weapon.game_time_last_fired)
		{
			continue;
		}
		dx = enemy->object.position.x - unit->object.position.x;
		dy = enemy->object.position.y - unit->object.position.y;
		dz = enemy->object.position.z - unit->object.position.z;
		if (dx * dx + dy * dy + dz * dz < best)
		{
			best = dx * dx + dy * dy + dz * dz;
			heard = enemy->object.position;
			heard_player = iterator.datum_index;
		}
	}
	/* (an enemy's grenade or rocket flying, bouncing, going off) */
	object_iterator_new(&objects, _object_mask_projectile, 0);
	while ((projectile = (struct projectile_datum *)object_iterator_next(&objects)) != NULL)
	{
		real radius, dx, dy, dz;
		long owner = projectile->object.owner_player_index;
		struct player_datum *thrower = owner != NONE ? player_try_and_get(owner) : NULL;

		if (!thrower || owner == player_index || !bot_projectile_blasts(projectile, &radius) ||
			bot_projectile_in_hand(objects.index, projectile) ||
			!game_team_is_enemy(unit->object.owner_team_index, projectile->object.owner_team_index))
		{
			continue;
		}
		dx = projectile->object.position.x - unit->object.position.x;
		dy = projectile->object.position.y - unit->object.position.y;
		dz = projectile->object.position.z - unit->object.position.z;
		if (dx * dx + dy * dy + dz * dz < best)
		{
			best = dx * dx + dy * dy + dz * dz;
			heard = projectile->object.position;
			heard_player = owner;
		}
	}
	if (heard_player == NONE)
		return;
	/* (as near as an ear tells) */
	best = (real)sqrt(best);
	heard.x += bot_gaussian(brain) * best * BOT_HEAR_ERROR;
	heard.y += bot_gaussian(brain) * best * BOT_HEAR_ERROR;
	/* (a new noise, or one long after the last: turned to, and gone to see) */
	if (heard_player != combat->heard_player || now - combat->heard_tick > 3 * TICKS_PER_SECOND)
	{
		combat->look_until_tick = now + BOT_LOOK_TICKS;
		combat->heard_new = TRUE;
		brain->statistics.noises_heard++;
	}
	combat->heard_position = heard;
	combat->heard_tick = now;
	combat->heard_player = heard_player;
}

void bot_sense_danger(
	struct bot *bot,
	long unit_index)
{
	struct bot_combat *combat = &bot->brain.combat;
	struct unit_datum *unit = unit_get(unit_index);
	struct object_iterator objects;
	struct projectile_datum *projectile;
	long now = game_time_get();
	real nearest = 1.0e30f;
	real_point3d danger;

	object_iterator_new(&objects, _object_mask_projectile, 0);
	while ((projectile = (struct projectile_datum *)object_iterator_next(&objects)) != NULL)
	{
		real radius, dx, dy, dz, distance_squared;
		struct projectile_definition *definition;

		if (!bot_projectile_blasts(projectile, &radius) || bot_projectile_in_hand(objects.index, projectile))
			continue;
		/* (grenades: the ones that fall; a rocket is too quick to run from) */
		definition = projectile_definition_get(projectile->definition_index);
		if (definition->projectile.air_gravity_scale <= 0.0f)
			continue;
		dx = projectile->object.position.x - unit->object.position.x;
		dy = projectile->object.position.y - unit->object.position.y;
		dz = projectile->object.position.z - unit->object.position.z;
		distance_squared = dx * dx + dy * dy + dz * dz;
		/* (its own, flying away from it: no danger) */
		if (projectile->object.owner_object_index == unit_index &&
			dx * projectile->object.translational_velocity.i + dy * projectile->object.translational_velocity.j > 0.0f)
		{
			continue;
		}
		/* (one stuck to it: no running from that) */
		if (projectile->object.parent_object_index == unit_index)
			continue;
		radius += BOT_DANGER_MARGIN;
		if (distance_squared < radius * radius && distance_squared < nearest)
		{
			nearest = distance_squared;
			danger = projectile->object.position;
		}
	}
	if (nearest > 1.0e29f)
		return;
	if (now >= combat->danger_until_tick)
		bot->brain.statistics.grenades_dodged++;
	combat->danger_position = danger;
	combat->danger_until_tick = now + BOT_DODGE_TICKS;
}

void bot_dodge(
	struct bot *bot,
	long unit_index,
	struct player_action *action)
{
	struct bot_combat *combat = &bot->brain.combat;
	struct unit_datum *unit = unit_get(unit_index);
	struct bots_nav *nav = bots_nav_current();
	real away, facing;
	short attempt;

	if (game_time_get() >= combat->danger_until_tick)
		return;
	away = (real)atan2(unit->object.position.y - combat->danger_position.y,
		unit->object.position.x - combat->danger_position.x);
	/* (straight away, else aslant, onto floor the graph has) */
	for (attempt = 0; nav && attempt < 5; attempt++)
	{
		static real const turns[5] = { 0.0f, 0.8f, -0.8f, 1.5f, -1.5f };
		real yaw = away + turns[attempt];

		if (bots_nav_locate(nav, unit->object.position.x + (real)cos(yaw) * 1.5f,
			unit->object.position.y + (real)sin(yaw) * 1.5f, unit->object.position.z) != NONE)
		{
			away = yaw;
			break;
		}
	}
	facing = action->desired_facing.yaw;
	action->throttle.i = (real)cos(away - facing);
	action->throttle.j = (real)sin(away - facing);
}
