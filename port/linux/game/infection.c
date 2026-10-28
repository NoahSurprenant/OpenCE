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
#include "trick_attack.h"
#include "bots.h"
#include "skate.h"

#include <math.h>
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
	/* the infected unit each player was last disarmed in (the host) */
	long disarmed_unit[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	/* the game's start was seen (the host picked its first infected) */
	boolean started;
	boolean over_logged;
	/* this machine's first player's team, as last told (NONE: none) */
	long local_team;
	/* (debug.infection_test_press) the refused presses counted, and the
	breaches of the rules seen (a test fails on any) */
	long refused_ticks;
	long breaches;
	long next_report_tick;
	/* out of reach: each player's ticks where no board reaches (held while
	unknown), the unit they were counted for, and the hurts dealt (the host) */
	long out_of_reach_ticks[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	long out_of_reach_unit[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	long out_of_reach_hurts[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	/* (debug.infection_test_place) the players put where the test wants */
	boolean placed[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	/* the graph's reach was told (the log: once a game) */
	boolean reach_told;
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

/* the unit is an infected player's, alive (Infection's own: their boards'
speed, turning, reach and run-over damage) */
static boolean infection_unit_infected(long unit_index)
{
	struct object_datum *object;

	if (unit_index == NONE || !infection_active() || !(object = object_try_and_get(unit_index)))
		return FALSE;
	return object->object.type == _object_type_biped && object->object.owner_player_index != NONE &&
		object->object.owner_team_index == INFECTION_TEAM_INFECTED &&
		!TEST_FLAG(object->object.damage_flags, _object_dead_bit);
}

boolean infection_unit_restricted(long unit_index)
{
	/* (and in Trick Attack everyone: trick_attack.c) */
	return infection_unit_infected(unit_index) || trick_attack_unit_restricted(unit_index);
}

void infection_filter_action(long player_index, long unit_index, unsigned long *control_flags)
{
	if (!infection_unit_restricted(unit_index))
		return;
	/* (counted for the tests: debug.infection_test_press has the bots press
	all of it every tick, and none of it may reach their unit) */
	if (*control_flags & INFECTION_REFUSED_CONTROLS)
		infection.refused_ticks++;
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
	{
		infection.infected_tick[index] = NONE;
		infection.disarmed_unit[index] = NONE;
		infection.out_of_reach_ticks[index] = 0;
		infection.out_of_reach_unit[index] = NONE;
		infection.out_of_reach_hurts[index] = 0;
		infection.placed[index] = FALSE;
	}
	infection.reach_told = FALSE;
	infection.started = TRUE;
	infection.over_logged = FALSE;
	infection.local_team = NONE;
	infection.refused_ticks = 0;
	infection.breaches = 0;
	infection.next_report_tick = 0;
	if (!infection_active())
		return;
	/* (every machine: the variant's settings, as it has them - a client's
	from the host's game) */
	{
		struct slayer_variant const *slayer = &infection_variant()->game_engine_variant.slayer;

		platform_log("infection: the variant: round %ld seconds, run-overs %d%%, speed %d%%, damage taken %d%%, "
			"reach %d, one infected in %d, turning %d%%, out of reach %d",
			infection_time_limit_ticks(game_variant_options_get()->time_limit) / TICKS_PER_SECOND,
			slayer->infection_run_over, slayer->infection_speed, slayer->infection_damage_taken,
			slayer->infection_reach, slayer->infection_ratio, slayer->infection_turning,
			slayer->infection_out_of_reach);
	}
	if (!infection_host())
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

		/* ("client": every person on another machine - neither the host's
		own players nor bots; "bots": every bot) */
		if (!strcmp(list, "bots"))
		{
			data_iterator_new(&iterator, player_data);
			while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
			{
				if (!player->quit_out_of_game && bots_player_is_bot(player))
					infection_set_team(iterator.datum_index, INFECTION_TEAM_INFECTED, "starts infected (the test's bot)");
			}
			list = "";
		}
		else if (!strcmp(list, "client"))
		{
			data_iterator_new(&iterator, player_data);
			while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
			{
				if (!player->quit_out_of_game && player->local_player_index == NONE &&
					!bots_player_is_bot(player))
				{
					infection_set_team(iterator.datum_index, INFECTION_TEAM_INFECTED, "starts infected (the test's client)");
				}
			}
			list = "";
		}
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
		/* (what reached the unit: its control flags of this tick) */
		if (weapons || grenades || unit->object.parent_object_index != NONE ||
			(unit->unit.control_flags & INFECTION_REFUSED_CONTROLS))
		{
			infection.breaches++;
			platform_log("infection: RULE BROKEN: infected player %ld has %ld weapons, %ld grenades, %s, controls %lx "
				"at tick %ld", infection_absolute(iterator.datum_index), weapons, grenades,
				unit->object.parent_object_index != NONE ? "in a vehicle" : "on foot",
				(unsigned long)unit->unit.control_flags, now);
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
game's), once for each unit, as it spawns: none comes back (they pick up
nothing), which infection_check_rules sees */
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
		long absolute = infection_absolute(iterator.datum_index);

		if (absolute == NONE || player->unit_index == NONE || !infection_unit_restricted(player->unit_index) ||
			infection.disarmed_unit[absolute] == player->unit_index)
		{
			continue;
		}
		infection.disarmed_unit[absolute] = player->unit_index;

		unit = (struct unit_datum *)object_get(player->unit_index);
		for (index = 0; index < MAXIMUM_WEAPONS_PER_UNIT; index++)
			armed |= unit->unit.weapon_object_indices[index] != NONE;
		if (armed)
			unit_delete_all_weapons(player->unit_index);
		for (index = 0; index < NUMBER_OF_UNIT_GRENADE_TYPES; index++)
			unit->unit.grenade_counts[index] = 0;
	}
}

/* ---------- out of reach (infection.h) */

/* the hurt each second out of reach: the shield gone (and held down), and
this much of the body's vitality - dead in four seconds */
#define INFECTION_OUT_OF_REACH_HURT 0.25f
/* a survivor is warned (infection_draw) once out of reach this long: a hop
onto a crate and back down tells nothing */
#define INFECTION_OUT_OF_REACH_WARN_TICKS (2 * TICKS_PER_SECOND)

/* the ticks a survivor may stay out of reach before it hurts them; 0: never */
static long infection_out_of_reach_limit(void)
{
	long seconds = infection_variant()->game_engine_variant.slayer.infection_out_of_reach;

	if (seconds == INFECTION_OUT_OF_REACH_NEVER)
		return 0;
	if (seconds <= 0)
		seconds = INFECTION_DEFAULT_OUT_OF_REACH;
	return seconds * TICKS_PER_SECOND;
}

/* (debug.infection_test_place, the host) each player the test names put
where it says, once, as their first unit is in the game */
static void infection_test_place(void)
{
	char const *list = config_string("debug.infection_test_place");
	struct player_datum *player;

	if (!list || !list[0])
		return;
	while (*list)
	{
		char *end;
		long wanted = strtol(list, &end, 10);
		real_point3d point;
		/* ("client": the person on another machine, as debug.infection_first) */
		boolean client = !strncmp(list, "client:", 7);

		if (client)
			end = (char *)list + 6;
		else if (end == list || *end != ':')
			break;
		list = end + 1;
		point.x = (real)strtod(list, &end);
		list = *end == ',' ? end + 1 : end;
		point.y = (real)strtod(list, &end);
		list = *end == ',' ? end + 1 : end;
		point.z = (real)strtod(list, &end);
		list = end;
		while (*list == ';' || *list == ' ')
			list++;
		{
			struct data_iterator iterator;

			data_iterator_new(&iterator, player_data);
			while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
			{
				long absolute = infection_absolute(iterator.datum_index);

				if (absolute == NONE || infection.placed[absolute] || player->unit_index == NONE ||
					!object_try_and_get(player->unit_index) || (client ? player->local_player_index != NONE ||
					bots_player_is_bot(player) : absolute != wanted))
				{
					continue;
				}
				wanted = absolute;
				object_set_position(player->unit_index, &point, NULL, NULL);
				infection.placed[wanted] = TRUE;
				platform_log("infection: the test puts player %ld at (%.2f %.2f %.2f) at tick %ld", wanted, point.x,
					point.y, point.z, game_time_get());
			}
		}
	}
}

/* (the host) a survivor out of reach too long hurt: their vitality, which
the host's object updates carry to the other machines as any damage's; at
nothing, killed (nobody's kill: they come back infected) */
static void infection_out_of_reach_hurt(long unit_index)
{
	struct object_datum *object = object_get(unit_index);

	object->object.shield_vitality = 0.0f;
	object->object.shield_stun_ticks = (short)MAX(object->object.shield_stun_ticks, 2 * TICKS_PER_SECOND);
	object->object.body_vitality -= INFECTION_OUT_OF_REACH_HURT;
	if (object->object.body_vitality <= 0.001f)
	{
		object->object.body_vitality = 0.0f;
		unit_kill(unit_index);
	}
}

/* every machine, each tick of the game: each survivor's time out of reach
(a client, its own players'), and the host's hurt once it runs past the
limit, each second until they come down (or die, infected) */
static void infection_out_of_reach_update(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long limit = infection_out_of_reach_limit();
	boolean host = infection_host();
	long now = game_time_get();

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long absolute = infection_absolute(iterator.datum_index);
		struct object_datum *unit;
		real_point3d origin;
		real node_x = 0.0f, node_y = 0.0f, node_z = 0.0f;
		long reach;

		if (absolute == NONE)
			continue;
		unit = player->unit_index != NONE ? object_try_and_get(player->unit_index) : NULL;
		if (limit <= 0 || !unit || player->team_index != INFECTION_TEAM_SURVIVORS ||
			TEST_FLAG(unit->object.damage_flags, _object_dead_bit) || (!host && player->local_player_index == NONE))
		{
			infection.out_of_reach_ticks[absolute] = 0;
			infection.out_of_reach_unit[absolute] = NONE;
			continue;
		}
		if (infection.out_of_reach_unit[absolute] != player->unit_index)
		{
			infection.out_of_reach_unit[absolute] = player->unit_index;
			infection.out_of_reach_ticks[absolute] = 0;
			infection.out_of_reach_hurts[absolute] = 0;
		}
		object_get_origin(player->unit_index, &origin);
		reach = bots_board_reaches(origin.x, origin.y, origin.z, &node_x, &node_y, &node_z);
		if (!infection.reach_told && reach != NONE)
		{
			infection.reach_told = TRUE;
			platform_log("infection: out of reach is decided by the walking graph from tick %ld", now);
		}
		if (reach == 1)
		{
			if (infection.out_of_reach_ticks[absolute] > 0)
			{
				platform_log("infection: player %ld is back in reach at (%.2f %.2f %.2f) after %ld ticks at tick %ld",
					absolute, origin.x, origin.y, origin.z, infection.out_of_reach_ticks[absolute], now);
			}
			infection.out_of_reach_ticks[absolute] = 0;
			continue;
		}
		if (reach != 0)
			continue;
		if (infection.out_of_reach_ticks[absolute]++ == 0)
		{
			platform_log("infection: player %ld is out of reach at (%.2f %.2f %.2f), node (%.2f %.2f %.2f), at tick %ld",
				absolute, origin.x, origin.y, origin.z, node_x, node_y, node_z, now);
		}
		if (host && infection.out_of_reach_ticks[absolute] >= limit &&
			(infection.out_of_reach_ticks[absolute] - limit) % TICKS_PER_SECOND == 0)
		{
			real shield = unit->object.shield_vitality, body = unit->object.body_vitality;

			infection_out_of_reach_hurt(player->unit_index);
			unit = object_try_and_get(player->unit_index);
			infection.out_of_reach_hurts[absolute]++;
			platform_log("infection: player %ld hurt out of reach (%ld) at (%.2f %.2f %.2f) at tick %ld: shield %.2f to "
				"%.2f, body %.2f to %.2f%s", absolute, infection.out_of_reach_hurts[absolute], origin.x, origin.y,
				origin.z, now, shield, unit ? unit->object.shield_vitality : 0.0f, body,
				unit ? unit->object.body_vitality : 0.0f,
				!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ? ", killed" : "");
		}
	}
}

long infection_out_of_reach_ticks(long player_index)
{
	long absolute = infection_absolute(player_index);

	return absolute != NONE && infection_active() ? infection.out_of_reach_ticks[absolute] : 0;
}

/* the local player's warning: (TRUE) out of reach long enough to be told,
with the seconds left before it hurts (0: hurting) */
static boolean infection_out_of_reach_warning(long player_index, long *seconds_left)
{
	long absolute = infection_absolute(player_index);
	long limit = infection_out_of_reach_limit();
	long ticks;

	if (absolute == NONE || limit <= 0)
		return FALSE;
	ticks = infection.out_of_reach_ticks[absolute];
	if (ticks < MIN(INFECTION_OUT_OF_REACH_WARN_TICKS, limit / 2) || ticks <= 0)
		return FALSE;
	*seconds_left = ticks >= limit ? 0 : (limit - ticks + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND;
	return TRUE;
}

void infection_update(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long now = game_time_get();

	if (!infection_active())
	{
		bots_graph_wanted(FALSE);
		return;
	}
	/* (out of reach: the map's walking graph, a client's its own) */
	bots_graph_wanted(infection_out_of_reach_limit() > 0);
	/* (every machine: each player's side from their unit, which the host
	made - a client's own player's too, whose unit is not always attached as
	the others' are; and when each was first seen infected) */
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long absolute = infection_absolute(iterator.datum_index);
		struct object_datum *unit = player->unit_index != NONE ? object_try_and_get(player->unit_index) : NULL;

		if (unit && network_game_distributed_client() && player->team_index != unit->object.owner_team_index &&
			(unit->object.owner_team_index == INFECTION_TEAM_SURVIVORS ||
			unit->object.owner_team_index == INFECTION_TEAM_INFECTED))
		{
			player->team_index = unit->object.owner_team_index;
		}
		if (absolute == NONE)
			continue;

		if (player->team_index == INFECTION_TEAM_INFECTED && infection.infected_tick[absolute] == NONE)
			infection.infected_tick[absolute] = now;
		else if (player->team_index != INFECTION_TEAM_INFECTED)
			infection.infected_tick[absolute] = NONE;
	}
	/* (every machine: its own player's side, told when it changes) */
	{
		long local = local_player_get_player_index(0);
		struct player_datum *mine = local != NONE ? player_try_and_get(local) : NULL;

		if (mine && mine->team_index != infection.local_team)
		{
			infection.local_team = mine->team_index;
			platform_log("infection: this machine's player %ld is %s at tick %ld", infection_absolute(local),
				mine->team_index == INFECTION_TEAM_INFECTED ? "infected" : "a survivor", now);
		}
	}
	/* (every machine: out of reach) */
	if (game_engine_port_game_active())
		infection_out_of_reach_update();
	if (!infection_host() || !game_engine_port_game_active())
		return;

	infection_test_place();
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

	if (!infection_unit_infected(skater_unit_index))
		return share;
	percent = infection_variant()->game_engine_variant.slayer.infection_run_over;
	if (percent <= 0)
		percent = 100;
	return MIN(1.0f, share * (real)percent / 100.0f);
}

real infection_damage_taken_scale(long damaged_player_index)
{
	long percent;

	if (!infection_player_infected(damaged_player_index))
		return 1.0f;
	percent = infection_variant()->game_engine_variant.slayer.infection_damage_taken;
	return percent > 0 ? (real)percent / 100.0f : 1.0f;
}

void infection_variant_defaults(struct game_variant *variant)
{
	struct slayer_variant *slayer = &variant->game_engine_variant.slayer;

	if (!slayer->infection_run_over)
		slayer->infection_run_over = INFECTION_DEFAULT_RUN_OVER;
	if (!slayer->infection_minutes)
		slayer->infection_minutes = 5;
	if (!slayer->infection_ratio)
		slayer->infection_ratio = 8;
	if (!slayer->infection_damage_taken)
		slayer->infection_damage_taken = INFECTION_DEFAULT_DAMAGE_TAKEN;
	if (!slayer->infection_reach)
		slayer->infection_reach = INFECTION_DEFAULT_REACH;
	if (!slayer->infection_speed)
		slayer->infection_speed = INFECTION_DEFAULT_SPEED;
	if (!slayer->infection_turning)
		slayer->infection_turning = INFECTION_DEFAULT_TURNING;
	if (!slayer->infection_out_of_reach)
		slayer->infection_out_of_reach = INFECTION_DEFAULT_OUT_OF_REACH;
}

real infection_skate_time_scale(long unit_index)

{
	long percent;

	if (!infection_unit_infected(unit_index))
		return 1.0f;
	percent = infection_variant()->game_engine_variant.slayer.infection_speed;
	return percent > 0 ? (real)PIN(percent, 50, 250) / 100.0f : 1.0f;
}

real infection_skate_turn_scale(long unit_index)
{
	long percent;

	if (!infection_unit_infected(unit_index))
		return 1.0f;
	percent = infection_variant()->game_engine_variant.slayer.infection_turning;
	return percent > 0 ? (real)PIN(percent, 100, 200) / 100.0f : 1.0f;
}

real infection_board_reach(long skater_unit_index, long biped_index)
{
	struct object_datum *biped;

	if (!infection_unit_infected(skater_unit_index) || !(biped = object_try_and_get(biped_index)) ||
		biped->object.owner_player_index == NONE || biped->object.owner_team_index != INFECTION_TEAM_SURVIVORS)
	{
		return 0.0f;
	}
	return infection_variant()->game_engine_variant.slayer.infection_reach / 100.0f;
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
	/* out of reach: told in the middle of the screen, the seconds left before
	it hurts, and held while it does */
	{
		long seconds_left;
		short middle;

		if (player->team_index != INFECTION_TEAM_SURVIVORS || !infection_out_of_reach_warning(player_index, &seconds_left))
			return;
		if (seconds_left > 0)
			usnprintf(text, NUMBEROF(text) - 1, L"RETURN TO COMBAT\r\n%ld", seconds_left);
		else
			usnprintf(text, NUMBEROF(text) - 1, L"RETURN TO COMBAT");
		text[NUMBEROF(text) - 1] = 0;
		/* (red, beating once a second while it hurts) */
		color.red = 1.0f;
		color.green = 0.2f;
		color.blue = 0.15f;
		color.alpha = seconds_left > 0 ? 1.0f : 0.55f + 0.45f * (real)fabs(sin((real)game_time_get() * _pi / TICKS_PER_SECOND));
		/* (above the crosshair, clear of it) */
		bounds = render.camera.window_bounds;
		middle = (short)((bounds.y0 + bounds.y1) / 2);
		bounds.y0 = (short)(middle - 3 * height - 8);
		bounds.y1 = (short)(bounds.y0 + 2 * height + 4);
		/* (a dark shadow under it, two pixels down and right, so that it
		reads over the sky and the sand alike) */
		{
			real_argb_color shadow = { 0.8f, 0.0f, 0.0f, 0.0f };
			rectangle2d below = bounds;

			shadow.alpha = color.alpha * 0.8f;
			below.x0 += 2;
			below.x1 += 2;
			below.y0 += 2;
			below.y1 += 2;
			draw_string_set_draw_mode(font_index, NONE, 2, 0, &shadow);
			rasterizer_draw_unicode_string(&below, NULL, NULL, 0, text);
		}
		draw_string_set_draw_mode(font_index, NONE, 2, 0, &color);
		rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
	}
}

long infection_refused_presses(void)
{
	return infection.refused_ticks;
}
