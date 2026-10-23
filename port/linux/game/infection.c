/* infection.c: the Infection game type (infection.h; port/linux/README.md's
"Infection")

Team Slayer with the variant's infection set (game_engine.h: in bytes the
Xbox's variant left unused, which travel with the network game as they are).
Who is infected is the player's team: the host sets it (at the start, at a
survivor's death, for a player joining later) and every machine has it from
the host's unit (owner_team_index, network_player_attach_unit), as the
gametype's auto team balance already moves players. So the wire is the same.

- The start: one infected for each infection_ratio players (8), at least
  one, chosen with the game's random numbers (fixed by debug.game_seed).
- A survivor who dies, for any reason, respawns infected (the infected
  respawn at the engine's least, three seconds; the variant's respawn time is 0).
- The infected are always on a board (skate.c, bots_skate.c), unarmed: no
  firing, melee, grenades, picking up, boarding a vehicle (players.c,
  units.c's unit_can_enter_seat, game_engine_allow_pick_up). Their run-overs
  deal infection_run_over percent of any skater's (skate_authority.c).
- The end: everyone infected (game_engine.c's multiple_teams_alive) - the
  infected win; the round's time out (infection_minutes, 5, or the game's
  shorter limit) with a survivor left - the survivors win. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"

#include "infection.h"
#include "skate.h"

#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...);
int config_boolean(const char *name);
const char *config_string(const char *name);

/* what the infected may not do (units.h's unit control bits) */
#define INFECTION_REFUSED_CONTROLS (FLAG(_unit_control_action_bit) | FLAG(_unit_control_use_equipment_bit) | \
	FLAG(_unit_control_weapon_reload_bit) | FLAG(_unit_control_weapon_primary_trigger_bit) | \
	FLAG(_unit_control_weapon_secondary_trigger_bit) | FLAG(_unit_control_throw_grenade_bit) | \
	FLAG(_unit_control_swap_weapons_bit) | FLAG(UNIT_CONTROL_PORT_ACTION_ONLY_BIT))

/* the infected's change colour (their armour): green */
static real_rgb_color const infection_green = { 0.15f, 0.85f, 0.15f };

static struct
{
	/* the tick each player (by absolute index) was first seen infected,
	NONE while a survivor */
	long infected_tick[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	/* the game's start was seen (the host picked its first infected) */
	boolean started;
	boolean over_logged;
	/* (debug.infection_test_press) the refused presses counted, and the
	breaches of the rules seen (a test fails on any) */
	long refused_ticks;
	long breaches;
	long next_report_tick;
} infection;

static struct game_variant const *infection_variant(void)
{
	return game_engine_get_variant();
}

boolean infection_active(void)
{
	struct game_variant const *variant;

	if (!game_engine_running())
		return FALSE;
	variant = infection_variant();
	return variant->game_engine_index == game_engine_slayer && variant->game_engine_variant.slayer.infection;
}

boolean infection_player_infected(long player_index)
{
	struct player_datum *player;

	if (!infection_active() || player_index == NONE || !(player = player_try_and_get(player_index)))
		return FALSE;
	return player->team_index == INFECTION_TEAM_INFECTED;
}

boolean infection_unit_restricted(long unit_index)
{
	struct object_datum *object;

	if (unit_index == NONE || !infection_active() || !(object = object_try_and_get(unit_index)))
		return FALSE;
	return object->object.type == _object_type_biped && object->object.owner_player_index != NONE &&
		object->object.owner_team_index == INFECTION_TEAM_INFECTED &&
		!TEST_FLAG(object->object.damage_flags, _object_dead_bit);
}

void infection_filter_action(long player_index, long unit_index, unsigned long *control_flags)
{
	if (!infection_unit_restricted(unit_index))
		return;
	/* (debug.infection_test_press: the infected press all of it, every tick,
	for the tests: none of it may reach their unit) */
	if (config_boolean("debug.infection_test_press"))
	{
		*control_flags |= INFECTION_REFUSED_CONTROLS & ~FLAG(UNIT_CONTROL_PORT_ACTION_ONLY_BIT);
		infection.refused_ticks++;
	}
	*control_flags &= ~(unsigned long)INFECTION_REFUSED_CONTROLS;
	(void)player_index;
}

static long infection_absolute(long player_index)
{
	long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);

	return absolute >= 0 && absolute < HALO_PORT_MAXIMUM_NETWORK_PLAYERS ? absolute : NONE;
}

static boolean infection_host(void)
{
	return !network_game_distributed_client();
}

/* the host moves the player to a side; their next unit carries it to the
other machines */
static void infection_set_team(long player_index, long team, char const *why)
{
	struct player_datum *player = player_get(player_index);

	if (player->team_index == team)
		return;
	game_engine_port_set_player_team(player_index, team);
	platform_log("infection: player %ld %s at tick %ld", infection_absolute(player_index), why,
		game_time_get());
}

long infection_survivors(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long count = 0;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (!player->quit_out_of_game && player->team_index == INFECTION_TEAM_SURVIVORS)
			count++;
	}
	return count;
}

long infection_infected(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long count = 0;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (!player->quit_out_of_game && player->team_index == INFECTION_TEAM_INFECTED)
			count++;
	}
	return count;
}

long infection_survival_seconds(long player_index)
{
	long absolute = infection_absolute(player_index);
	long until;

	if (absolute == NONE)
		return 0;
	until = infection.infected_tick[absolute] != NONE ? infection.infected_tick[absolute] : game_time_get();
	return MAX(until, 0) / TICKS_PER_SECOND;
}

/* a random survivor (by the game's random numbers), NONE for none */
static long infection_random_survivor(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long count = infection_survivors();
	long chosen;

	if (count <= 0)
		return NONE;
	chosen = seed_random_range(get_global_random_seed_address(), 0, (short)count);
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (!player->quit_out_of_game && player->team_index == INFECTION_TEAM_SURVIVORS && chosen-- == 0)
			return iterator.datum_index;
	}
	return NONE;
}

void infection_game_starting(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long players = 0;
	long first;
	long ratio;
	short index;

	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
		infection.infected_tick[index] = NONE;
	infection.started = TRUE;
	infection.over_logged = FALSE;
	infection.refused_ticks = 0;
	infection.breaches = 0;
	infection.next_report_tick = 0;
	if (!infection_active() || !infection_host())
		return;
	/* everyone a survivor, then the first infected */
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (!player->quit_out_of_game)
		{
			players++;
			game_engine_port_set_player_team(iterator.datum_index, INFECTION_TEAM_SURVIVORS);
		}
	}
	ratio = infection_variant()->game_engine_variant.slayer.infection_ratio;
	if (ratio <= 0)
		ratio = 8;
	first = MAX(1, players / ratio);
	/* (one alone is a survivor: nobody to infect them, nor to win) */
	if (players < 2)
		first = 0;
	platform_log("infection: the game starts with %ld players, %ld infected, the round %ld seconds, run-overs at %d%%",
		players, first, infection_time_limit_ticks(game_variant_options_get()->time_limit) / TICKS_PER_SECOND,
		infection_variant()->game_engine_variant.slayer.infection_run_over > 0 ?
			infection_variant()->game_engine_variant.slayer.infection_run_over : 100);
	/* (debug.infection_first: the tests' own, by absolute index) */
	if (first > 0 && config_string("debug.infection_first") && config_string("debug.infection_first")[0])
	{
		char const *list = config_string("debug.infection_first");

		while (*list)
		{
			long wanted = strtol(list, (char **)&list, 10);

			data_iterator_new(&iterator, player_data);
			while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
			{
				if (!player->quit_out_of_game && infection_absolute(iterator.datum_index) == wanted)
					infection_set_team(iterator.datum_index, INFECTION_TEAM_INFECTED, "starts infected (the test's)");
			}
			while (*list && (*list < '0' || *list > '9'))
				list++;
		}
		first = 0;
	}
	while (first-- > 0)
	{
		long chosen = infection_random_survivor();


		if (chosen == NONE)
			break;
		infection_set_team(chosen, INFECTION_TEAM_INFECTED, "starts infected");
	}
}

void infection_player_added(long player_index)
{
	/* (a player joining once the game is under way starts infected; the
	start's own are game_starting's) */
	if (!infection_active() || !infection_host() || !infection.started || game_time_get() <= 0)
		return;
	infection_set_team(player_index, INFECTION_TEAM_INFECTED, "joins infected");
}

void infection_player_killed(long dead_player_index, long killing_player_index)
{
	struct player_datum *dead = player_get(dead_player_index);
	char why[96];

	if (!infection_active() || !infection_host() || dead->quit_out_of_game ||
		dead->team_index != INFECTION_TEAM_SURVIVORS)
	{
		return;
	}
	if (killing_player_index != NONE && killing_player_index != dead_player_index &&
		infection_player_infected(killing_player_index))
	{
		snprintf(why, sizeof(why), "is infected by player %ld", infection_absolute(killing_player_index));
	}
	else
		snprintf(why, sizeof(why), "dies a survivor (killer %ld), infected", killing_player_index == NONE ? -1L :
			infection_absolute(killing_player_index));
	infection_set_team(dead_player_index, INFECTION_TEAM_INFECTED, why);
}

long infection_time_limit_ticks(long time_limit_minutes)
{
	long minutes = infection_variant()->game_engine_variant.slayer.infection_minutes;

	if (minutes <= 0)
		minutes = 5;
	if (time_limit_minutes > 0)
		minutes = MIN(minutes, time_limit_minutes);
	return minutes * 60L * TICKS_PER_SECOND;
}

/* (the host, every few seconds of a test: the rules kept - no infected
armed, holding grenades or in a vehicle, and on a board when they can be) */
static void infection_check_rules(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long now = game_time_get();
	boolean report = now >= infection.next_report_tick;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct unit_datum *unit;
		short index;
		long weapons = 0, grenades = 0;

		if (player->unit_index == NONE || !infection_unit_restricted(player->unit_index))
			continue;
		unit = (struct unit_datum *)object_get(player->unit_index);
		for (index = 0; index < MAXIMUM_WEAPONS_PER_UNIT; index++)
			weapons += unit->unit.weapon_object_indices[index] != NONE;
		for (index = 0; index < NUMBER_OF_UNIT_GRENADE_TYPES; index++)
			grenades += unit->unit.grenade_counts[index];
		if (weapons || grenades || unit->object.parent_object_index != NONE)
		{
			infection.breaches++;
			platform_log("infection: RULE BROKEN: infected player %ld has %ld weapons, %ld grenades, %s at tick %ld",
				infection_absolute(iterator.datum_index), weapons, grenades,
				unit->object.parent_object_index != NONE ? "in a vehicle" : "on foot", now);
		}
		if (report)
		{
			platform_log("infection: infected player %ld at tick %ld: %s, %ld weapons, %ld grenades, %s",
				infection_absolute(iterator.datum_index), now,
				skate_unit_is_skating(player->unit_index) ? "on a board" : "not on a board", weapons, grenades,
				unit->object.parent_object_index != NONE ? "in a vehicle" : "no vehicle");
		}
	}
	if (report)
	{
		platform_log("infection: at tick %ld: %ld survivors, %ld infected; the infected's presses refused %ld times, "
			"%ld rules broken", now, infection_survivors(), infection_infected(), infection.refused_ticks,
			infection.breaches);
		infection.next_report_tick = now + 10 * TICKS_PER_SECOND;
	}
}

/* the infected's weapons and grenades taken (the host: its units are the
game's); each tick, so a spawn's starting weapons never stay */
static void infection_disarm(void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct unit_datum *unit;
		short index;
		boolean armed = FALSE;

		if (player->unit_index == NONE || !infection_unit_restricted(player->unit_index))
			continue;
		unit = (struct unit_datum *)object_get(player->unit_index);
		for (index = 0; index < MAXIMUM_WEAPONS_PER_UNIT; index++)
			armed |= unit->unit.weapon_object_indices[index] != NONE;
		if (armed)
			unit_delete_all_weapons(player->unit_index);
		for (index = 0; index < NUMBER_OF_UNIT_GRENADE_TYPES; index++)
			unit->unit.grenade_counts[index] = 0;
	}
}

void infection_update(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long now = game_time_get();

	if (!infection_active())
		return;
	/* (every machine: when each was first seen infected) */
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long absolute = infection_absolute(iterator.datum_index);

		if (absolute == NONE)
			continue;
		if (player->team_index == INFECTION_TEAM_INFECTED && infection.infected_tick[absolute] == NONE)
			infection.infected_tick[absolute] = now;
		else if (player->team_index != INFECTION_TEAM_INFECTED)
			infection.infected_tick[absolute] = NONE;
	}
	if (!infection_host() || !game_engine_port_game_active())
		return;
	infection_disarm();
	/* the last infected gone (quit) with survivors still playing: one of them
	is infected, dying where they stand to come back on a board */
	if (now > 0 && infection_infected() == 0 && infection_survivors() >= 2)
	{
		long chosen = infection_random_survivor();

		if (chosen != NONE)
		{
			long unit_index = player_get(chosen)->unit_index;

			infection_set_team(chosen, INFECTION_TEAM_INFECTED, "is infected (no infected left)");
			if (unit_index != NONE)
				unit_kill(unit_index);
		}
	}
	infection_check_rules();
}

void infection_game_over(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long survivors;

	if (!infection_active() || !infection_host() || infection.over_logged)
		return;
	infection.over_logged = TRUE;
	survivors = infection_survivors();
	if (survivors > 0)
		platform_log("infection: the game is over at tick %ld: the survivors win, %ld left", game_time_get(), survivors);
	else
		platform_log("infection: the game is over at tick %ld: the infected win", game_time_get());
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		platform_log("infection: player %ld %s, score %ld, survived %ld seconds",
			infection_absolute(iterator.datum_index),
			player->team_index == INFECTION_TEAM_INFECTED ? "infected" : "a survivor",
			game_engine_port_player_score(iterator.datum_index), infection_survival_seconds(iterator.datum_index));
	}
}

real infection_run_over_share(long skater_unit_index, real share)
{
	long percent;

	if (!infection_unit_restricted(skater_unit_index))
		return share;
	percent = infection_variant()->game_engine_variant.slayer.infection_run_over;
	if (percent <= 0)
		percent = 100;
	return MIN(1.0f, share * (real)percent / 100.0f);
}

real_rgb_color const *infection_change_color(long team_index)
{
	return team_index == INFECTION_TEAM_INFECTED ? &infection_green : global_real_rgb_red;
}

void infection_draw(short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);
	long font_index = hud_get_font_index();
	struct player_datum *player;
	struct font_header *font;
	real_argb_color color;
	rectangle2d bounds;
	wchar_t text[96];
	long survivors = 0;
	short height;
	struct data_iterator iterator;
	struct player_datum *other;

	if (!infection_active() || player_index == NONE || font_index == NONE || !(player = player_try_and_get(player_index)))
		return;
	/* (the survivors alive: a dead one comes back infected) */
	data_iterator_new(&iterator, player_data);
	while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (!other->quit_out_of_game && other->team_index == INFECTION_TEAM_SURVIVORS && other->unit_index != NONE)
			survivors++;
	}
	color.alpha = 1.0f;
	if (player->team_index == INFECTION_TEAM_INFECTED)
	{
		color.red = infection_green.red;
		color.green = infection_green.green;
		color.blue = infection_green.blue;
		usnprintf(text, NUMBEROF(text) - 1, L"INFECTED\r\n%ld SURVIVOR%ls LEFT", survivors, survivors == 1 ? L"" : L"S");
	}
	else
	{
		color.red = 1.0f;
		color.green = 0.35f;
		color.blue = 0.3f;
		usnprintf(text, NUMBEROF(text) - 1, L"SURVIVOR\r\n%ld SURVIVOR%ls LEFT", survivors, survivors == 1 ? L"" : L"S");
	}
	text[NUMBEROF(text) - 1] = 0;
	font = font_definition_get(font_index);
	height = (short)(font->ascending_height + font->descending_height);
	bounds = render.camera.window_bounds;
	bounds.y0 = (short)(bounds.y0 + 8);
	bounds.y1 = (short)(bounds.y0 + 2 * height + 4);
	/* draw mode 2: centred */
	draw_string_set_draw_mode(font_index, NONE, 2, 0, &color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
}
