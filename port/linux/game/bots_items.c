/* bots_items.c: what a bot wants of the items lying about, and the weapon
it fights with (bots_internal.h)

bot_item_value says how much a bot wants an item on the ground, for
bots_brain.c's pickups (the best for its distance is gone to; one it wants
more is gone to from further off):
- a weapon better than the worse of the two it carries (by bot_weapon_kinds:
  the rocket launcher and the sniper rifle above the rest), or any weapon
  while it carries fewer than two; ammunition (the same kind of weapon) for
  one that runs low; never a flag nor a ball (the game types' own);
- grenades of a kind it has fewer of than it can carry (the more when it
  has none at all), unless it is peaceful;
- an overshield and active camouflage always (more than any weapon), a
  health pack when its body is hurt.
A bot with two weapons that goes for a third drops the worse of its two for
it (bot_pickup_swap: that one in hand when it presses the action button).

bot_weapon_choose picks which of its weapons a bot fights with for the
target's distance: each kind's power, times how well it suits the range (a
shotgun near, a sniper rifle far, a rocket launcher never within its own
blast), and only with ammunition. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "items/equipment_definitions.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "bots_internal.h"

#include <string.h>

/* how much grenades are wanted; the powerups */
#define BOT_VALUE_GRENADES 0.6f
#define BOT_VALUE_FIRST_GRENADES 0.9f
#define BOT_VALUE_OVERSHIELD 3.0f
#define BOT_VALUE_CAMOUFLAGE 2.5f
#define BOT_VALUE_HEALTH 2.5f
/* a weapon of a kind the table does not know (a custom map's) */
#define BOT_VALUE_WEAPON 1.0f

/* the stock weapons, by their tags' last names (the first that the name
contains: plasma pistol before pistol): how good each is, the range it suits
(world units), and whether its blast hurts its holder near */
struct bot_weapon_kind
{
	char const *name;
	real value;
	real near, far;
	boolean blast;
};

static struct bot_weapon_kind const bot_weapon_kinds[] =
{
	{ "rocket launcher", 3.0f, 5.0f, 35.0f, TRUE },
	{ "sniper rifle", 2.6f, 10.0f, 90.0f, FALSE },
	{ "plasma_cannon", 2.4f, 5.0f, 30.0f, TRUE },
	{ "shotgun", 2.2f, 0.0f, 6.0f, FALSE },
	{ "plasma pistol", 1.0f, 0.0f, 12.0f, FALSE },
	{ "plasma rifle", 1.5f, 0.0f, 15.0f, FALSE },
	{ "pistol", 2.0f, 0.0f, 30.0f, FALSE },
	{ "flamethrower", 1.6f, 0.0f, 6.0f, FALSE },
	{ "assault rifle", 1.4f, 0.0f, 15.0f, FALSE },
	{ "needler", 1.3f, 2.0f, 15.0f, FALSE },
};

/* a weapon's kind (NULL: one the table does not know) */
static struct bot_weapon_kind const *bot_weapon_kind(
	long weapon_index)
{
	char const *name = tag_get_name(object_get(weapon_index)->definition_index);
	char const *last = name ? strrchr(name, '\\') : NULL;
	short index;

	last = last ? last + 1 : name;
	for (index = 0; last && index < (short)NUMBEROF(bot_weapon_kinds); index++)
	{
		if (strstr(last, bot_weapon_kinds[index].name))
			return &bot_weapon_kinds[index];
	}
	return NULL;
}

real bot_weapon_power(
	long weapon_index)
{
	struct bot_weapon_kind const *kind = bot_weapon_kind(weapon_index);

	if (weapon_is_flag(weapon_index))
		return 0.0f;
	return kind ? kind->value : BOT_VALUE_WEAPON;
}

/* how much of its ammunition the weapon has, 0 to 1 (a battery's charge,
or the rounds against a full load) */
static real bot_weapon_ammunition(
	long weapon_index)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	struct weapon_definition *definition = weapon_definition_get(weapon->definition_index);

	if (definition->weapon.magazines.count <= 0)
		return 1.0f - PIN(weapon->weapon.age, 0.0f, 1.0f);
	{
		struct weapon_magazine_definition *magazine =
			TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, 0, struct weapon_magazine_definition);
		real full = (real)MAX(1, magazine->rounds_total_initial + magazine->rounds_loaded_maximum);

		return PIN((real)(weapon->weapon.magazines[0].rounds_loaded + weapon->weapon.magazines[0].rounds_total) / full,
			0.0f, 1.0f);
	}
}

/* the unit's weapons: how many count toward the two, and the worse one's
slot and power (NONE, 0 for none) */
static short bot_weapons_held(
	struct unit_datum *unit,
	short *worst_slot,
	real *worst_power)
{
	short count = 0;
	short slot;

	*worst_slot = NONE;
	*worst_power = 0.0f;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long held = unit->unit.weapon_object_indices[slot];
		real power;

		if (held == NONE)
			continue;
		power = bot_weapon_power(held);
		/* (an empty one is worth little) */
		if (bot_weapon_ammunition(held) <= 0.0f)
			power *= 0.1f;
		if (*worst_slot == NONE || power < *worst_power)
		{
			*worst_slot = slot;
			*worst_power = power;
		}
		count++;
	}
	return count;
}

static real bot_weapon_value(
	struct unit_datum *unit,
	long weapon_index)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	real power = bot_weapon_power(weapon_index);
	short worst_slot;
	real worst_power;
	short slot;

	if (power <= 0.0f)
		return 0.0f;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long held = unit->unit.weapon_object_indices[slot];

		/* (one of a kind it holds: its ammunition, when it runs low) */
		if (held != NONE && weapon_get(held)->definition_index == weapon->definition_index)
			return bot_weapon_ammunition(held) < 0.4f && bot_weapon_ammunition(weapon_index) > 0.0f ? 0.8f : 0.0f;
	}
	if (bot_weapon_ammunition(weapon_index) <= 0.0f)
		return 0.0f;
	if (bot_weapons_held(unit, &worst_slot, &worst_power) < 2)
		return power * 0.6f;
	/* (its hands full: only for a better one than the worse it holds) */
	return power > worst_power + 0.3f ? (power - worst_power) * (power >= 2.2f ? 1.4f : 0.8f) : 0.0f;
}

static real bot_equipment_value(
	struct bot const *bot,
	struct unit_datum *unit,
	long equipment_index)
{
	struct object_datum *equipment = object_get(equipment_index);
	struct equipment_definition *definition = equipment_definition_get(equipment->definition_index);
	struct game_globals *globals = scenario_get_game_globals();

	switch (definition->equipment.powerup_type)
	{
	case _equipment_powerup_grenade:
	{
		short type = definition->equipment.grenade_type;
		struct game_globals_grenade *grenade;

		/* (not for a peaceful bot, which never throws one) */
		if (bot->difficulty == _bot_difficulty_peaceful || !globals || type < 0 || type >= NUMBER_OF_UNIT_GRENADE_TYPES ||
			type >= globals->grenades.count)
		{
			return 0.0f;
		}
		grenade = TAG_BLOCK_GET_ELEMENT(&globals->grenades, type, struct game_globals_grenade);
		if (unit->unit.grenade_counts[type] >= grenade->maximum_count)
			return 0.0f;
		return unit->unit.grenade_counts[_unit_grenade_human_fragmentation] +
			unit->unit.grenade_counts[_unit_grenade_covenant_plasma] == 0 ? BOT_VALUE_FIRST_GRENADES : BOT_VALUE_GRENADES;
	}
	case _equipment_powerup_overshield:
		/* (one it has already: no more) */
		return unit->object.shield_vitality > 1.05f ? 0.0f : BOT_VALUE_OVERSHIELD;
	case _equipment_powerup_active_camouflage:
		return TEST_FLAG(unit->unit.flags, _unit_active_camouflaged_bit) ? 0.0f : BOT_VALUE_CAMOUFLAGE;
	case _equipment_powerup_health:
		return unit->object.body_vitality < 0.95f ? BOT_VALUE_HEALTH * (1.0f - unit->object.body_vitality) + 0.5f : 0.0f;
	default:
		return 0.0f;
	}
}

real bot_item_value(
	struct bot const *bot,
	long unit_index,
	long object_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct object_datum *item = object_get(object_index);

	switch (item->object.type)
	{
	case _object_type_weapon:
		return bot_weapon_value(unit, object_index);
	case _object_type_equipment:
		return bot_equipment_value(bot, unit, object_index);
	default:
		return 0.0f;
	}
}

boolean bot_item_is_powerup(
	long object_index)
{
	struct object_datum *item = object_try_and_get(object_index);
	short powerup;

	if (!item || item->object.type != _object_type_equipment)
		return FALSE;
	powerup = equipment_definition_get(item->definition_index)->equipment.powerup_type;
	return powerup == _equipment_powerup_overshield || powerup == _equipment_powerup_active_camouflage ||
		powerup == _equipment_powerup_health;
}

short bot_pickup_swap(
	long unit_index,
	long object_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct object_datum *item = object_try_and_get(object_index);
	short worst_slot;
	real worst_power;
	short slot;

	if (!item || item->object.type != _object_type_weapon)
		return NONE;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long held = unit->unit.weapon_object_indices[slot];

		if (held != NONE && weapon_get(held)->definition_index == item->definition_index)
			return NONE;
	}
	if (bot_weapons_held(unit, &worst_slot, &worst_power) < 2)
		return NONE;
	return worst_slot;
}

short bot_weapon_choose(
	long unit_index,
	real distance)
{
	struct unit_datum *unit = unit_get(unit_index);
	short best = NONE;
	real best_score = 0.0f;
	short slot;

	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long held = unit->unit.weapon_object_indices[slot];
		struct bot_weapon_kind const *kind;
		real score;

		if (held == NONE || weapon_is_flag(held) || bot_weapon_ammunition(held) <= 0.0f)
			continue;
		kind = bot_weapon_kind(held);
		score = kind ? kind->value : BOT_VALUE_WEAPON;
		if (kind)
		{
			/* (within its own blast: hardly ever; out of its range: less) */
			if (kind->blast && distance < kind->near)
				score *= 0.02f;
			else if (distance < kind->near || distance > kind->far)
				score *= 0.35f;
		}
		/* (the one in hand, unless another is clearly better: no dithering) */
		if (slot == unit->unit.current_weapon_index)
			score *= 1.15f;
		if (score > best_score)
		{
			best_score = score;
			best = slot;
		}
	}
	return best;
}

boolean bot_weapon_blasts_near(
	long weapon_index,
	real distance)
{
	struct bot_weapon_kind const *kind = weapon_index != NONE ? bot_weapon_kind(weapon_index) : NULL;

	return kind && kind->blast && distance < kind->near * 0.8f;
}
