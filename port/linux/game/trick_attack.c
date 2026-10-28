/* trick_attack.c: the Trick Attack game type (trick_attack.h;
port/linux/README.md's "Trick Attack")

Slayer, free for all, with the variant's trick_attack set (game_engine.h: in
bytes the Xbox's variant left unused, which travel with the network game as
they are). Everyone is always on a skateboard and unarmed (Infection's rules
for the infected, for everyone: infection.c's infection_unit_restricted).

- Tricks: what Skate 3's own scoring (the skate engine's, halo_skate_tricks)
  makes of a skater's riding. Each sequence it lands - from leaving the
  ground, a grind or a manual to rolling away clean, with its own in-sequence
  multiplier - joins the skater's combo.
- Combos: each sequence chained on raises the combo's multiplier (x2, x3 ...,
  to TRICK_ATTACK_MAXIMUM_MULTIPLIER); the combo is worth its sequences'
  points times it. It banks - into the player's score - once the skater has
  ridden on clean for the variant's bank delay after its last landing, with
  no sequence under way. A bail (or anything else that takes them off their
  board, death too) first loses it.
- Ramming (unless the variant turns it off): a skater who knocks into one
  with a combo going makes them bail (the host's verdict on the hit,
  skate_authority.c) and takes the variant's share of the combo into their
  own score.
- The hot spot: a spot of the level's main ground a board reaches and leaves
  (bots_board_spot, from the walking graph) where whatever is landed counts
  the variant's multiplier more; it moves after the variant's seconds.
- The end: the round's time (trick_minutes, 5, or the game's shorter limit);
  the highest score wins (Slayer's own winner).

The host keeps every score: its own skaters' (its players', its bots')
tricks from its engine, a client's skater's from the client in the skate
report message (each entry the client's running totals, a few times a
second, so that a lost datagram loses nothing), checked to be that machine's
player and capped. The banked scores reach every machine as Slayer's always
have (the game type's state); the combos going and the hot spot in the skate
verdict message, a few times a second. Both kinds of entry start with
TRICK_ATTACK_ENTRY_FORMAT, which older builds skip as entries of another
format: the network version is unchanged. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "render/render_camera_projection.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"
#include "interface/hud_messaging.h"
#include "game/game_globals.h"
#include "game/game_engine_king.h"
#include "physics/collisions.h"
#include "rasterizer/rasterizer_model_types.h"
#include "scenario/scenario.h"
#include "tag_files/tag_groups.h"

#include "trick_attack.h"
#include "infection.h"
#include "bots.h"
#include "network_distributed.h"
#include "skate.h"
#include "../../skate/include/halo_skate.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...);
int config_boolean(const char *name);
long config_integer(const char *name);
const char *config_string(const char *name);

/* the most points one landed sequence may bring (a report claiming more is
refused: Skate 3's biggest lines score a few tens of thousands) */
#define TRICK_ATTACK_MAXIMUM_SEQUENCE_POINTS 250000
/* the slowest knock that counts as ramming (world units a second, 3 m/s) */
#define TRICK_ATTACK_RAM_SPEED 1.0f
/* a client sends its totals every this many ticks (and at once when they
change); the host takes a client's totals as current for this long */
#define TRICK_ATTACK_REPORT_TICKS 3
#define TRICK_ATTACK_REPORT_STALE_TICKS (2 * TICKS_PER_SECOND)
/* the host sends a player's combo for this many ticks after it changes,
each combo going again every so often, and the hot spot every so often */
#define TRICK_ATTACK_ENTRY_REPEAT_TICKS 6
#define TRICK_ATTACK_ENTRY_REFRESH_TICKS TICKS_PER_SECOND
#define TRICK_ATTACK_HOT_SPOT_ENTRY_TICKS 10
/* a client forgets a combo the host has not told of lately */
#define TRICK_ATTACK_ENTRY_STALE_TICKS (3 * TICKS_PER_SECOND)
/* a callout's ticks on screen */
#define TRICK_ATTACK_CALLOUT_TICKS (3 * TICKS_PER_SECOND)
/* the hot spot moves at least this far (world units, level) */
#define TRICK_ATTACK_HOT_SPOT_MOVE 20.0f

/* what the infected may not do (infection.c's, units.h's control bits) */
#define TRICK_ATTACK_REFUSED_CONTROLS (FLAG(_unit_control_action_bit) | FLAG(_unit_control_use_equipment_bit) | \
	FLAG(_unit_control_weapon_reload_bit) | FLAG(_unit_control_weapon_primary_trigger_bit) | \
	FLAG(_unit_control_weapon_secondary_trigger_bit) | FLAG(_unit_control_throw_grenade_bit) | \
	FLAG(_unit_control_swap_weapons_bit))

enum
{
	_trick_attack_entry_combo = 1,
	_trick_attack_entry_hot_spot = 2,
};

enum
{
	_trick_attack_report_valid_bit,
	_trick_attack_report_sequence_bit,
};

struct trick_attack_player
{
	/* (the host) the skater's running totals as last taken (from its engine
	or its client's report), and the ones scored up to */
	boolean have_totals;
	long totals_time;
	long generation;
	unsigned long sequences, points, bails, landed;
	long live_tricks;
	boolean based;
	long based_generation;
	unsigned long based_sequences, based_points, based_bails, based_landed;
	/* (the host) the combo going: its sequences' points (the hot spot's
	already counted), sequences and tricks; the tick of its last landing;
	whether they rode last tick, and their unit then */
	long combo_points;
	long combo_sequences;
	long combo_tricks;
	long last_land_tick;
	boolean riding;
	long unit_index;
	/* (the host) the disarmed unit */
	long disarmed_unit;
	/* (the host) for the log at the end */
	long banked_combos, best_combo, lost_combos, lost_points, rams, stolen_points, hot_spot_sequences;
	/* (every machine: the host's word) the combo's worth and multiplier, the
	banks counted (a byte, round) and the last one's points */
	long unbanked;
	long multiplier;
	byte bank_number;
	long last_bank_points;
	/* (the host) what was last sent and until when it is sent again; (a
	client) when the host last told of it, and the banks seen */
	long sent_unbanked, sent_multiplier;
	byte sent_bank_number;
	long send_until;
	long next_refresh;
	long told_time;
	boolean told;
};

static struct
{
	struct trick_attack_player players[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	boolean started;
	boolean over_logged;
	/* the hot spot (every machine: the host's word) */
	boolean hot_valid;
	byte hot_number;
	real hot_x, hot_y, hot_z;
	long hot_until;
	real hot_multiplier;
	long hot_next_try;
	long hot_told_time;
	/* a callout (a banked combo past trick_attack_mark_points) */
	wchar_t callout[64];
	long callout_until;
	/* (a client) its skater's totals: a new count each time they start
	afresh, and when last sent */
	long client_generation;
	unsigned long client_sequences;
	int client_session;
	long client_unit;
	long client_sent_time;
	unsigned long client_sent_sequences, client_sent_bails;
	long client_sent_live;
	/* (the host, tests) rules broken: an armed or vehicle-bound skater */
	long breaches;
	long next_report_tick;
	long refused_reports;
	/* (debug.trick_test_place) the players put where the test wants */
	boolean placed[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
} trick_attack;

static void trick_attack_hud_log(void);

/* ---------- small things */

static struct game_variant const *trick_attack_variant(void)
{
	return game_engine_get_variant();
}

static struct slayer_variant const *trick_attack_slayer(void)
{
	return &game_engine_get_variant()->game_engine_variant.slayer;
}

boolean trick_attack_active(void)
{
	struct game_variant const *variant;

	if (!game_engine_running())
		return FALSE;
	variant = trick_attack_variant();
	return variant->game_engine_index == game_engine_slayer && variant->game_engine_variant.slayer.trick_attack;
}

static boolean trick_attack_host(void)
{
	return !network_game_distributed_client();
}

static long trick_attack_absolute(long player_index)
{
	long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);

	return player_index != NONE && absolute >= 0 && absolute < HALO_PORT_MAXIMUM_NETWORK_PLAYERS ? absolute : NONE;
}

/* the player of an absolute index, NONE for none */
static long trick_attack_player_of(long absolute)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index) == absolute)
			return iterator.datum_index;
	}
	return NONE;
}

static void trick_attack_put_long(byte *out, unsigned long value)
{
	out[0] = (byte)value;
	out[1] = (byte)(value >> 8);
	out[2] = (byte)(value >> 16);
	out[3] = (byte)(value >> 24);
}

static unsigned long trick_attack_get_long(byte const *data)
{
	return (unsigned long)data[0] | ((unsigned long)data[1] << 8) | ((unsigned long)data[2] << 16) |
		((unsigned long)data[3] << 24);
}

static void trick_attack_put_real(byte *out, real value)
{
	union { real r; unsigned long u; } bits;

	bits.u = 0;
	bits.r = value;
	trick_attack_put_long(out, bits.u);
}

static real trick_attack_get_real(byte const *data)
{
	union { real r; unsigned long u; } bits;

	bits.u = trick_attack_get_long(data);
	return bits.r;
}

/* points with thousands apart ("12,400") */
static void trick_attack_points_text(long points, wchar_t *text, long size)
{
	char digits[16];
	wchar_t out[24];
	long length, index, written = 0;
	boolean negative = points < 0;

	snprintf(digits, sizeof(digits), "%ld", negative ? -points : points);
	length = (long)strlen(digits);
	if (negative)
		out[written++] = L'-';
	for (index = 0; index < length; index++)
	{
		if (index > 0 && (length - index) % 3 == 0)
			out[written++] = L',';
		out[written++] = (wchar_t)digits[index];
	}
	out[written] = 0;
	ustrncpy(text, out, size - 1);
	text[size - 1] = 0;
}

/* the player's name as a wide string */
static void trick_attack_name(struct player_datum const *player, wchar_t *name)
{
	short index;

	for (index = 0; index < (short)NUMBEROF(player->name); index++)
		name[index] = player->name[index];
	name[NUMBEROF(player->name)] = 0;
}

/* ---------- the variant */

void trick_attack_variant_defaults(struct game_variant *variant)
{
	struct slayer_variant *slayer = &variant->game_engine_variant.slayer;

	if (!slayer->trick_minutes)
		slayer->trick_minutes = TRICK_ATTACK_DEFAULT_MINUTES;
	if (!slayer->trick_bank_delay)
		slayer->trick_bank_delay = TRICK_ATTACK_DEFAULT_BANK_DELAY;
	if (!slayer->trick_steal)
		slayer->trick_steal = TRICK_ATTACK_DEFAULT_STEAL;
	if (!slayer->trick_hot_spot_multiplier)
		slayer->trick_hot_spot_multiplier = TRICK_ATTACK_DEFAULT_HOT_SPOT_MULTIPLIER;
	if (!slayer->trick_hot_spot_seconds)
		slayer->trick_hot_spot_seconds = TRICK_ATTACK_DEFAULT_HOT_SPOT_SECONDS;
}

long trick_attack_time_limit_ticks(long time_limit_minutes)
{
	long minutes = trick_attack_slayer()->trick_minutes;

	if (minutes <= 0)
		minutes = TRICK_ATTACK_DEFAULT_MINUTES;
	if (time_limit_minutes > 0)
		minutes = MIN(minutes, time_limit_minutes);
	return minutes * 60L * TICKS_PER_SECOND;
}

long trick_attack_bank_ticks(void)
{
	long tenths = trick_attack_slayer()->trick_bank_delay;

	if (tenths <= 0)
		tenths = TRICK_ATTACK_DEFAULT_BANK_DELAY;
	return MAX(1, tenths * TICKS_PER_SECOND / 10);
}

static long trick_attack_steal_percent(void)
{
	long percent = trick_attack_slayer()->trick_steal;

	return percent > 0 ? MIN(percent, 100) : TRICK_ATTACK_DEFAULT_STEAL;
}

static real trick_attack_hot_spot_multiplier(void)
{
	long tenths = trick_attack_slayer()->trick_hot_spot_multiplier;

	if (tenths <= 0)
		tenths = TRICK_ATTACK_DEFAULT_HOT_SPOT_MULTIPLIER;
	return (real)PIN(tenths, 10, 100) / 10.0f;
}

static long trick_attack_hot_spot_ticks(void)
{
	long seconds = trick_attack_slayer()->trick_hot_spot_seconds;

	if (seconds <= 0)
		seconds = TRICK_ATTACK_DEFAULT_HOT_SPOT_SECONDS;
	return seconds * TICKS_PER_SECOND;
}

/* ---------- the rules (infection.c's for the infected) */

boolean trick_attack_unit_restricted(long unit_index)
{
	struct object_datum *object;

	if (unit_index == NONE || !trick_attack_active() || !(object = object_try_and_get(unit_index)))
		return FALSE;
	return object->object.type == _object_type_biped && object->object.owner_player_index != NONE &&
		!TEST_FLAG(object->object.damage_flags, _object_dead_bit);
}

real trick_attack_run_over_share(real share)
{
	return trick_attack_active() ? 0.0f : share;
}

/* (the host) each skater's weapons and grenades taken once, as their unit
spawns (they pick up nothing); and (every few seconds) the rules checked */
static void trick_attack_disarm_and_check(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long now = game_time_get();
	boolean report = now >= trick_attack.next_report_tick;
	long on_boards = 0, players = 0;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long absolute = trick_attack_absolute(iterator.datum_index);
		struct unit_datum *unit;
		short index;
		long weapons = 0, grenades = 0;

		if (absolute == NONE || player->unit_index == NONE || !trick_attack_unit_restricted(player->unit_index))
			continue;
		unit = (struct unit_datum *)object_get(player->unit_index);
		if (trick_attack.players[absolute].disarmed_unit != player->unit_index)
		{
			trick_attack.players[absolute].disarmed_unit = player->unit_index;
			for (index = 0; index < MAXIMUM_WEAPONS_PER_UNIT; index++)
			{
				if (unit->unit.weapon_object_indices[index] != NONE)
				{
					unit_delete_all_weapons(player->unit_index);
					break;
				}
			}
			for (index = 0; index < NUMBER_OF_UNIT_GRENADE_TYPES; index++)
				unit->unit.grenade_counts[index] = 0;
			continue;
		}
		for (index = 0; index < MAXIMUM_WEAPONS_PER_UNIT; index++)
			weapons += unit->unit.weapon_object_indices[index] != NONE;
		for (index = 0; index < NUMBER_OF_UNIT_GRENADE_TYPES; index++)
			grenades += unit->unit.grenade_counts[index];
		players++;
		on_boards += skate_unit_is_skating(player->unit_index) != FALSE;
		if (weapons || grenades || unit->object.parent_object_index != NONE ||
			(unit->unit.control_flags & TRICK_ATTACK_REFUSED_CONTROLS))
		{
			trick_attack.breaches++;
			platform_log("trick attack: RULE BROKEN: player %ld has %ld weapons, %ld grenades, %s, controls %lx at tick %ld",
				absolute, weapons, grenades, unit->object.parent_object_index != NONE ? "in a vehicle" : "on foot",
				(unsigned long)unit->unit.control_flags, now);
		}
	}
	if (report)
	{
		platform_log("trick attack: at tick %ld: %ld of %ld players on boards, %ld rules broken, %ld reports refused, "
			"presses refused %ld times", now, on_boards, players, trick_attack.breaches, trick_attack.refused_reports,
			infection_refused_presses());
		trick_attack.next_report_tick = now + 10 * TICKS_PER_SECOND;
	}
}

/* ---------- the game */

static void trick_attack_reset_player(struct trick_attack_player *state)
{
	memset(state, 0, sizeof(*state));
	state->unit_index = NONE;
	state->disarmed_unit = NONE;
	state->last_land_tick = NONE;
}

void trick_attack_game_starting(void)
{
	short index;

	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		trick_attack_reset_player(&trick_attack.players[index]);
		trick_attack.placed[index] = FALSE;
	}
	trick_attack.started = TRUE;
	trick_attack.over_logged = FALSE;
	trick_attack.hot_valid = FALSE;
	trick_attack.hot_number = 0;
	trick_attack.hot_until = 0;
	trick_attack.hot_next_try = 0;
	trick_attack.callout[0] = 0;
	trick_attack.callout_until = 0;
	trick_attack.client_generation = 0;
	trick_attack.client_sequences = 0;
	trick_attack.client_session = -1;
	trick_attack.client_unit = NONE;
	trick_attack.client_sent_time = NONE;
	trick_attack.breaches = 0;
	trick_attack.next_report_tick = 0;
	trick_attack.refused_reports = 0;
	if (!trick_attack_active())
		return;
	{
		struct slayer_variant const *slayer = trick_attack_slayer();

		platform_log("trick attack: the variant: round %ld seconds, bank delay %ld ticks, steal %ld%%, hot spot x%.1f for "
			"%ld seconds, ramming %s", trick_attack_time_limit_ticks(game_variant_options_get()->time_limit) /
			TICKS_PER_SECOND, trick_attack_bank_ticks(), trick_attack_steal_percent(),
			trick_attack_hot_spot_multiplier(), trick_attack_hot_spot_ticks() / TICKS_PER_SECOND,
			slayer->trick_no_ramming ? "off" : "on");
	}
}

/* the combo's worth now: its points times one for each trick chained (to
TRICK_ATTACK_MAXIMUM_MULTIPLIER) */
static void trick_attack_combo_worth(struct trick_attack_player *state)
{
	state->multiplier = state->combo_sequences > 0 ?
		MIN(MAX(state->combo_tricks, state->combo_sequences), TRICK_ATTACK_MAXIMUM_MULTIPLIER) : 0;
	state->unbanked = state->combo_points * state->multiplier;
}

long trick_attack_mark_points(void)
{
	long points = config_integer("debug.trick_mark_points");

	return points > 0 ? points : TRICK_ATTACK_MARK_POINTS;
}

static void trick_attack_callout(struct player_datum const *player, long points)
{
	wchar_t name[16];
	wchar_t text[24];

	trick_attack_name(player, name);
	trick_attack_points_text(points, text, NUMBEROF(text));
	usnprintf(trick_attack.callout, NUMBEROF(trick_attack.callout) - 1, L"%ls landed a %ls combo", name, text);
	trick_attack.callout[NUMBEROF(trick_attack.callout) - 1] = 0;
	trick_attack.callout_until = game_time_get() + TRICK_ATTACK_CALLOUT_TICKS;
}

/* (the host) the combo lost (a bail, a ram, death) */
static void trick_attack_lose(long player_index, struct trick_attack_player *state, char const *why)
{
	if (state->combo_sequences <= 0)
		return;
	trick_attack_combo_worth(state);
	state->lost_combos++;
	state->lost_points += state->unbanked;
	platform_log("trick attack: player %ld %s and loses a combo of %ld sequences (%ld tricks) worth %ld at tick %ld",
		trick_attack_absolute(player_index), why, state->combo_sequences, state->combo_tricks, state->unbanked,
		game_time_get());
	state->combo_points = 0;
	state->combo_sequences = 0;
	state->combo_tricks = 0;
	state->last_land_tick = NONE;
	trick_attack_combo_worth(state);
}

/* (the host) the combo banked into the player's score */
static void trick_attack_bank(long player_index, struct trick_attack_player *state)
{
	long points;

	trick_attack_combo_worth(state);
	points = state->unbanked;
	game_engine_slayer_port_add_score(player_index, points);
	state->banked_combos++;
	state->best_combo = MAX(state->best_combo, points);
	state->bank_number++;
	state->last_bank_points = points;
	platform_log("trick attack: player %ld banks a combo of %ld sequences (%ld tricks) x%ld for %ld points, score %ld, "
		"at tick %ld", trick_attack_absolute(player_index), state->combo_sequences, state->combo_tricks,
		state->multiplier, points, game_engine_port_player_score(player_index), game_time_get());
	if (points >= trick_attack_mark_points())
		trick_attack_callout(player_get(player_index), points);
	state->combo_points = 0;
	state->combo_sequences = 0;
	state->combo_tricks = 0;
	state->last_land_tick = NONE;
	trick_attack_combo_worth(state);
}

/* ---------- the hot spot's boundary

Drawn round the hot spot on every machine as King of the Hill draws its hill
(game_engine_king.c's king_engine_post_rasterize: a band of the
multiplayer globals' hill shader, 0.8 world units tall, along each edge of
the hill's outline), in the same shader. The outline is a regular polygon
of TRICK_ATTACK_HOT_SPOT_SIDES corners TRICK_ATTACK_HOT_SPOT_RADIUS from the
hot spot's centre, and what counts as in the hot spot is that polygon seen
from above, the very edges drawn (and, as before, within
TRICK_ATTACK_HOT_SPOT_HEIGHT above or below the centre): the corners follow
from the centre alone, so every machine has the host's outline from the
centre the host sends. Each corner is drawn on the floor under it. */

static struct
{
	boolean valid;
	real x, y, z;
	/* the corners, counterclockwise from above; drawn on the floor */
	real_point3d corners[TRICK_ATTACK_HOT_SPOT_SIDES];
	/* (debug.trick_hud_log) the last hot spot whose boundary was logged */
	byte logged_number;
	boolean logged;
	real logged_x, logged_y;
} trick_attack_boundary;

/* the boundary of the hot spot where it is now */
static boolean trick_attack_boundary_update(void)
{
	short corner;

	if (!trick_attack.hot_valid)
	{
		trick_attack_boundary.valid = FALSE;
		return FALSE;
	}
	if (trick_attack_boundary.valid && trick_attack_boundary.x == trick_attack.hot_x &&
		trick_attack_boundary.y == trick_attack.hot_y && trick_attack_boundary.z == trick_attack.hot_z)
	{
		return TRUE;
	}
	for (corner = 0; corner < TRICK_ATTACK_HOT_SPOT_SIDES; corner++)
	{
		real angle = (real)corner * (2.f * _pi / TRICK_ATTACK_HOT_SPOT_SIDES);
		real_point3d *point = &trick_attack_boundary.corners[corner];
		real_point3d start;
		real_vector3d down;
		struct collision_result collision;

		point->x = trick_attack.hot_x + TRICK_ATTACK_HOT_SPOT_RADIUS * (real)cos(angle);
		point->y = trick_attack.hot_y + TRICK_ATTACK_HOT_SPOT_RADIUS * (real)sin(angle);
		point->z = trick_attack.hot_z;
		/* (on the floor under it, from a little above the centre's height:
		the band stands on the ground a slope or a step puts it on) */
		start.x = point->x;
		start.y = point->y;
		start.z = trick_attack.hot_z + 1.f;
		down.i = 0.f;
		down.j = 0.f;
		down.k = -2.f * TRICK_ATTACK_HOT_SPOT_HEIGHT;
		if (collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit),
			&start, &down, NONE, &collision))
		{
			point->z = collision.point.z;
		}
	}
	trick_attack_boundary.x = trick_attack.hot_x;
	trick_attack_boundary.y = trick_attack.hot_y;
	trick_attack_boundary.z = trick_attack.hot_z;
	trick_attack_boundary.valid = TRUE;
	return TRUE;
}

/* whether a point is in the hot spot: inside its outline from above (on an
edge counts) and within TRICK_ATTACK_HOT_SPOT_HEIGHT of its centre */
static boolean trick_attack_in_hot_spot_at(real_point3d const *point)
{
	short corner;

	if (!trick_attack_boundary_update() || !(fabs(point->z - trick_attack.hot_z) <= TRICK_ATTACK_HOT_SPOT_HEIGHT))
		return FALSE;
	for (corner = 0; corner < TRICK_ATTACK_HOT_SPOT_SIDES; corner++)
	{
		real_point3d const *a = &trick_attack_boundary.corners[corner];
		real_point3d const *b = &trick_attack_boundary.corners[(corner + 1) % TRICK_ATTACK_HOT_SPOT_SIDES];

		/* (left of each edge, counterclockwise) */
		if ((b->x - a->x) * (point->y - a->y) - (b->y - a->y) * (point->x - a->x) < 0.f)
			return FALSE;
	}
	return TRUE;
}

/* whether the player's unit is in the hot spot */
static boolean trick_attack_in_hot_spot(long unit_index)
{
	real_point3d origin;

	if (!trick_attack.hot_valid || unit_index == NONE || !object_try_and_get(unit_index))
		return FALSE;
	object_get_origin(unit_index, &origin);
	return trick_attack_in_hot_spot_at(&origin);
}

/* (debug.trick_hud_log) the boundary drawn, once a hot spot: its corners'
reach from the centre, and points either side of each edge's middle and
each corner tried as the scoring tries a skater (trick_attack_in_hot_spot_at),
so that a test sees the area drawn is the area scored */
static void trick_attack_boundary_log(void)
{
	short corner;
	long inside = 0, outside = 0;
	real nearest = 1.0e9f, farthest = 0.f;

	if (!config_boolean("debug.trick_hud_log") ||
		(trick_attack_boundary.logged && trick_attack_boundary.logged_number == trick_attack.hot_number &&
			trick_attack_boundary.logged_x == trick_attack.hot_x && trick_attack_boundary.logged_y == trick_attack.hot_y))
	{
		return;
	}
	trick_attack_boundary.logged = TRUE;
	trick_attack_boundary.logged_number = trick_attack.hot_number;
	trick_attack_boundary.logged_x = trick_attack.hot_x;
	trick_attack_boundary.logged_y = trick_attack.hot_y;
	for (corner = 0; corner < TRICK_ATTACK_HOT_SPOT_SIDES; corner++)
	{
		real_point3d const *a = &trick_attack_boundary.corners[corner];
		real_point3d const *b = &trick_attack_boundary.corners[(corner + 1) % TRICK_ATTACK_HOT_SPOT_SIDES];
		real dx = a->x - trick_attack.hot_x, dy = a->y - trick_attack.hot_y;
		real reach = (real)sqrt(dx * dx + dy * dy);
		real_point3d middle, probe;
		short side;

		nearest = MIN(nearest, reach);
		farthest = MAX(farthest, reach);
		middle.x = (a->x + b->x) * 0.5f;
		middle.y = (a->y + b->y) * 0.5f;
		for (side = 0; side < 4; side++)
		{
			/* the edge's middle and the corner, 1% in and 1% out */
			real_point3d const *at = side < 2 ? &middle : a;
			real scale = side % 2 ? 1.01f : 0.99f;

			probe.x = trick_attack.hot_x + (at->x - trick_attack.hot_x) * scale;
			probe.y = trick_attack.hot_y + (at->y - trick_attack.hot_y) * scale;
			probe.z = trick_attack.hot_z;
			if (trick_attack_in_hot_spot_at(&probe) == !(side % 2))
			{
				if (side % 2)
					outside++;
				else
					inside++;
			}
		}
	}
	platform_log("trick attack: the hot spot %d's boundary drawn at (%.2f %.2f %.2f): %d sides %.3f to %.3f from its "
		"centre, %ld of %d points just inside it scored in, %ld of %d just outside scored out",
		trick_attack.hot_number, trick_attack.hot_x, trick_attack.hot_y, trick_attack.hot_z, TRICK_ATTACK_HOT_SPOT_SIDES,
		nearest, farthest, inside, 2 * TRICK_ATTACK_HOT_SPOT_SIDES, outside, 2 * TRICK_ATTACK_HOT_SPOT_SIDES);
}

/* slayer_engine_post_rasterize (game_engine_slayer.c), every machine: the
hot spot's boundary drawn as King of the Hill's hill is */
void trick_attack_post_rasterize(void)
{
	struct game_globals *game_globals;
	struct game_globals_multiplayer_information *multiplayer_information;
	real perimeter = 0.f, inverse_segment_count, texels_per_unit, previous_u = 0.f, accumulated_distance = 0.f;
	short corner;

	if (!trick_attack_active() || !trick_attack_boundary_update())
		return;
	game_globals = scenario_get_game_globals();
	if (!game_globals || game_globals->multiplayer_information.count < 1)
		return;
	multiplayer_information = TAG_BLOCK_GET_ELEMENT(&game_globals->multiplayer_information, 0,
		struct game_globals_multiplayer_information);
	if (multiplayer_information->hill_shader.index == NONE)
		return;
	trick_attack_boundary_log();
	/* (as king_engine_post_rasterize: the shader's texture repeated once a
	world unit or so round the outline) */
	for (corner = 0; corner < TRICK_ATTACK_HOT_SPOT_SIDES; corner++)
		perimeter += distance3d(&trick_attack_boundary.corners[corner],
			&trick_attack_boundary.corners[(corner + 1) % TRICK_ATTACK_HOT_SPOT_SIDES]);
	inverse_segment_count = (real)(1.0 / MAX(floor(perimeter + 0.5f), 1.0));
	texels_per_unit = 1.0f / (inverse_segment_count * perimeter);
	render_dynamic_quad_initialize();
	for (corner = 0; corner < TRICK_ATTACK_HOT_SPOT_SIDES; corner++)
	{
		struct model_vertex_uncompressed vertices[NUMBER_OF_VERTICES_PER_QUADRILATERAL];
		real_point3d const *a = &trick_attack_boundary.corners[corner];
		real_point3d const *b = &trick_attack_boundary.corners[(corner + 1) % TRICK_ATTACK_HOT_SPOT_SIDES];
		real_vector3d sides[2];
		real_vector3d normal;
		real edge_length = distance3d(a, b);
		real edge_end_u = (edge_length + accumulated_distance) * texels_per_unit;

		accumulated_distance += edge_length;
		csmemset(vertices, 0, sizeof(vertices));
		vertices[0].position = *a;
		vertices[1].position = *a;
		vertices[1].position.z += TRICK_ATTACK_HOT_SPOT_BAND;
		vertices[2].position = *b;
		vertices[2].position.z += TRICK_ATTACK_HOT_SPOT_BAND;
		vertices[3].position = *b;
		vector_from_points3d(&vertices[0].position, &vertices[1].position, &sides[0]);
		vector_from_points3d(&vertices[1].position, &vertices[2].position, &sides[1]);
		cross_product3d(&sides[0], &sides[1], &normal);
		normalize3d(&normal);
		vertices[0].normal = vertices[1].normal = vertices[2].normal = vertices[3].normal = normal;
		vertices[0].texcoord.x = previous_u * inverse_segment_count;
		vertices[0].texcoord.y = 1.0f;
		vertices[1].texcoord.x = previous_u * inverse_segment_count;
		vertices[1].texcoord.y = 0.2f;
		vertices[2].texcoord.x = edge_end_u * inverse_segment_count;
		vertices[2].texcoord.y = 0.2f;
		vertices[3].texcoord.x = edge_end_u * inverse_segment_count;
		vertices[3].texcoord.y = 1.0f;
		previous_u = edge_end_u;
		render_dynamic_quad(vertices, multiplayer_information->hill_shader.index, NULL, NULL,
			1.0f / inverse_segment_count, 1.0f);
	}
}

/* whether the player's skater rides (on the board, not bailing), as the
host has them: its own skaters by its engine, a client's by their state */
static boolean trick_attack_riding(long absolute, long unit_index)
{
	if (unit_index == NONE)
		return FALSE;
	if (skate_host_skater(unit_index))
		return skate_host_skater_riding(unit_index);
	return skate_network_player_riding((short)absolute) && skate_network_unit_skating(unit_index);
}

/* (the host) one player's tick: their totals taken, what they landed added
to the combo, a bail losing it, a clean ride banking it */
static void trick_attack_score_player(long player_index, struct player_datum *player)
{
	long absolute = trick_attack_absolute(player_index);
	struct trick_attack_player *state = &trick_attack.players[absolute];
	long now = game_time_get();
	long unit_index = player->unit_index;
	boolean alive = unit_index != NONE && object_try_and_get(unit_index) &&
		!TEST_FLAG(object_get(unit_index)->object.damage_flags, _object_dead_bit);
	boolean riding;

	/* (its own skaters' totals from its engine) */
	if (alive && skate_host_skater(unit_index))
	{
		struct halo_skate_tricks tricks;
		int session;

		if (skate_unit_tricks(unit_index, &tricks, &session))
		{
			state->have_totals = TRUE;
			state->totals_time = now;
			state->generation = session;
			state->sequences = tricks.sequences;
			state->points = (unsigned long)(tricks.points > 0.0f ? tricks.points + 0.5f : 0.0f);
			state->bails = tricks.bails;
			state->landed = tricks.landed;
			state->live_tricks = (long)tricks.sequence_tricks;
		}
	}
	riding = alive && trick_attack_riding(absolute, unit_index);
	/* (a new unit, or dead: whatever was going is lost) */
	if (state->unit_index != unit_index || !alive)
	{
		trick_attack_lose(player_index, state, alive ? "respawns" : "dies");
		state->unit_index = alive ? unit_index : NONE;
		state->riding = FALSE;
	}
	if (state->have_totals && now - state->totals_time <= TRICK_ATTACK_REPORT_STALE_TICKS)
	{
		/* (counting afresh - a new session, a new life's: the totals so far
		are the start) */
		if (!state->based || state->generation != state->based_generation || state->sequences < state->based_sequences ||
			state->points < state->based_points || state->bails < state->based_bails)
		{
			state->based = TRUE;
			state->based_generation = state->generation;
			state->based_sequences = state->sequences;
			state->based_points = state->points;
			state->based_bails = state->bails;
			state->based_landed = state->landed;
		}
		if (state->bails > state->based_bails)
		{
			state->based_bails = state->bails;
			trick_attack_lose(player_index, state, "bails");
		}
		if (state->sequences > state->based_sequences)
		{
			unsigned long sequences = state->sequences - state->based_sequences;
			unsigned long points = state->points - state->based_points;
			long tricks = (long)(state->landed - state->based_landed);

			state->based_sequences = state->sequences;
			state->based_points = state->points;
			state->based_landed = state->landed;
			if (points > (unsigned long)TRICK_ATTACK_MAXIMUM_SEQUENCE_POINTS * sequences)
			{
				trick_attack.refused_reports++;
				platform_log("trick attack: player %ld's %lu sequences for %lu points refused (too many) at tick %ld",
					absolute, sequences, points, now);
			}
			else if (alive)
			{
				boolean hot = trick_attack_in_hot_spot(unit_index);
				long worth = (long)points;

				if (hot)
				{
					worth = (long)((real)points * trick_attack_hot_spot_multiplier() + 0.5f);
					state->hot_spot_sequences++;
				}
				state->combo_points += worth;
				state->combo_sequences += (long)sequences;
				state->combo_tricks += MAX(tricks, 0);
				state->last_land_tick = now;
				trick_attack_combo_worth(state);
				platform_log("trick attack: player %ld lands %ld tricks for %ld points%s: combo of %ld sequences x%ld worth "
					"%ld at tick %ld", absolute, MAX(tricks, 0), worth, hot ? " (in the hot spot)" : "",
					state->combo_sequences, state->multiplier, state->unbanked, now);
			}
		}
	}
	/* (off the board - a bail the engine had no sequence for, a fall) */
	if (state->riding && !riding)
		trick_attack_lose(player_index, state, "comes off the board");
	state->riding = riding;
	/* (ridden on clean long enough, nothing under way: banked) */
	if (state->combo_sequences > 0 && riding && state->live_tricks == 0 && state->last_land_tick != NONE &&
		now - state->last_land_tick >= trick_attack_bank_ticks())
	{
		trick_attack_bank(player_index, state);
	}
	trick_attack_combo_worth(state);
}

/* (debug.trick_test_place, the host) each player the test names put where
it says, once, as their first unit is in the game */
static void trick_attack_test_place(void)
{
	char const *list = config_string("debug.trick_test_place");

	if (!list || !list[0])
		return;
	while (*list)
	{
		char *end;
		long wanted = strtol(list, &end, 10);
		real_point3d point;
		long player_index;
		struct player_datum *player;

		if (end == list || *end != ':')
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
		if (wanted < 0 || wanted >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS || trick_attack.placed[wanted] ||
			(player_index = trick_attack_player_of(wanted)) == NONE)
		{
			continue;
		}
		player = player_get(player_index);
		if (player->unit_index == NONE || !object_try_and_get(player->unit_index))
			continue;
		object_set_position(player->unit_index, &point, NULL, NULL);
		trick_attack.placed[wanted] = TRUE;
		platform_log("trick attack: the test puts player %ld at (%.2f %.2f %.2f) at tick %ld", wanted, point.x, point.y,
			point.z, game_time_get());
	}
}

/* (the host) the hot spot placed, and moved when its time is up */
static void trick_attack_hot_spot_update(void)
{
	long now = game_time_get();
	real x = 0.0f, y = 0.0f, z = 0.0f;
	unsigned long pick;
	long count;

	if (trick_attack.hot_valid && now < trick_attack.hot_until)
		return;
	if (now < trick_attack.hot_next_try)
		return;
	pick = (unsigned long)seed_random_range(get_global_random_seed_address(), 0, 32767) * 32768UL +
		(unsigned long)seed_random_range(get_global_random_seed_address(), 0, 32767);
	count = bots_board_spot(pick, trick_attack.hot_valid ? trick_attack.hot_x : 1.0e6f,
		trick_attack.hot_valid ? trick_attack.hot_y : 1.0e6f, trick_attack.hot_valid ? TRICK_ATTACK_HOT_SPOT_MOVE : 0.0f,
		&x, &y, &z);
	/* (debug.trick_hot_spot: the test's first) */
	if (count > 0 && !trick_attack.hot_valid && config_string("debug.trick_hot_spot") &&
		config_string("debug.trick_hot_spot")[0])
	{
		char const *text = config_string("debug.trick_hot_spot");
		char *end;

		x = (real)strtod(text, &end);
		text = *end == ',' ? end + 1 : end;
		y = (real)strtod(text, &end);
		text = *end == ',' ? end + 1 : end;
		z = (real)strtod(text, &end);
	}
	if (count <= 0)
	{
		/* (no graph yet: a second later) */
		trick_attack.hot_next_try = now + TICKS_PER_SECOND;
		return;
	}
	trick_attack.hot_valid = TRUE;
	trick_attack.hot_number++;
	trick_attack.hot_x = x;
	trick_attack.hot_y = y;
	trick_attack.hot_z = z;
	trick_attack.hot_until = now + trick_attack_hot_spot_ticks();
	trick_attack.hot_multiplier = trick_attack_hot_spot_multiplier();
	trick_attack.hot_told_time = now;
	platform_log("trick attack: hot spot %d at (%.2f %.2f %.2f), one of %ld a board reaches (reaches %ld), x%.1f for %ld "
		"seconds, at tick %ld", trick_attack.hot_number, x, y, z, count, bots_board_reaches(x, y, z, NULL, NULL, NULL),
		trick_attack.hot_multiplier, trick_attack_hot_spot_ticks() / TICKS_PER_SECOND, now);
}

/* (a client) its skater's totals: a new generation when they start afresh */
static void trick_attack_client_update(void)
{
	long unit_index = skate_local_skater_unit();
	struct halo_skate_tricks tricks;
	int session;

	if (unit_index == NONE || !skate_unit_tricks(unit_index, &tricks, &session))
		return;
	if (session != trick_attack.client_session || unit_index != trick_attack.client_unit ||
		tricks.sequences < trick_attack.client_sequences)
	{
		trick_attack.client_generation = (trick_attack.client_generation + 1) & 0xFF;
		trick_attack.client_session = session;
		trick_attack.client_unit = unit_index;
	}
	trick_attack.client_sequences = tricks.sequences;
}

void trick_attack_update(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long now = game_time_get();

	if (!trick_attack_active())
		return;
	trick_attack_hud_log();
	/* (the hot spot: the map's walking graph, on the host) */
	if (trick_attack_host())
		bots_graph_wanted(TRUE);
	if (!game_engine_port_game_active())
		return;
	if (!trick_attack_host())
	{
		short index;

		trick_attack_client_update();
		/* (combos the host has stopped telling of: gone) */
		for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
		{
			struct trick_attack_player *state = &trick_attack.players[index];

			if (state->told && now - state->told_time > TRICK_ATTACK_ENTRY_STALE_TICKS)
			{
				state->unbanked = 0;
				state->multiplier = 0;
			}
		}
		if (trick_attack.hot_valid && now - trick_attack.hot_told_time > TRICK_ATTACK_ENTRY_STALE_TICKS)
			trick_attack.hot_valid = FALSE;
		return;
	}
	trick_attack_test_place();
	trick_attack_hot_spot_update();
	trick_attack_disarm_and_check();
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (trick_attack_absolute(iterator.datum_index) == NONE || player->quit_out_of_game)
			continue;
		trick_attack_score_player(iterator.datum_index, player);
	}
}

void trick_attack_game_over(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long best = NONE;
	long best_score = 0;

	if (!trick_attack_active() || !trick_attack_host() || trick_attack.over_logged)
		return;
	trick_attack.over_logged = TRUE;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long absolute = trick_attack_absolute(iterator.datum_index);
		struct trick_attack_player const *state;
		long score = game_engine_port_player_score(iterator.datum_index);

		if (absolute == NONE)
			continue;
		state = &trick_attack.players[absolute];
		platform_log("trick attack: player %ld scores %ld: %ld combos banked (best %ld), %ld lost (%ld points), %ld rams "
			"(%ld points taken), %ld sequences in the hot spot", absolute, score, state->banked_combos, state->best_combo,
			state->lost_combos, state->lost_points, state->rams, state->stolen_points, state->hot_spot_sequences);
		if (best == NONE || score > best_score)
		{
			best = absolute;
			best_score = score;
		}
	}
	platform_log("trick attack: the game is over at tick %ld: player %ld wins with %ld points, %d hot spots, %ld rules "
		"broken", game_time_get(), best, best_score, trick_attack.hot_number, trick_attack.breaches);
}

/* ---------- ramming */

void trick_attack_ram(long rammer_absolute, long target_absolute, real closing, boolean *rammer_bails,
	boolean *target_bails)
{
	struct trick_attack_player *target;
	struct trick_attack_player *rammer;
	long rammer_index, target_index;
	long taken;

	if (!trick_attack_active() || !trick_attack_host() || trick_attack_slayer()->trick_no_ramming ||
		rammer_absolute < 0 || rammer_absolute >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS || target_absolute < 0 ||
		target_absolute >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS || rammer_absolute == target_absolute ||
		closing < TRICK_ATTACK_RAM_SPEED)
	{
		return;
	}
	target = &trick_attack.players[target_absolute];
	rammer = &trick_attack.players[rammer_absolute];
	trick_attack_combo_worth(target);
	if (target->combo_sequences <= 0 || target->unbanked <= 0)
		return;
	rammer_index = trick_attack_player_of(rammer_absolute);
	target_index = trick_attack_player_of(target_absolute);
	if (rammer_index == NONE || target_index == NONE)
		return;
	*target_bails = TRUE;
	*rammer_bails = FALSE;
	taken = target->unbanked * trick_attack_steal_percent() / 100;
	rammer->rams++;
	rammer->stolen_points += taken;
	platform_log("trick attack: player %ld rams player %ld mid-combo at %.1f m/s and takes %ld of its %ld at tick %ld",
		rammer_absolute, target_absolute, closing * 3.048f, taken, target->unbanked, game_time_get());
	trick_attack_lose(target_index, target, "is rammed");
	if (taken > 0)
		game_engine_slayer_port_add_score(rammer_index, taken);
}

/* ---------- the wire */

short trick_attack_reports(byte *out, short capacity)
{
	long unit_index = skate_local_skater_unit();
	long player_index = local_player_get_player_index(0);
	long absolute = trick_attack_absolute(player_index);
	struct halo_skate_tricks tricks;
	int session;
	long now = game_time_get();
	unsigned long points;
	byte *entry = out;

	if (capacity < 1 || !trick_attack_active() || trick_attack_host() || absolute == NONE || unit_index == NONE ||
		!skate_unit_tricks(unit_index, &tricks, &session))
	{
		return 0;
	}
	/* (every few ticks, and at once on a change) */
	if (trick_attack.client_sent_time != NONE && now - trick_attack.client_sent_time < TRICK_ATTACK_REPORT_TICKS &&
		now >= trick_attack.client_sent_time && tricks.sequences == trick_attack.client_sent_sequences &&
		tricks.bails == trick_attack.client_sent_bails && (long)tricks.sequence_tricks == trick_attack.client_sent_live)
	{
		return 0;
	}
	trick_attack.client_sent_time = now;
	trick_attack.client_sent_sequences = tricks.sequences;
	trick_attack.client_sent_bails = tricks.bails;
	trick_attack.client_sent_live = (long)tricks.sequence_tricks;
	points = (unsigned long)(tricks.points > 0.0f ? tricks.points + 0.5f : 0.0f);
	memset(entry, 0, SKATE_REPORT_SIZE);
	entry[0] = TRICK_ATTACK_ENTRY_FORMAT;
	entry[1] = (byte)absolute;
	entry[2] = (byte)trick_attack.client_generation;
	entry[3] = (byte)(FLAG(_trick_attack_report_valid_bit) |
		(tricks.sequence_tricks > 0 ? FLAG(_trick_attack_report_sequence_bit) : 0));
	trick_attack_put_long(entry + 4, tricks.sequences);
	trick_attack_put_long(entry + 8, points);
	trick_attack_put_long(entry + 12, tricks.bails);
	trick_attack_put_long(entry + 16, tricks.landed);
	return 1;
}

void trick_attack_handle_report(long machine_index, byte const *entry)
{
	long absolute = entry[1];
	struct trick_attack_player *state;

	if (!trick_attack_active() || !trick_attack_host() || absolute >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS ||
		!TEST_FLAG(entry[3], _trick_attack_report_valid_bit) ||
		!distributed_machine_has_player(machine_index, (short)absolute))
	{
		return;
	}
	state = &trick_attack.players[absolute];
	/* (a report from before the latest taken: a datagram late) */
	if (state->have_totals && state->generation == entry[2] && trick_attack_get_long(entry + 4) < state->sequences)
		return;
	state->have_totals = TRUE;
	state->totals_time = game_time_get();
	state->generation = entry[2];
	state->sequences = trick_attack_get_long(entry + 4);
	state->points = trick_attack_get_long(entry + 8);
	state->bails = trick_attack_get_long(entry + 12);
	state->landed = trick_attack_get_long(entry + 16);
	state->live_tricks = TEST_FLAG(entry[3], _trick_attack_report_sequence_bit) ? 1 : 0;
}

short trick_attack_entries(byte *out, short capacity)
{
	long now = game_time_get();
	short count = 0;
	short index;

	if (capacity <= 0 || !trick_attack_active() || !trick_attack_host())
		return 0;
	if (trick_attack.hot_valid && now % TRICK_ATTACK_HOT_SPOT_ENTRY_TICKS == 0)
	{
		byte *entry = out + SKATE_VERDICT_SIZE * count++;
		long left = MAX(0, trick_attack.hot_until - now) / TICKS_PER_SECOND;

		memset(entry, 0, SKATE_VERDICT_SIZE);
		entry[0] = TRICK_ATTACK_ENTRY_FORMAT;
		entry[1] = _trick_attack_entry_hot_spot;
		entry[2] = trick_attack.hot_number;
		entry[3] = (byte)PIN((long)(trick_attack.hot_multiplier * 10.0f + 0.5f), 0, 255);
		trick_attack_put_real(entry + 4, trick_attack.hot_x);
		trick_attack_put_real(entry + 8, trick_attack.hot_y);
		trick_attack_put_real(entry + 12, trick_attack.hot_z);
		trick_attack_put_long(entry + 16, (unsigned long)MIN(left, 65535));
	}
	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS && count < capacity; index++)
	{
		struct trick_attack_player *state = &trick_attack.players[index];
		byte *entry;

		if (state->unbanked != state->sent_unbanked || state->multiplier != state->sent_multiplier ||
			state->bank_number != state->sent_bank_number)
		{
			state->sent_unbanked = state->unbanked;
			state->sent_multiplier = state->multiplier;
			state->sent_bank_number = state->bank_number;
			state->send_until = now + TRICK_ATTACK_ENTRY_REPEAT_TICKS;
		}
		/* (a combo going told again now and then, against a lost word) */
		if (now > state->send_until && state->unbanked > 0 && now >= state->next_refresh)
			state->send_until = now;
		if (now > state->send_until)
			continue;
		if (now >= state->next_refresh)
			state->next_refresh = now + TRICK_ATTACK_ENTRY_REFRESH_TICKS;
		entry = out + SKATE_VERDICT_SIZE * count++;
		memset(entry, 0, SKATE_VERDICT_SIZE);
		entry[0] = TRICK_ATTACK_ENTRY_FORMAT;
		entry[1] = _trick_attack_entry_combo;
		entry[2] = (byte)index;
		entry[3] = (byte)PIN(state->multiplier, 0, 255);
		trick_attack_put_long(entry + 4, (unsigned long)MAX(state->unbanked, 0));
		trick_attack_put_long(entry + 8, (unsigned long)MAX(state->last_bank_points, 0));
		entry[12] = state->bank_number;
		trick_attack_put_long(entry + 16, (unsigned long)MAX(state->combo_tricks, 0));
	}
	return count;
}

void trick_attack_handle_entry(byte const *entry)
{
	long now = game_time_get();

	if (!trick_attack_active() || trick_attack_host())
		return;
	if (entry[1] == _trick_attack_entry_hot_spot)
	{
		boolean moved = !trick_attack.hot_valid || trick_attack.hot_number != entry[2];

		trick_attack.hot_valid = TRUE;
		trick_attack.hot_number = entry[2];
		trick_attack.hot_multiplier = (real)entry[3] / 10.0f;
		trick_attack.hot_x = trick_attack_get_real(entry + 4);
		trick_attack.hot_y = trick_attack_get_real(entry + 8);
		trick_attack.hot_z = trick_attack_get_real(entry + 12);
		trick_attack.hot_until = now + (long)trick_attack_get_long(entry + 16) * TICKS_PER_SECOND;
		trick_attack.hot_told_time = now;
		if (moved)
		{
			platform_log("trick attack: this machine hears of hot spot %d at (%.2f %.2f %.2f) at tick %ld",
				trick_attack.hot_number, trick_attack.hot_x, trick_attack.hot_y, trick_attack.hot_z, now);
		}
	}
	else if (entry[1] == _trick_attack_entry_combo && entry[2] < HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
	{
		struct trick_attack_player *state = &trick_attack.players[entry[2]];
		byte bank_number = entry[12];
		long last_bank = (long)trick_attack_get_long(entry + 8);

		state->multiplier = entry[3];
		state->unbanked = (long)trick_attack_get_long(entry + 4);
		state->combo_tricks = (long)trick_attack_get_long(entry + 16);
		/* (a bank heard of for the first time: called out if big) */
		if (state->told && bank_number != state->bank_number && last_bank >= trick_attack_mark_points())
		{
			long player_index = trick_attack_player_of(entry[2]);

			if (player_index != NONE)
				trick_attack_callout(player_get(player_index), last_bank);
		}
		if (!state->told || bank_number != state->bank_number)
		{
			platform_log("trick attack: this machine hears player %d banked %ld (bank %d) at tick %ld", entry[2],
				last_bank, bank_number, now);
		}
		state->bank_number = bank_number;
		state->last_bank_points = last_bank;
		state->told = TRUE;
		state->told_time = now;
	}
}

/* ---------- what every machine knows */

long trick_attack_unbanked(long player_index)
{
	long absolute = trick_attack_absolute(player_index);

	return absolute != NONE && trick_attack_active() ? trick_attack.players[absolute].unbanked : 0;
}

long trick_attack_multiplier(long player_index)
{
	long absolute = trick_attack_absolute(player_index);

	return absolute != NONE && trick_attack_active() ? trick_attack.players[absolute].multiplier : 0;
}

long trick_attack_combo_leader(long least_points, long except)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long best = NONE;
	long best_points = 0;

	if (!trick_attack_active())
		return NONE;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long points = trick_attack_unbanked(iterator.datum_index);

		if (iterator.datum_index == except || player->quit_out_of_game || player->unit_index == NONE ||
			points < least_points)
		{
			continue;
		}
		if (best == NONE || points > best_points)
		{
			best = iterator.datum_index;
			best_points = points;
		}
	}
	return best;
}

boolean trick_attack_hot_spot(real *x, real *y, real *z, long *seconds_left, real *multiplier)
{
	if (!trick_attack_active() || !trick_attack.hot_valid)
		return FALSE;
	if (x)
		*x = trick_attack.hot_x;
	if (y)
		*y = trick_attack.hot_y;
	if (z)
		*z = trick_attack.hot_z;
	if (seconds_left)
		*seconds_left = MAX(0, trick_attack.hot_until - game_time_get()) / TICKS_PER_SECOND;
	if (multiplier)
		*multiplier = trick_attack.hot_multiplier;
	return TRUE;
}

/* ---------- the HUD */

/* what the HUD shows the local player: their combo (empty: none going) and
the sequence under way, the hot spot's line, the leaders (the five best,
then the local player's place past them), and the callout (empty: none) */
struct trick_attack_hud
{
	wchar_t combo[64];
	wchar_t live[64];
	wchar_t hot[64];
	wchar_t leaders[7][48];
	short leader_count;
	short local_leader;
	wchar_t callout[64];
};

static void trick_attack_hud_compose(long player_index, struct player_datum *player, struct trick_attack_hud *hud)
{
	long now = game_time_get();

	memset(hud, 0, sizeof(*hud));
	hud->local_leader = NONE;
	/* the leaders */
	{
		long order[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
		long scores[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
		long count = 0, index, local_place = NONE;
		struct data_iterator iterator;
		struct player_datum *other;

		data_iterator_new(&iterator, player_data);
		while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL &&
			count < HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
		{
			long score;
			long at;

			if (other->quit_out_of_game)
				continue;
			score = game_engine_port_player_score(iterator.datum_index);
			/* (insertion: highest first, then the earlier player) */
			for (at = count; at > 0 && scores[at - 1] < score; at--)
			{
				order[at] = order[at - 1];
				scores[at] = scores[at - 1];
			}
			order[at] = iterator.datum_index;
			scores[at] = score;
			count++;
		}
		for (index = 0; index < count; index++)
		{
			if (order[index] == player_index)
				local_place = index;
		}
		ustrncpy(hud->leaders[hud->leader_count++], L"TRICK ATTACK", 47);
		for (index = 0; index < count; index++)
		{
			wchar_t name[16];
			wchar_t points[24];

			if (index >= 5 && index != local_place)
				continue;
			trick_attack_name(player_get(order[index]), name);
			trick_attack_points_text(scores[index], points, NUMBEROF(points));
			if (order[index] == player_index)
				hud->local_leader = hud->leader_count;
			usnprintf(hud->leaders[hud->leader_count], 47, L"%ld. %ls  %ls", index + 1, name, points);
			hud->leaders[hud->leader_count++][47] = 0;
			if (hud->leader_count >= (short)NUMBEROF(hud->leaders))
				break;
		}
	}
	/* the hot spot */
	{
		real x, y, z, multiplier;
		long seconds_left;

		if (trick_attack_hot_spot(&x, &y, &z, &seconds_left, &multiplier))
		{
			real distance = 0.0f;

			if (player->unit_index != NONE && object_try_and_get(player->unit_index))
			{
				real_point3d origin;

				object_get_origin(player->unit_index, &origin);
				distance = (real)sqrt((origin.x - x) * (origin.x - x) + (origin.y - y) * (origin.y - y) +
					(origin.z - z) * (origin.z - z));
			}
			usnprintf(hud->hot, NUMBEROF(hud->hot) - 1, L"HOT SPOT x%ld.%ld  %ld:%02ld  %ld M",
				(long)(multiplier + 0.05f), (long)(multiplier * 10.0f + 0.5f) % 10, seconds_left / 60, seconds_left % 60,
				(long)(distance * 3.048f + 0.5f));
		}
	}
	/* the combo, and the sequence under way (this machine's own engine) */
	{
		long unbanked = trick_attack_unbanked(player_index);
		long multiplier = trick_attack_multiplier(player_index);
		long live_points = 0, live_tricks = 0;
		char last_trick[32] = "";

		if (player->unit_index != NONE && player->local_player_index != NONE)
		{
			struct halo_skate_tricks tricks;
			int session;

			if (skate_unit_tricks(player->unit_index, &tricks, &session))
			{
				live_points = (long)(tricks.sequence_points * (tricks.multiplier > 0.0f ? tricks.multiplier : 1.0f) + 0.5f);
				live_tricks = (long)tricks.sequence_tricks;
				memcpy(last_trick, tricks.last_trick, sizeof(last_trick));
				last_trick[sizeof(last_trick) - 1] = 0;
			}
		}
		if (unbanked > 0 || multiplier > 0)
		{
			wchar_t value[24];

			trick_attack_points_text(unbanked, value, NUMBEROF(value));
			usnprintf(hud->combo, NUMBEROF(hud->combo) - 1, L"COMBO %ls  x%ld", value, multiplier);
		}
		if (live_tricks > 0)
		{
			wchar_t name[32];
			wchar_t live[24];
			short index;

			char const *shown = last_trick;
			short length = 0;

			/* (Skate 3's string's name, "ID_TRICK_FLIP_OLLIE": the trick's
			own, "OLLIE") */
			if (!strncmp(shown, "ID_TRICK_", 9))
				shown += 9;
			if (strchr(shown, '_') && (!strncmp(shown, "FLIP_", 5) || !strncmp(shown, "GRAB_", 5) ||
				!strncmp(shown, "GRIND_", 6) || !strncmp(shown, "MANUAL_", 7) || !strncmp(shown, "SLIDE_", 6)))
			{
				shown = strchr(shown, '_') + 1;
			}
			for (index = 0; length < 31 && shown[index]; index++)
				name[length++] = shown[index] == '_' ? L' ' : (wchar_t)(unsigned char)shown[index];
			name[length] = 0;
			trick_attack_points_text(live_points, live, NUMBEROF(live));
			usnprintf(hud->live, NUMBEROF(hud->live) - 1, L"+%ls  %ls", live, name);
			if (!hud->combo[0])
				ustrncpy(hud->combo, L"COMBO", NUMBEROF(hud->combo) - 1);
		}
	}
	/* a callout: a big combo banked */
	if (trick_attack.callout[0] && now < trick_attack.callout_until &&
		trick_attack.callout_until - now <= TRICK_ATTACK_CALLOUT_TICKS)
	{
		ustrncpy(hud->callout, trick_attack.callout, NUMBEROF(hud->callout) - 1);
	}
}

/* a wide string as ASCII, for the log */
static char const *trick_attack_ascii(wchar_t const *text, char *out, long size)
{
	long index;

	for (index = 0; index < size - 1 && text[index]; index++)
		out[index] = text[index] < 128 ? (char)text[index] : '?';
	out[index] = 0;
	return out;
}

/* (debug.trick_hud_log, every machine) the local player's HUD in the log
every five seconds, for the tests: what trick_attack_draw shows */
static void trick_attack_hud_log(void)
{
	long player_index = local_player_get_player_index(0);
	struct player_datum *player;
	struct trick_attack_hud hud;
	char combo[64], live[64], hot[64], callout[64], leaders[400];
	long now = game_time_get();
	short index;
	struct data_iterator iterator;
	struct player_datum *other;
	char marks[200];

	if (!config_boolean("debug.trick_hud_log") || now % (5 * TICKS_PER_SECOND) != 0 || player_index == NONE ||
		!(player = player_try_and_get(player_index)))
	{
		return;
	}
	trick_attack_hud_compose(player_index, player, &hud);
	leaders[0] = 0;
	for (index = 0; index < hud.leader_count; index++)
	{
		char line[48];

		if (index > 0)
			strncat(leaders, " / ", sizeof(leaders) - strlen(leaders) - 1);
		strncat(leaders, trick_attack_ascii(hud.leaders[index], line, sizeof(line)), sizeof(leaders) - strlen(leaders) - 1);
	}
	marks[0] = 0;
	data_iterator_new(&iterator, player_data);
	while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		char mark[32];

		if (iterator.datum_index == player_index || trick_attack_unbanked(iterator.datum_index) < trick_attack_mark_points())
			continue;
		snprintf(mark, sizeof(mark), "%s%ld:%ld", marks[0] ? "," : "", trick_attack_absolute(iterator.datum_index),
			trick_attack_unbanked(iterator.datum_index));
		strncat(marks, mark, sizeof(marks) - strlen(marks) - 1);
	}
	platform_log("trick attack hud: player %ld at tick %ld: combo '%s' live '%s' hot '%s' leaders '%s' callout '%s' marks '%s'",
		trick_attack_absolute(player_index), now, trick_attack_ascii(hud.combo, combo, sizeof(combo)),
		trick_attack_ascii(hud.live, live, sizeof(live)), trick_attack_ascii(hud.hot, hot, sizeof(hot)), leaders,
		trick_attack_ascii(hud.callout, callout, sizeof(callout)), marks);
}

/* draw_string.c's justifications (its own enum) */
#define TRICK_ATTACK_RIGHT 1
#define TRICK_ATTACK_CENTRE 2

/* text with a dark shadow under it, two pixels down and right */
static void trick_attack_text(long font_index, short justification, rectangle2d const *bounds,
	real_argb_color const *color, wchar_t const *text)
{
	real_argb_color shadow = { 0.8f, 0.0f, 0.0f, 0.0f };
	rectangle2d below = *bounds;

	shadow.alpha = color->alpha * 0.8f;
	below.x0 += 2;
	below.x1 += 2;
	below.y0 += 2;
	below.y1 += 2;
	draw_string_set_draw_mode(font_index, NONE, justification, 0, &shadow);
	rasterizer_draw_unicode_string(&below, NULL, NULL, 0, text);
	draw_string_set_draw_mode(font_index, NONE, justification, 0, color);
	rasterizer_draw_unicode_string(bounds, NULL, NULL, 0, text);
}

/* where a point of the world is on the screen (FALSE: behind the camera) */
static boolean trick_attack_screen_point(real_point3d const *point, short *x, short *y)
{
	real_point3d view_position;
	real_point2d screen_position;

	matrix4x3_transform_point(&render.frustum.world_to_view, point, &view_position);
	if (!render_camera_view_to_screen(&render.camera, &render.frustum, &view_position, &screen_position))
		return FALSE;
	*x = (short)fast_ftol(screen_position.x) - render.camera.viewport_bounds.x0;
	*y = (short)fast_ftol(screen_position.y) - render.camera.viewport_bounds.y0;
	return TRUE;
}

void trick_attack_draw(short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);
	long font_index = hud_get_font_index();
	struct player_datum *player;
	struct font_header *font;
	struct trick_attack_hud hud;
	real_argb_color white = { 1.0f, 1.0f, 1.0f, 1.0f };
	real_argb_color gold = { 1.0f, 1.0f, 0.82f, 0.25f };
	real_argb_color orange = { 1.0f, 1.0f, 0.55f, 0.1f };
	rectangle2d window;
	rectangle2d bounds;
	wchar_t text[96];
	short height;
	short index;
	long now = game_time_get();

	if (!trick_attack_active() || player_index == NONE || font_index == NONE || !(player = player_try_and_get(player_index)))
		return;
	trick_attack_hud_compose(player_index, player, &hud);
	font = font_definition_get(font_index);
	height = (short)(font->ascending_height + font->descending_height);
	window = render.camera.window_bounds;

	/* the leaders, top right, under the game's own score */
	bounds = window;
	bounds.x1 = (short)(bounds.x1 - 12);
	bounds.x0 = (short)(bounds.x1 - 400);
	bounds.y0 = (short)(bounds.y0 + 3 * height + 8);
	bounds.y1 = (short)(bounds.y0 + height + 2);
	for (index = 0; index < hud.leader_count; index++)
	{
		trick_attack_text(font_index, TRICK_ATTACK_RIGHT, &bounds, index == 0 || index == hud.local_leader ? &gold : &white,
			hud.leaders[index]);
		bounds.y0 = (short)(bounds.y0 + height);
		bounds.y1 = (short)(bounds.y1 + height);
	}

	/* the hot spot: its line at the top, and a marker where it is */
	if (hud.hot[0])
	{
		real x, y, z;
		real_point3d spot;
		short sx, sy;

		bounds = window;
		bounds.y0 = (short)(bounds.y0 + 8);
		bounds.y1 = (short)(bounds.y0 + height + 2);
		trick_attack_text(font_index, TRICK_ATTACK_CENTRE, &bounds, &orange, hud.hot);
		if (trick_attack_hot_spot(&x, &y, &z, NULL, NULL))
		{
			spot.x = x;
			spot.y = y;
			spot.z = z + 1.0f;
			if (trick_attack_screen_point(&spot, &sx, &sy))
			{
				bounds.x0 = (short)(sx - 100);
				bounds.x1 = (short)(sx + 100);
				bounds.y1 = sy;
				bounds.y0 = (short)(sy - height);
				trick_attack_text(font_index, TRICK_ATTACK_CENTRE, &bounds, &orange, L"[ HOT SPOT ]");
			}
		}
	}

	/* big combos marked over their skaters' heads */
	{
		struct data_iterator iterator;
		struct player_datum *other;

		data_iterator_new(&iterator, player_data);
		while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			long points = trick_attack_unbanked(iterator.datum_index);
			real_point3d head;
			short sx, sy;
			wchar_t value[24];

			if (iterator.datum_index == player_index || other->unit_index == NONE ||
				!object_try_and_get(other->unit_index) || points < trick_attack_mark_points())
			{
				continue;
			}
			unit_get_head_position(other->unit_index, &head);
			head.z += 0.6f;
			if (!trick_attack_screen_point(&head, &sx, &sy))
				continue;
			trick_attack_points_text(points, value, NUMBEROF(value));
			usnprintf(text, NUMBEROF(text) - 1, L"%ls x%ld", value, trick_attack_multiplier(iterator.datum_index));
			text[NUMBEROF(text) - 1] = 0;
			bounds.x0 = (short)(sx - 120);
			bounds.x1 = (short)(sx + 120);
			bounds.y1 = sy;
			bounds.y0 = (short)(sy - height);
			trick_attack_text(font_index, TRICK_ATTACK_CENTRE, &bounds, &gold, text);
		}
	}

	/* the local player's combo, bottom centre */
	if (hud.combo[0])
	{
		short bottom = (short)(window.y1 - 4 * height - 24);

		bounds = window;
		bounds.y0 = bottom;
		bounds.y1 = (short)(bottom + height + 2);
		trick_attack_text(font_index, TRICK_ATTACK_CENTRE, &bounds, &gold, hud.combo);
		if (hud.live[0])
		{
			bounds.y0 = (short)(bounds.y0 + height);
			bounds.y1 = (short)(bounds.y1 + height);
			trick_attack_text(font_index, TRICK_ATTACK_CENTRE, &bounds, &white, hud.live);
		}
	}

	/* a callout: a big combo banked, fading in its last second */
	if (hud.callout[0])
	{
		short middle = (short)((window.y0 + window.y1) / 2);
		real_argb_color fading = gold;

		fading.alpha = PIN((real)(trick_attack.callout_until - now) / (real)TICKS_PER_SECOND, 0.0f, 1.0f);
		bounds = window;
		bounds.y0 = (short)(middle - 4 * height);
		bounds.y1 = (short)(bounds.y0 + height + 2);
		trick_attack_text(font_index, TRICK_ATTACK_CENTRE, &bounds, &fading, hud.callout);
	}
}
