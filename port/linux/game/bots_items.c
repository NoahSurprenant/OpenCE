/* bots_items.c: what a bot wants of the items lying about (bots_internal.h)

bot_item_value says how much a bot wants an item on the ground, for
bots_brain.c's pickups (the best for its distance is gone to; one it wants
more is gone to from further off):
- a weapon of a kind it holds none of (not a flag nor a ball: the game
  types' own);
- grenades of a kind it has fewer of than it can carry (the more when it
  has none at all), unless it is peaceful. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "items/equipment_definitions.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "bots_internal.h"

/* how much a weapon of a kind it lacks is wanted, and grenades */
#define BOT_VALUE_WEAPON 1.0f
#define BOT_VALUE_GRENADES 0.6f
#define BOT_VALUE_FIRST_GRENADES 0.9f

static real bot_weapon_value(
	struct unit_datum *unit,
	long weapon_index)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	short slot;

	if (weapon_is_flag(weapon_index))
		return 0.0f;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long held_index = unit->unit.weapon_object_indices[slot];

		if (held_index != NONE && weapon_get(held_index)->definition_index == weapon->definition_index)
			return 0.0f;
	}
	return BOT_VALUE_WEAPON;
}

static real bot_equipment_value(
	struct bot const *bot,
	struct unit_datum *unit,
	long equipment_index)
{
	struct object_datum *equipment = object_get(equipment_index);
	struct equipment_definition *definition = equipment_definition_get(equipment->definition_index);
	struct game_globals *globals = scenario_get_game_globals();

	/* (grenades: not for a peaceful bot, which never throws one) */
	if (definition->equipment.powerup_type == _equipment_powerup_grenade && bot->difficulty != _bot_difficulty_peaceful)
	{
		short type = definition->equipment.grenade_type;
		struct game_globals_grenade *grenade;

		if (!globals || type < 0 || type >= NUMBER_OF_UNIT_GRENADE_TYPES || type >= globals->grenades.count)
			return 0.0f;
		grenade = TAG_BLOCK_GET_ELEMENT(&globals->grenades, type, struct game_globals_grenade);
		if (unit->unit.grenade_counts[type] >= grenade->maximum_count)
			return 0.0f;
		return unit->unit.grenade_counts[_unit_grenade_human_fragmentation] +
			unit->unit.grenade_counts[_unit_grenade_covenant_plasma] == 0 ? BOT_VALUE_FIRST_GRENADES : BOT_VALUE_GRENADES;
	}
	return 0.0f;
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
