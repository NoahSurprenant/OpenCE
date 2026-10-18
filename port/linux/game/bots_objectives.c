/* bots_objectives.c: what a bot plays for in each game type (bots_internal.h)

Each game type's objective, from what every machine sees (the flags and
balls are weapons, weapon_is_flag; the goals the game types set for the
HUD's navigation points, game_engine_get_goal_position; the scenario's
netgame flags), as a place a bot goes to, holds, or carries something to:

- Capture the Flag: a bot carrying the enemy's flag runs home with it (to
  its own flag's stand), and fights only what is in its way; one whose own
  flag lies dropped goes to return it (the nearest of its team); one whose
  flag is carried hunts the carrier. Otherwise each team's bots share the
  roles: the first, third and so on attack (to the enemy's flag, wherever
  it is, and take it; or, a team mate carrying it, escort them), the second,
  fourth and so on defend (they stay near their own flag's stand). One flag
  (single-flag CTF): its team defends it, the other attacks.
- King of the Hill: to the hill (the game's goal), and stay on it, fighting
  from it; a moving hill is followed.
- Oddball: to a ball lying about, to take it; with it, away from its enemies
  (a carrier fights only at hand); an enemy carrying it is hunted, a team
  mate carrying it escorted. Juggernaut and reverse tag (the ball goes to a
  killer): the one who has it is hunted; the one who has it fights.
- Race: to the race's flags, each in turn.
- Team Slayer: no objective, but a bot that sees nobody goes over to a team
  mate now and then, so that team mates fight together.
- Slayer: none (the bot roams, bots_brain.c). */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "bots_internal.h"

#include <math.h>

/* the platform layer's (port_config.c, the port's log) */
int config_boolean(const char *name);
void platform_log(const char *format, ...);
/* game_engine_oddball.c's: whether the player has a ball */
boolean player_has_ball(long player_index);

/* the most flags or balls looked at, and the game's goals (race flags) */
#define BOT_OBJECTIVE_ITEMS 16
#define BOT_OBJECTIVE_GOALS 32
/* how near a defender keeps to its flag's stand, an escort to the carrier,
and how far it goes to return its flag (world units) */
#define BOT_DEFEND_RADIUS 5.0f
/* how near the carrier of its flag an attacker turns to hunt them */
#define BOT_HUNT_NEAR 12.0f
#define BOT_ESCORT_RADIUS 4.0f
/* the hill's radius taken when its markers say nothing */
#define BOT_HILL_RADIUS 1.5f
/* how near a race flag counts as touched */
#define BOT_RACE_TOUCH 2.0f

struct bot_flag
{
	long object_index;
	short team;
	long carrier_player;
	real_point3d position;
	boolean home;
};

/* the stand of a team's flag (FALSE: none) */
static boolean bot_flag_stand(
	short team,
	real_point3d *stand)
{
	struct scenario *scenario = global_scenario_get();
	long index;

	for (index = 0; scenario && index < scenario->netgame_flags.count; index++)
	{
		struct scenario_netgame_flag *flag = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, index,
			struct scenario_netgame_flag);

		if (flag->type == _netgame_flag_ctf_flag && flag->team_index == team)
		{
			*stand = flag->position;
			return TRUE;
		}
	}
	return FALSE;
}

/* the flags (or balls) of the game: each one's team, carrier and place;
their count */
static short bot_flags(
	struct bot_flag *flags,
	short maximum)
{
	struct object_iterator iterator;
	struct object_datum *object;
	short count = 0;

	object_iterator_new(&iterator, _object_mask_weapon, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL && count < maximum)
	{
		struct bot_flag *flag = &flags[count];
		long parent;
		real_point3d stand;

		if (!weapon_is_flag(iterator.index))
			continue;
		flag->object_index = iterator.index;
		flag->team = object->object.owner_team_index;
		flag->carrier_player = NONE;
		parent = object->object.parent_object_index;
		if (parent != NONE && object_try_and_get(parent) && object_get_type(parent) == _object_type_biped)
			flag->carrier_player = unit_get(parent)->unit.player_index;
		flag->position = parent != NONE && object_try_and_get(parent) ? object_get(parent)->object.position :
			object->object.position;
		flag->home = flag->carrier_player == NONE && bot_flag_stand(flag->team, &stand) &&
			(flag->position.x - stand.x) * (flag->position.x - stand.x) +
			(flag->position.y - stand.y) * (flag->position.y - stand.y) < 1.5f * 1.5f;
		count++;
	}
	return count;
}

static void bot_objective_at(
	struct bot_objective *objective,
	short kind,
	real_point3d const *point,
	real radius)
{
	objective->kind = kind;
	objective->point.x = point->x;
	objective->point.y = point->y;
	objective->point.z = point->z;
	objective->radius = radius;
}

static real bot_flat_distance(
	real_point3d const *a,
	real_point3d const *b)
{
	return (real)sqrt((a->x - b->x) * (a->x - b->x) + (a->y - b->y) * (a->y - b->y));
}

/* whether the bot is its team's nearest bot to the point (of the bots and
people of its team alive) */
static boolean bot_nearest_of_team(
	long player_index,
	long unit_index,
	real_point3d const *point)
{
	struct unit_datum *unit = unit_get(unit_index);
	real mine = bot_flat_distance(&unit->object.position, point);
	struct data_iterator iterator;
	struct player_datum *other;

	data_iterator_new(&iterator, player_data);
	while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct object_datum *mate;

		if (iterator.datum_index == player_index || other->unit_index == NONE)
			continue;
		mate = object_get(other->unit_index);
		if (!TEST_FLAG(mate->object.damage_flags, _object_dead_bit) &&
			!game_team_is_enemy(unit->object.owner_team_index, mate->object.owner_team_index) &&
			bot_flat_distance(&mate->object.position, point) < mine)
		{
			return FALSE;
		}
	}
	return TRUE;
}

/* ---------- capture the flag */

static void bot_objective_ctf(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct bot_objective *objective)
{
	struct unit_datum *unit = unit_get(unit_index);
	short team = unit->object.owner_team_index;
	struct bot_flag flags[BOT_OBJECTIVE_ITEMS];
	short count = bot_flags(flags, BOT_OBJECTIVE_ITEMS);
	struct bot_flag *own = NULL, *enemy = NULL;
	short index, rank, mates;
	real_point3d home;

	for (index = 0; index < count; index++)
	{
		if (flags[index].team == team)
			own = &flags[index];
		else
			enemy = &flags[index];
	}
	/* carrying the enemy's flag: home with it */
	if (enemy && enemy->carrier_player == player_index && bot_flag_stand(team, &home))
	{
		real_point3d stand;

		/* (how far it has carried it from the enemy's stand, the furthest) */
		if (bot_flag_stand(enemy->team, &stand) &&
			bot_flat_distance(&unit->object.position, &stand) > bot->brain.statistics.longest_carry)
		{
			bot->brain.statistics.longest_carry = bot_flat_distance(&unit->object.position, &stand);
		}
		bot_objective_at(objective, _bot_objective_carry, &home, 0.5f);
		return;
	}
	rank = bots_team_rank(bot, &mates);
	/* its own flag carried off: the carrier hunted, by the defender, the
	nearest of the team and any near (the attackers go on; a team's only
	bot goes for the other flag, unless it is near); dropped: returned */
	if (own && own->carrier_player != NONE &&
		((mates > 1 && rank % 2 == 1) || (mates > 1 && bot_nearest_of_team(player_index, unit_index, &own->position)) ||
		bot_flat_distance(&unit->object.position, &own->position) < BOT_HUNT_NEAR))
	{
		bot_objective_at(objective, _bot_objective_hunt, &own->position, 1.0f);
		objective->hunted_player = own->carrier_player;
		return;
	}
	if (own && !own->home && bot_nearest_of_team(player_index, unit_index, &own->position))
	{
		bot_objective_at(objective, _bot_objective_take, &own->position, 0.3f);
		objective->object_index = own->object_index;
		return;
	}
	/* a defender (every second bot of a team, and the team of the one flag) */
	if (own && (!enemy || (mates > 1 && rank % 2 == 1)) && bot_flag_stand(team, &home))
	{
		bot_objective_at(objective, _bot_objective_hold, &home, BOT_DEFEND_RADIUS);
		return;
	}
	if (!enemy)
		return;
	/* an attacker: a team mate with the flag escorted, else to the flag */
	if (enemy->carrier_player != NONE)
	{
		bot_objective_at(objective, _bot_objective_hold, &enemy->position, BOT_ESCORT_RADIUS);
		return;
	}
	bot_objective_at(objective, _bot_objective_take, &enemy->position, 0.3f);
	objective->object_index = enemy->object_index;
}

/* ---------- king of the hill */

static void bot_objective_king(
	struct bot_objective *objective)
{
	struct scenario *scenario = global_scenario_get();
	real_point3d center;
	real radius = 1.0e30f;
	long index;

	if (!game_engine_get_goal_in_use(0))
		return;
	game_engine_get_goal_position(&center, 0);
	/* (the goal is set a biped's eyes above the hill: on the ground) */
	center.z -= 0.63f;
	/* (the hill's markers around it: the nearest, inside) */
	for (index = 0; scenario && index < scenario->netgame_flags.count; index++)
	{
		struct scenario_netgame_flag *flag = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, index,
			struct scenario_netgame_flag);
		real distance;

		if (flag->type != _netgame_flag_hill)
			continue;
		distance = bot_flat_distance(&flag->position, &center);
		if (distance > 0.5f && distance < 10.0f && distance < radius)
			radius = distance;
	}
	radius = radius < 1.0e29f ? PIN(radius * 0.6f, 0.8f, 4.0f) : BOT_HILL_RADIUS;
	bot_objective_at(objective, _bot_objective_hold, &center, radius);
}

/* ---------- oddball */

static void bot_objective_oddball(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct bot_objective *objective)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct bot_flag balls[BOT_OBJECTIVE_ITEMS];
	short count = bot_flags(balls, BOT_OBJECTIVE_ITEMS);
	struct bot_flag *best = NULL;
	real best_distance = 1.0e30f;
	short index;

	(void)bot;
	/* (juggernaut, reverse tag: no ball to carry, the one who has it hunted) */
	if (count == 0)
	{
		struct data_iterator iterator;
		struct player_datum *other;

		if (player_has_ball(player_index))
			return;
		data_iterator_new(&iterator, player_data);
		while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			if (iterator.datum_index != player_index && other->unit_index != NONE && player_has_ball(iterator.datum_index))
			{
				bot_objective_at(objective, _bot_objective_hunt, &object_get(other->unit_index)->object.position, 1.0f);
				objective->hunted_player = iterator.datum_index;
				return;
			}
		}
		return;
	}
	for (index = 0; index < count; index++)
	{
		struct bot_flag *ball = &balls[index];
		real distance = bot_flat_distance(&ball->position, &unit->object.position);

		/* (its own: away from its enemies with it) */
		if (ball->carrier_player == player_index)
		{
			bot_objective_at(objective, _bot_objective_carry, &unit->object.position, 0.0f);
			objective->flee = TRUE;
			return;
		}
		if (distance < best_distance)
		{
			best_distance = distance;
			best = ball;
		}
	}
	if (!best)
		return;
	if (best->carrier_player == NONE)
	{
		bot_objective_at(objective, _bot_objective_take, &best->position, 0.3f);
		objective->object_index = best->object_index;
	}
	else if (game_engine_has_teams() && player_get(best->carrier_player)->team_index == player_get(player_index)->team_index)
	{
		bot_objective_at(objective, _bot_objective_hold, &best->position, BOT_ESCORT_RADIUS);
	}
	else
	{
		bot_objective_at(objective, _bot_objective_hunt, &best->position, 1.0f);
		objective->hunted_player = best->carrier_player;
	}
}

/* ---------- race */

static void bot_objective_race(
	struct bot *bot,
	long unit_index,
	struct bot_objective *objective)
{
	struct bot_brain *brain = &bot->brain;
	struct unit_datum *unit = unit_get(unit_index);
	short tries;

	for (tries = 0; tries < BOT_OBJECTIVE_GOALS; tries++)
	{
		short goal = (short)((brain->race_flag + tries) % BOT_OBJECTIVE_GOALS);
		real_point3d point;

		if (!game_engine_get_goal_in_use(goal))
			continue;
		game_engine_get_goal_position(&point, goal);
		point.z -= 0.63f;
		if (bot_flat_distance(&point, &unit->object.position) < BOT_RACE_TOUCH)
		{
			brain->race_flag = goal + 1;
			brain->statistics.race_flags++;
			continue;
		}
		brain->race_flag = goal;
		bot_objective_at(objective, _bot_objective_go, &point, BOT_RACE_TOUCH * 0.5f);
		return;
	}
}

/* ---------- team slayer */

static void bot_objective_team(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct bot_objective *objective)
{
	struct bot_brain *brain = &bot->brain;
	struct unit_datum *unit = unit_get(unit_index);
	long now = game_time_get();
	struct player_datum *mate;

	/* (a team mate to go over to, chosen now and then) */
	if (now >= brain->buddy_tick)
	{
		struct data_iterator iterator;
		struct player_datum *other;
		long candidates[16];
		short count = 0;

		brain->buddy_tick = now + (long)((10.0f + bot_random_real(brain) * 10.0f) * TICKS_PER_SECOND);
		brain->buddy_player = NONE;
		data_iterator_new(&iterator, player_data);
		while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL && count < 16)
		{
			if (iterator.datum_index != player_index && other->unit_index != NONE &&
				other->team_index == player_get(player_index)->team_index)
			{
				candidates[count++] = iterator.datum_index;
			}
		}
		/* (half the time: the other half it roams) */
		if (count > 0 && bot_random_real(brain) < 0.5f)
			brain->buddy_player = candidates[(long)(bot_random_real(brain) * count) % count];
	}
	mate = brain->buddy_player != NONE ? player_try_and_get(brain->buddy_player) : NULL;
	if (!mate || mate->unit_index == NONE)
		return;
	if (bot_flat_distance(&object_get(mate->unit_index)->object.position, &unit->object.position) < BOT_ESCORT_RADIUS)
		return;
	bot_objective_at(objective, _bot_objective_hold, &object_get(mate->unit_index)->object.position, BOT_ESCORT_RADIUS);
}

/* ---------- */

void bot_objective_update(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct bot_objective *objective)
{
	struct game_variant *variant = game_engine_running() ? game_engine_get_variant() : NULL;

	short before = objective->kind;

	csmemset(objective, 0, sizeof(*objective));
	objective->kind = _bot_objective_none;
	objective->object_index = NONE;
	objective->hunted_player = NONE;
	if (!variant)
		return;
	switch (variant->game_engine_index)
	{
	case game_engine_ctf:
		bot_objective_ctf(bot, player_index, unit_index, objective);
		break;
	case game_engine_king:
		bot_objective_king(objective);
		break;
	case game_engine_oddball:
		bot_objective_oddball(bot, player_index, unit_index, objective);
		break;
	case game_engine_race:
		bot_objective_race(bot, unit_index, objective);
		break;
	case game_engine_slayer:
		if (variant->universal_variant.teams)
			bot_objective_team(bot, player_index, unit_index, objective);
		break;
	default:
		break;
	}
	if (objective->kind != before && config_boolean("debug.bot_trace"))
	{
		struct object_datum *unit = object_get(unit_index);

		struct unit_datum *carrier = (struct unit_datum *)unit;
		long held = carrier->unit.current_weapon_index != NONE ? carrier->unit.weapon_object_indices[carrier->unit.current_weapon_index] : NONE;

		platform_log("bots: objective %p %d -> %d at (%.1f %.1f %.1f): to (%.1f %.1f %.1f) radius %.1f; body %.2f shield %.2f, "
			"in hand %s (slot %d, desired %d), throw state %d", (void *)bot,
			(int)before, (int)objective->kind, unit->object.position.x, unit->object.position.y, unit->object.position.z,
			objective->point.x, objective->point.y, objective->point.z, objective->radius, unit->object.body_vitality,
			unit->object.shield_vitality, held != NONE ? tag_get_name(object_get(held)->definition_index) : "-",
			(int)carrier->unit.current_weapon_index, (int)carrier->unit.desired_weapon_index,
			(int)carrier->unit.grenade_throw_state);
	}
}
