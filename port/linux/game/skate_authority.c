/* skate_authority.c: who decides a skater's hit in a network game (skate.h,
port/skate/README.md "Multiplayer", port/linux/NETCODE.md)

Each machine runs the skate engine for its own skater alone (skate.c), so
two machines see a hit between players differently: each a one-way or a
round trip late. Whether a hit knocks a skater over, and whether a player on
foot is pushed by it, is decided once, by the host, from its own view, and
every machine applies that verdict:

- In a network game the other players, their vehicles and other skaters go
  to the skate engine as held obstacles (skate.c): the board still stops at
  them, but the engine bails by them on its own neither past the bail speed
  nor by Skate 3's own thresholds; it holds the bail it would have had and
  tells of the hit (halo_skate_take_held_hit).
- A client reports its skater's hit to the host (_distributed_message_skate_report,
  92: what was hit, how fast, the contact's normal, whether its engine held
  a bail), a few ticks over against a lost datagram. The host decides at
  once from the report and its own copies (skate_verdict_decide): believed
  only if it has the two that near, the speed taken no faster than its
  copies closed on each other lately, a skater who rides bails past the
  obstacle bail speed or, at a fair speed, when their own engine wanted to.
  The host's own skater's hits are decided the same way, at once.
- The host sends every client the verdict (_distributed_message_skate_verdict,
  93), a few ticks over: who bails, and the push it gives a player on foot.
  The skater's own machine knocks them over through the engine
  (halo_skate_force_bail) or lets the held bail go; a person on foot is
  pushed by the host on its copy, and by their own machine on theirs; every
  machine logs it alike.
- A skater riding into a person on foot at SKATE_RUN_OVER_SPEED (skate.h)
  or faster runs them over, as a vehicle does in Halo: their engine goes on
  through them, slowed (skate.c marks such a person, the engine drops the
  contacts), and tells so in the report. The host's verdict says so too: the
  skater rides on, the person is pushed and the host deals them Halo's
  vehicle collision damage by the speed (skate_run_over_share), the
  skater's own, as a vehicle's driver's: the kill and its credit, friendly
  fire as the game type has it, reach the clients as any damage does
  (network_damage.c). Two skaters still bump: neither is on foot.

A build without the mode decides and applies verdicts too (it skates not,
but hosts skaters and is pushed). A build older than this drops both
messages as kinds it does not know: its host sends no verdicts, so a client
holds nothing (it holds only while the host says every half second that it
decides), and its client's skater decides its own hits as before. */

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "units/bipeds.h"
#include "units/units.h"
#include "network_distributed.h"

#include "bots.h"
#include "skate.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* the platform layer's (the port's log) */
void platform_log(const char *format, ...);
/* network_distributed.c's: how a player died, as the host had it (its copy
of the death on a client) */
boolean distributed_get_death(short dead_player_index, byte *killing_player_index, boolean *friendly_fire,
	boolean *killed_by_vehicle);

/* the flags of a report */
enum
{
	/* the skater's engine held a bail by the hit */
	_skate_report_bail_held_bit,
	/* the skater went through a person on foot, running them over
	(SKATE_RUN_OVER_SPEED: the engine) */
	_skate_report_run_over_bit,
};

/* the flags of a verdict */
enum
{
	/* the skater who told of the hit bails; what they hit, a skater, bails */
	_skate_verdict_first_bails_bit,
	_skate_verdict_second_bails_bit,
	/* what they hit is a person on foot, pushed by the push given */
	_skate_verdict_second_pushed_bit,
	/* what they hit is a skater; a person on foot; a vehicle */
	_skate_verdict_second_skater_bit,
	_skate_verdict_second_on_foot_bit,
	_skate_verdict_second_vehicle_bit,
	/* no verdict: the host says it decides skaters' hits */
	_skate_verdict_announce_bit,
	/* the person on foot is run over by the skater (dealt Halo's vehicle
	collision damage by the speed, by the host: skate_run_over_share) */
	_skate_verdict_second_run_over_bit,
};

/* a closing speed's scale on the wire (1/1024 world units a second, to 32
either way), a unit vector's (1/32767) and a push's (1/1024 world units a
tick, as distributed_vector's velocities) */
#define SKATE_AUTHORITY_SPEED_SCALE 1024.0f
#define SKATE_AUTHORITY_UNIT_SCALE 32767.0f
#define SKATE_AUTHORITY_PUSH_SCALE 1024.0f

/* how a verdict is decided (skate_verdict_decide), world units a second:
over the fastest the host's copies of the two closed on each other in the
last SKATE_AUTHORITY_HISTORY ticks, a told speed is taken up to this more
(the engine's parts, a leg or the board, close faster than the unit); a
skater's own engine's wish to bail is granted at this speed or more; and a
person on foot is pushed by a hit this fast or more, by Halo's acceleration
of the closing speed (half of it, as biped_accelerate halves a living
biped's), at most SKATE_PUSH_MOST, lifted by a quarter of it */
#define SKATE_VERDICT_SLACK (1.5f / 3.048f)
#define SKATE_VERDICT_LEAST_BAIL (2.f / 3.048f)
#define SKATE_PUSH_LEAST (1.f / 3.048f)
#define SKATE_PUSH_SHARE 1.f
#define SKATE_PUSH_MOST (8.f / 3.048f)
#define SKATE_PUSH_LIFT 0.25f
/* the host believes a hit of two it has this far apart (world units, less
a vehicle's bounding radius: 4.6 m), and as far more as the two go in this
many ticks (half a second: a client's skater a one-way trip late, a hit
told a little after) */
#define SKATE_VERDICT_REACH 1.5f
#define SKATE_VERDICT_LAG_TICKS 15
/* a person on foot run over (SKATE_RUN_OVER_SPEED, skate.h) is dealt Halo's
vehicle collision damage (physics.c: the game globals' vehicle_collision,
1000 on every stock map, past any unit's shield and body), as a vehicle
deals it once the two close at 1 world unit a second (3 m/s) or the vehicle
goes at 2 (6.1 m/s): from this speed (world units a second, Halo's 2) the
whole of it, as a Warthog's; slower, a share rising from none at
SKATE_RUN_OVER_SPEED to SKATE_RUN_OVER_HURT_MOST (140 of it: a Spartan
whole, 75 shield and 75 body, lives through it, one hurt does not) */
#define SKATE_RUN_OVER_LETHAL 2.f
#define SKATE_RUN_OVER_HURT_MOST 0.14f

/* ticks a report or a verdict goes in each message for (against a lost
datagram: five lost in a row at a tenth lost is one in 100,000) */
#define SKATE_AUTHORITY_REPEAT_TICKS 5
/* how often the host says it decides (ticks), and how lately a client must
have heard it to hold its skater's bails */
#define SKATE_AUTHORITY_ANNOUNCE_TICKS 15
#define SKATE_AUTHORITY_ANNOUNCED_TICKS 60
/* a hit on the same thing within this many ticks of the last contact with
it is the same collision, decided once */
#define SKATE_AUTHORITY_APART_TICKS 30
/* a bail held this many ticks with no verdict is the engine's own after all
(the host lost or gone) */
#define SKATE_AUTHORITY_WAIT_TICKS 30
/* ticks of the players' velocities the host keeps for its bound */
#define SKATE_AUTHORITY_HISTORY 16
/* a push measured this many ticks on (half a second) */
#define SKATE_AUTHORITY_MEASURE_TICKS 15
/* a person run over watched this many ticks for their death (two seconds) */
#define SKATE_RUN_OVER_WATCH_TICKS (2 * TICKS_PER_SECOND)
/* the most verdicts a skater's hits get from the host in a second; and the
hits, verdicts, verdicts heard and pushes or run-overs watched kept at once:
room for a whole lobby skating (every player, the host's bots too) */
#define SKATE_AUTHORITY_DECISIONS_PER_SECOND 4
#define SKATE_AUTHORITY_HITS 512
#define SKATE_AUTHORITY_COLLISIONS 512
#define SKATE_AUTHORITY_HEARD 256
#define SKATE_AUTHORITY_MEASURES 64
#define SKATE_AUTHORITY_NO_PLAYER 0xFF

/* ---------- the verdict
(tools/harness/tests/skate_verdict.c takes these as they are) */

/* one side of a hit, as the host has it */
struct skate_verdict_party
{
	/* a skater (else a person on foot or a vehicle) */
	boolean skater;
	/* ... on the board and not bailing */
	boolean riding;
	/* ... whose own engine held a bail by the hit (it told so) */
	boolean wants_bail;
	/* a living person on foot, on their own: pushed by a hit */
	boolean pushable;
};

/* a hit to decide: the skater whose engine told of it, and what they hit */
struct skate_verdict_input
{
	struct skate_verdict_party parties[2];
	/* the closing speed told (world units a second) and the fastest the
	host's copies of the two closed lately, an upper bound on it */
	real claimed;
	real bound;
	/* how far apart the host has them, and how far apart they may be */
	real distance;
	real allowed;
	real bail_speed;
	/* the contact's normal, from what was hit toward the skater */
	real_vector3d normal;
	/* the skater's engine went through the second (they told so) */
	boolean run_over_told;
};

struct skate_verdict_outcome
{
	boolean believed;
	boolean bails[2];
	/* the closing speed decided by */
	real closing;
	/* the push on the second, Halo's acceleration (world units a second; 0
	for none) */
	real_vector3d push;
	/* the second, a person on foot, run over, and the share of Halo's vehicle
	collision damage they are dealt (skate_run_over_share) */
	boolean run_over;
	real share;
};

static boolean skate_verdict_real_valid(real value)
{
	return value == value && value > -1e30f && value < 1e30f;
}

/* the share of Halo's vehicle collision damage a person on foot run over at
closing (world units a second) is dealt: none below SKATE_RUN_OVER_SPEED,
the whole from SKATE_RUN_OVER_LETHAL, between a share rising to
SKATE_RUN_OVER_HURT_MOST */
static real skate_run_over_share(real closing)
{
	if (!skate_verdict_real_valid(closing) || !(closing >= SKATE_RUN_OVER_SPEED))
		return 0.f;
	if (closing >= SKATE_RUN_OVER_LETHAL)
		return 1.f;
	return SKATE_RUN_OVER_HURT_MOST * (closing - SKATE_RUN_OVER_SPEED) / (SKATE_RUN_OVER_LETHAL - SKATE_RUN_OVER_SPEED);
}

/* the verdict on a hit: believed if the host had the two that near; the
speed the told one, no faster than the host's bound and its slack; each
skater who rides bails past the bail speed, or at SKATE_VERDICT_LEAST_BAIL
or more when their own engine wanted to; a person on foot is pushed away
from the skater along the ground */
static void skate_verdict_decide(struct skate_verdict_input const *input, struct skate_verdict_outcome *out)
{
	short index;

	memset(out, 0, sizeof(*out));
	out->believed = skate_verdict_real_valid(input->claimed) && input->claimed >= 0.f &&
		skate_verdict_real_valid(input->bound) && skate_verdict_real_valid(input->distance) &&
		input->distance <= input->allowed;
	if (!out->believed)
		return;
	out->closing = input->claimed;
	if (out->closing > input->bound + SKATE_VERDICT_SLACK)
		out->closing = input->bound + SKATE_VERDICT_SLACK;
	for (index = 0; index < 2; index++)
	{
		struct skate_verdict_party const *party = &input->parties[index];

		out->bails[index] = party->skater && party->riding && (out->closing > input->bail_speed ||
			(party->wants_bail && out->closing >= SKATE_VERDICT_LEAST_BAIL));
	}
	/* a person on foot hit at the run-over speed or faster (or gone through
	on the skater's machine) is run over, as by a vehicle, which goes on:
	the skater rides on */
	if (input->parties[1].pushable && (input->run_over_told || out->closing >= SKATE_RUN_OVER_SPEED))
	{
		out->run_over = TRUE;
		out->share = skate_run_over_share(out->closing);
		out->bails[0] = FALSE;
	}
	if (input->parties[1].pushable && out->closing >= SKATE_PUSH_LEAST && skate_verdict_real_valid(input->normal.i) &&
		skate_verdict_real_valid(input->normal.j))
	{
		/* (away from the skater: against the normal, along the ground) */
		real across = (real)sqrt(input->normal.i * input->normal.i + input->normal.j * input->normal.j);
		real speed = out->closing * SKATE_PUSH_SHARE;

		if (speed > SKATE_PUSH_MOST)
			speed = SKATE_PUSH_MOST;
		if (across > 0.1f)
		{
			out->push.i = -input->normal.i / across * speed;
			out->push.j = -input->normal.j / across * speed;
			out->push.k = speed * SKATE_PUSH_LIFT;
		}
	}
}

/* ---------- the entries on the wire */

/* a report, read (skate_report_read) */
struct skate_report
{
	byte player_index;
	byte flags;
	/* the reporting machine's count of its skater's collisions */
	byte number;
	long target_index;
	real closing;
	real_vector3d normal;
	/* the reporter's tick of the first contact */
	long time;
};

/* a verdict, read (skate_verdict_read) */
struct skate_verdict
{
	byte flags;
	byte player_indices[2];
	word number;
	real closing;
	long object_indices[2];
	/* the push on the second (world units a tick, as biped_accelerate takes
	it) */
	real_vector3d push;
	/* the report it answers (its number) */
	byte report_number;
};

static long skate_authority_round(real value)
{
	return (long)(value < 0.f ? value - 0.5f : value + 0.5f);
}

static void skate_authority_put_short(byte *out, real value, real scale)
{
	long step = skate_verdict_real_valid(value) ? skate_authority_round(value * scale) : 0;
	short part = (short)(step < -32767 ? -32767 : step > 32767 ? 32767 : step);

	memcpy(out, &part, sizeof(part));
}

static real skate_authority_get_short(byte const *data, real scale)
{
	short part;

	memcpy(&part, data, sizeof(part));
	return (real)part / scale;
}

static void skate_authority_put_long(byte *out, long value)
{
	int part = (int)value;

	memcpy(out, &part, sizeof(part));
}

static long skate_authority_get_long(byte const *data)
{
	int part;

	memcpy(&part, data, sizeof(part));
	return part;
}

/* a report into out (SKATE_REPORT_SIZE bytes) */
static void skate_report_write(struct skate_report const *report, byte *out)
{
	memset(out, 0, SKATE_REPORT_SIZE);
	out[0] = SKATE_REPORT_FORMAT;
	out[1] = report->player_index;
	out[2] = report->flags;
	out[3] = report->number;
	skate_authority_put_long(out + 4, report->target_index);
	skate_authority_put_short(out + 8, report->closing, SKATE_AUTHORITY_SPEED_SCALE);
	skate_authority_put_short(out + 10, report->normal.i, SKATE_AUTHORITY_UNIT_SCALE);
	skate_authority_put_short(out + 12, report->normal.j, SKATE_AUTHORITY_UNIT_SCALE);
	skate_authority_put_short(out + 14, report->normal.k, SKATE_AUTHORITY_UNIT_SCALE);
	skate_authority_put_long(out + 16, report->time);
}

/* ... and back: FALSE for one of another format, a speed below 0 or a
normal that is not one */
static boolean skate_report_read(byte const *data, struct skate_report *report)
{
	real length;

	if (data[0] != SKATE_REPORT_FORMAT)
		return FALSE;
	memset(report, 0, sizeof(*report));
	report->player_index = data[1];
	report->flags = data[2];
	report->number = data[3];
	report->target_index = skate_authority_get_long(data + 4);
	report->closing = skate_authority_get_short(data + 8, SKATE_AUTHORITY_SPEED_SCALE);
	report->normal.i = skate_authority_get_short(data + 10, SKATE_AUTHORITY_UNIT_SCALE);
	report->normal.j = skate_authority_get_short(data + 12, SKATE_AUTHORITY_UNIT_SCALE);
	report->normal.k = skate_authority_get_short(data + 14, SKATE_AUTHORITY_UNIT_SCALE);
	report->time = skate_authority_get_long(data + 16);
	length = (real)sqrt(report->normal.i * report->normal.i + report->normal.j * report->normal.j +
		report->normal.k * report->normal.k);
	return report->closing >= 0.f && length > 0.9f && length < 1.1f;
}

/* a verdict into out (SKATE_VERDICT_SIZE bytes) */
static void skate_verdict_write(struct skate_verdict const *verdict, byte *out)
{
	memset(out, 0, SKATE_VERDICT_SIZE);
	out[0] = SKATE_VERDICT_FORMAT;
	out[1] = verdict->flags;
	out[2] = verdict->player_indices[0];
	out[3] = verdict->player_indices[1];
	memcpy(out + 4, &verdict->number, sizeof(word));
	skate_authority_put_short(out + 6, verdict->closing, SKATE_AUTHORITY_SPEED_SCALE);
	skate_authority_put_long(out + 8, verdict->object_indices[0]);
	skate_authority_put_long(out + 12, verdict->object_indices[1]);
	skate_authority_put_short(out + 16, verdict->push.i, SKATE_AUTHORITY_PUSH_SCALE);
	skate_authority_put_short(out + 18, verdict->push.j, SKATE_AUTHORITY_PUSH_SCALE);
	skate_authority_put_short(out + 20, verdict->push.k, SKATE_AUTHORITY_PUSH_SCALE);
	out[22] = verdict->report_number;
}

/* ... and back: FALSE for one of another format, or a speed below 0 */
static boolean skate_verdict_read(byte const *data, struct skate_verdict *verdict)
{
	if (data[0] != SKATE_VERDICT_FORMAT)
		return FALSE;
	memset(verdict, 0, sizeof(*verdict));
	verdict->flags = data[1];
	verdict->player_indices[0] = data[2];
	verdict->player_indices[1] = data[3];
	memcpy(&verdict->number, data + 4, sizeof(word));
	verdict->closing = skate_authority_get_short(data + 6, SKATE_AUTHORITY_SPEED_SCALE);
	verdict->object_indices[0] = skate_authority_get_long(data + 8);
	verdict->object_indices[1] = skate_authority_get_long(data + 12);
	verdict->push.i = skate_authority_get_short(data + 16, SKATE_AUTHORITY_PUSH_SCALE);
	verdict->push.j = skate_authority_get_short(data + 18, SKATE_AUTHORITY_PUSH_SCALE);
	verdict->push.k = skate_authority_get_short(data + 20, SKATE_AUTHORITY_PUSH_SCALE);
	verdict->report_number = data[22];
	return verdict->closing >= 0.f;
}

/* what a verdict says, the same words on every machine (the network test
compares them): "player 1's skater bails; player 0 on foot pushed 2.1 m/s;
at 7.3 m/s" */
static void skate_verdict_text(struct skate_verdict const *verdict, char *text, size_t size)
{
	char second[96];
	char first_player[16];
	char second_player[16];
	/* (the speed the person gets: half of Halo's acceleration, as biped_accelerate
	gives a living biped) */
	real push = (real)sqrt(verdict->push.i * verdict->push.i + verdict->push.j * verdict->push.j +
		verdict->push.k * verdict->push.k) * TICKS_PER_SECOND * 3.048f * 0.5f;

	snprintf(first_player, sizeof(first_player), "player %d", verdict->player_indices[0]);
	if (verdict->player_indices[1] == SKATE_AUTHORITY_NO_PLAYER)
		snprintf(second_player, sizeof(second_player), "%s", "an AI's");
	else
		snprintf(second_player, sizeof(second_player), "player %d", verdict->player_indices[1]);
	if (TEST_FLAG(verdict->flags, _skate_verdict_second_skater_bit))
	{
		snprintf(second, sizeof(second), "%s's skater %s", second_player,
			TEST_FLAG(verdict->flags, _skate_verdict_second_bails_bit) ? "bails" : "rides on");
	}
	else if (TEST_FLAG(verdict->flags, _skate_verdict_second_vehicle_bit))
	{
		if (verdict->player_indices[1] == SKATE_AUTHORITY_NO_PLAYER)
			snprintf(second, sizeof(second), "%s", "a vehicle");
		else
			snprintf(second, sizeof(second), "%s's vehicle", second_player);
	}
	else if (TEST_FLAG(verdict->flags, _skate_verdict_second_run_over_bit))
	{
		snprintf(second, sizeof(second), "%s on foot run over, %.2f of a vehicle's hit, pushed %.1f m/s", second_player,
			skate_run_over_share(verdict->closing), push);
	}
	else if (TEST_FLAG(verdict->flags, _skate_verdict_second_pushed_bit))
		snprintf(second, sizeof(second), "%s on foot pushed %.1f m/s", second_player, push);
	else
		snprintf(second, sizeof(second), "%s on foot not pushed", second_player);
	snprintf(text, size, "%s's skater %s; %s; at %.1f m/s", first_player,
		TEST_FLAG(verdict->flags, _skate_verdict_first_bails_bit) ? "bails" : "rides on", second,
		verdict->closing * 3.048f);
}

/* ---------- this machine's skater's hits, and the host's collisions */

static struct skate_local_hit
{
	/* what was hit (NONE: a free slot), and by which of this machine's
	skaters (its player's, or on the host a bot's: skate.c) */
	long target_index;
	long skater_index;
	byte number;
	long first_time;
	long last_time;
	real closing;
	real_vector3d normal;
	boolean bail_held;
	/* ... and went through a person on foot, running them over */
	boolean run_over;
	/* its report goes in each message until this tick */
	long send_until;
	/* a verdict came (or none came in time), and whether it knocked this
	machine's skater over */
	boolean resolved;
	boolean bails;
	/* the verdict's number (0: none came in time) */
	word verdict;
} skate_local_hits[SKATE_AUTHORITY_HITS];

/* (the host) the hits it decided: the verdict, sent until a tick, and the
last report of it (a report within SKATE_AUTHORITY_APART_TICKS of it is the
same collision's, answered with the same verdict) */
static struct skate_collision
{
	boolean used;
	long last_time;
	long send_until;
	struct skate_verdict verdict;
} skate_collisions[SKATE_AUTHORITY_COLLISIONS];

/* (a client) the verdicts it heard: their numbers, when, and what they said */
static struct skate_heard
{
	boolean used;
	long time;
	struct skate_verdict verdict;
} skate_heard[SKATE_AUTHORITY_HEARD];

/* a push measured half a second on (both machines' logs: the network
test checks that a person pushed moved on each) */
static struct skate_push_measure
{
	long object_index;
	word number;
	long time;
	real_point3d from;
	real_vector3d along;
} skate_measures[SKATE_AUTHORITY_MEASURES];

/* a person run over watched a while (every machine's log: the network test
checks that they died, killed by the skater, as a vehicle's driver kills) */
static struct skate_run_over_watch
{
	long object_index;
	word number;
	long time;
	byte skater_player;
	byte victim_player;
} skate_run_over_watches[SKATE_AUTHORITY_MEASURES];

static struct
{
	byte next_hit_number;
	word next_verdict_number;
	short next_heard;
	short next_measure;
	short next_run_over_watch;
	/* (a client) when the host last said it decides */
	long announced_time;
	/* (the host) each player's unit's velocity, the last ticks, and the
	verdicts their hits had in the last second */
	real_vector3d velocities[SKATE_NETWORK_PLAYERS][SKATE_AUTHORITY_HISTORY];
	long decisions_time[SKATE_NETWORK_PLAYERS];
	short decisions[SKATE_NETWORK_PLAYERS];
} skate_authority_globals;

void skate_authority_new_game(void)
{
	memset(skate_local_hits, 0, sizeof(skate_local_hits));
	memset(skate_collisions, 0, sizeof(skate_collisions));
	memset(skate_heard, 0, sizeof(skate_heard));
	memset(skate_measures, 0, sizeof(skate_measures));
	memset(skate_run_over_watches, 0, sizeof(skate_run_over_watches));
	memset(&skate_authority_globals, 0, sizeof(skate_authority_globals));
	{
		short index;

		for (index = 0; index < SKATE_AUTHORITY_HITS; index++)
			skate_local_hits[index].target_index = NONE;
		for (index = 0; index < SKATE_AUTHORITY_MEASURES; index++)
			skate_measures[index].object_index = NONE;
		for (index = 0; index < SKATE_AUTHORITY_MEASURES; index++)
			skate_run_over_watches[index].object_index = NONE;
	}
	skate_authority_globals.announced_time = NONE;
}

boolean skate_authority_holds(void)
{
	switch (game_connection())
	{
	case _game_connection_network_server:
		return TRUE;
	case _game_connection_network_client:
		return skate_authority_globals.announced_time != NONE &&
			game_time_get() - skate_authority_globals.announced_time <= SKATE_AUTHORITY_ANNOUNCED_TICKS &&
			game_time_get() >= skate_authority_globals.announced_time;
	default:
		return FALSE;
	}
}

/* the player whose unit an object is, as a byte (SKATE_AUTHORITY_NO_PLAYER
for none: an AI's, a vehicle) */
static byte skate_authority_player_of(long object_index)
{
	struct object_datum *object = object_index != NONE ? object_try_and_get(object_index) : NULL;
	long player_index;

	if (!object || !(object->object.type == _object_type_biped || object->object.type == _object_type_vehicle))
		return SKATE_AUTHORITY_NO_PLAYER;
	player_index = ((struct unit_datum *)object)->unit.player_index;
	if (player_index == NONE && object->object.type == _object_type_vehicle)
	{
		/* (a vehicle: its driver's) */
		long driver_index = ((struct unit_datum *)object)->unit.driver_object_index;

		if (driver_index != NONE)
			player_index = unit_get(driver_index)->unit.player_index;
	}
	if (player_index == NONE || DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) >= SKATE_NETWORK_PLAYERS)
		return SKATE_AUTHORITY_NO_PLAYER;
	return (byte)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
}

/* whether a unit is a skater on the board and not bailing, as this machine
has them: its own skaters (its player's, the host's bots') as its engine has
them, another machine's as their state says */
static boolean skate_authority_riding(long unit_index)
{
	byte player_index;

	if (unit_index == NONE)
		return FALSE;
	if (skate_host_skater_riding(unit_index))
		return TRUE;
	player_index = skate_authority_player_of(unit_index);
	return player_index != SKATE_AUTHORITY_NO_PLAYER && skate_network_player_riding(player_index) &&
		skate_network_unit_skating(unit_index);
}

/* a push to measure a third of a second on */
static void skate_authority_measure(word number, long object_index, real_vector3d const *push)
{
	struct skate_push_measure *measure = &skate_measures[skate_authority_globals.next_measure];
	struct object_datum *object = object_try_and_get(object_index);
	real across = (real)sqrt(push->i * push->i + push->j * push->j);

	skate_authority_globals.next_measure = (short)((skate_authority_globals.next_measure + 1) %
		SKATE_AUTHORITY_MEASURES);
	if (!object || !(across > 0.f))
		return;
	measure->object_index = object_index;
	measure->number = number;
	measure->time = game_time_get();
	measure->from = object->object.position;
	measure->along.i = push->i / across;
	measure->along.j = push->j / across;
	measure->along.k = 0.f;
}

/* whether this machine runs the player's skater: its own player, or on the
host one of its bots (skate.c runs their skate engine) */
static boolean skate_authority_runs(struct player_datum const *player)
{
	return player && (player->local_player_index != NONE ||
		(game_connection() == _game_connection_network_server && bots_player_is_bot(player)));
}

/* this machine's skater's (skater_index's) hit on target_index (a slot within
SKATE_AUTHORITY_APART_TICKS of its last contact), else NULL */
static struct skate_local_hit *skate_authority_local_hit(long skater_index, long target_index)
{
	short index;

	for (index = 0; index < SKATE_AUTHORITY_HITS; index++)
	{
		struct skate_local_hit *hit = &skate_local_hits[index];

		if (hit->target_index != NONE && hit->target_index == target_index && hit->skater_index == skater_index &&
			game_time_get() - hit->last_time <= SKATE_AUTHORITY_APART_TICKS)
		{
			return hit;
		}
	}
	return NULL;
}

/* a slot for a new hit of this machine's skater on target_index: a free
one, else the oldest */
static struct skate_local_hit *skate_authority_new_local_hit(long skater_index, long target_index)
{
	struct skate_local_hit *hit = &skate_local_hits[0];
	short index;

	for (index = 0; index < SKATE_AUTHORITY_HITS; index++)
	{
		if (skate_local_hits[index].target_index == NONE)
		{
			hit = &skate_local_hits[index];
			break;
		}
		if (skate_local_hits[index].last_time < hit->last_time)
			hit = &skate_local_hits[index];
	}
	memset(hit, 0, sizeof(*hit));
	hit->target_index = target_index;
	hit->skater_index = skater_index;
	hit->number = ++skate_authority_globals.next_hit_number;
	hit->first_time = game_time_get();
	hit->last_time = game_time_get();
	return hit;
}

/* a verdict on this machine's skater: knocked over, or the bail their
engine held let go; their hit on the other is decided, and no new report
reopens it (one their engine never saw is noted as decided). Once a hit */
static void skate_authority_settle(struct skate_verdict const *verdict)
{
	short party;

	for (party = 0; party < 2; party++)
	{
		long other_index = verdict->object_indices[party ^ 1];
		boolean bails = TEST_FLAG(verdict->flags, party ? _skate_verdict_second_bails_bit :
			_skate_verdict_first_bails_bit);
		struct skate_local_hit *hit;
		long skater_index = verdict->object_indices[party];
		byte player_index = skate_authority_player_of(skater_index);

		/* (this machine's player's skater, on the board still or not) */
		if ((party && !TEST_FLAG(verdict->flags, _skate_verdict_second_skater_bit)) ||
			player_index == SKATE_AUTHORITY_NO_PLAYER || !skate_authority_runs(distributed_player(player_index)) ||
			distributed_living_unit(distributed_player(player_index)) != skater_index)
		{
			continue;
		}
		hit = skate_authority_local_hit(skater_index, other_index);
		if (!hit)
			hit = skate_authority_new_local_hit(skater_index, other_index);
		/* (each verdict once; a newer one on the same hit is the host's still) */
		if (hit->resolved && hit->verdict == verdict->number)
			continue;
		hit->resolved = TRUE;
		hit->verdict = verdict->number;
		hit->bails = bails;
		hit->send_until = NONE;
		if (bails)
		{
			platform_log("skate verdict %u: this machine's skater bails%s", verdict->number,
				skate_force_bail(skater_index, verdict->closing) ? "" : " (not on the board)");
		}
		else if (hit->bail_held)
			platform_log("skate verdict %u: this machine's skater rides on, the bail its engine held let go",
				verdict->number);
	}
}

/* a person on foot run over (the verdict's second): the host deals them
Halo's vehicle collision damage by the speed (skate_run_over_share), the
skater's as a vehicle's is its driver's (network_damage_vehicle_collision:
the kill theirs, friendly fire as the game type has it; the clients have it
from the host as any damage, a killing blow with its killer); every machine
watches them a while (skate_authority_update) */
static void skate_authority_run_over(struct skate_verdict const *verdict, boolean decided)
{
	long object_index = verdict->object_indices[1];
	long skater_index = verdict->object_indices[0];
	struct object_datum *object = object_try_and_get(object_index);
	struct object_datum *skater = object_try_and_get(skater_index);
	struct skate_run_over_watch *watch;
	real share = skate_run_over_share(verdict->closing);

	/* (one dead here already, the host's killing blow come first, is watched
	too: its death is told at once) */
	if (!object || object->object.type != _object_type_biped)
		return;
	/* (a later run-over of the same person stands for the earlier still watched:
	one death, told once, of the run-over that came last) */
	{
		short index;

		for (index = 0; index < SKATE_AUTHORITY_MEASURES; index++)
		{
			if (skate_run_over_watches[index].object_index == object_index)
				skate_run_over_watches[index].object_index = NONE;
		}
	}
	watch = &skate_run_over_watches[skate_authority_globals.next_run_over_watch];
	skate_authority_globals.next_run_over_watch = (short)((skate_authority_globals.next_run_over_watch + 1) %
		SKATE_AUTHORITY_MEASURES);
	watch->object_index = object_index;
	watch->number = verdict->number;
	watch->time = game_time_get();
	watch->skater_player = verdict->player_indices[0];
	watch->victim_player = verdict->player_indices[1];
	if (decided && skater && share > 0.f && !TEST_FLAG(object->object.damage_flags, _object_dead_bit))
	{
		real shield = object->object.shield_vitality;
		real body = object->object.body_vitality;
		real_vector3d direction;
		boolean dealt;

		/* (from the skater to them, or the push's way) */
		vector_from_points3d(&skater->object.bounding_sphere_center, &object->object.bounding_sphere_center,
			&direction);
		if (!(magnitude3d(&direction) > 0.001f))
			direction = verdict->push;
		dealt = network_damage_vehicle_collision(object_index, skater_index, share,
			&skater->object.bounding_sphere_center, &direction);
		object = object_try_and_get(object_index);
		platform_log("skate verdict %u: player %d's skater ran over %s here, %.2f of Halo's vehicle collision damage%s: "
			"shield %.2f to %.2f, body %.2f to %.2f%s", verdict->number, verdict->player_indices[0],
			verdict->player_indices[1] == SKATE_AUTHORITY_NO_PLAYER ? "an AI" : "a player", share,
			dealt ? "" : " (none in this map's globals)", shield, object ? object->object.shield_vitality : 0.f, body,
			object ? object->object.body_vitality : 0.f,
			!object || TEST_FLAG(object->object.damage_flags, _object_dead_bit) ? ", killed" : "");
	}
}

/* a verdict decided (the host) or heard (a client), applied on this
machine: its own skater settled (skate_authority_settle), a person on foot
pushed (by the host on its copy, by their own machine on theirs) and, run
over, hurt by the host, and logged alike on every machine */
static void skate_authority_apply(struct skate_verdict const *verdict, boolean decided)
{
	char text[192];

	skate_verdict_text(verdict, text, sizeof(text));
	platform_log("skate verdict %u %s: %s", verdict->number, decided ? "decided" : "heard", text);
	skate_authority_settle(verdict);
	if (TEST_FLAG(verdict->flags, _skate_verdict_second_pushed_bit))
	{
		long object_index = verdict->object_indices[1];
		struct object_datum *object = object_try_and_get(object_index);
		boolean push = FALSE;

		if (object && object->object.type == _object_type_biped &&
			!TEST_FLAG(object->object.damage_flags, _object_dead_bit) && object->object.parent_object_index == NONE &&
			!skate_unit_is_skating(object_index))
		{
			long player_index = ((struct unit_datum *)object)->unit.player_index;

			/* (the host its copy, a client its own player's unit) */
			push = decided || (player_index != NONE && distributed_player_is_local(player_index) &&
				distributed_living_unit(player_try_and_get(player_index)) == object_index);
		}
		if (push)
		{
			real_vector3d acceleration = verdict->push;

			biped_accelerate(object_index, &acceleration);
		}
		skate_authority_measure(verdict->number, object_index, &verdict->push);
		if (push)
		{
			platform_log("skate verdict %u: pushed %s's unit here, %.1f m/s", verdict->number,
				verdict->player_indices[1] == SKATE_AUTHORITY_NO_PLAYER ? "an AI" : "a player",
				magnitude3d(&verdict->push) * TICKS_PER_SECOND * 3.048f * 0.5f);
		}
	}
	/* (pushed, as a vehicle pushes them, then hurt) */
	if (TEST_FLAG(verdict->flags, _skate_verdict_second_run_over_bit))
		skate_authority_run_over(verdict, decided);
}

/* (the host) the collision between two units, if one was decided within
SKATE_AUTHORITY_APART_TICKS of its last report */
static struct skate_collision *skate_authority_collision(long first_index, long second_index)
{
	short index;

	for (index = 0; index < SKATE_AUTHORITY_COLLISIONS; index++)
	{
		struct skate_collision *collision = &skate_collisions[index];
		long const *units = collision->verdict.object_indices;

		if (collision->used && game_time_get() - collision->last_time <= SKATE_AUTHORITY_APART_TICKS &&
			((units[0] == first_index && units[1] == second_index) ||
			(units[0] == second_index && units[1] == first_index)))
		{
			return collision;
		}
	}
	return NULL;
}

/* the fastest two units closed on each other lately as the host has them
(world units a second): each player's unit by its velocities of the last
SKATE_AUTHORITY_HISTORY ticks, anything else by its velocity now */
static real skate_authority_bound(long first_index, long second_index)
{
	long const units[2] = { first_index, second_index };
	byte players[2];
	real bound = 0.f;
	short tick;
	short side;

	for (side = 0; side < 2; side++)
		players[side] = skate_authority_player_of(units[side]);
	for (tick = 0; tick < SKATE_AUTHORITY_HISTORY; tick++)
	{
		real_vector3d velocities[2];
		real_vector3d relative;
		real speed;

		for (side = 0; side < 2; side++)
		{
			struct object_datum *object = object_get(units[side]);

			if (players[side] != SKATE_AUTHORITY_NO_PLAYER && object->object.type == _object_type_biped)
				velocities[side] = skate_authority_globals.velocities[players[side]][tick];
			else
				velocities[side] = object->object.translational_velocity;
		}
		relative.i = velocities[0].i - velocities[1].i;
		relative.j = velocities[0].j - velocities[1].j;
		relative.k = velocities[0].k - velocities[1].k;
		speed = magnitude3d(&relative) * TICKS_PER_SECOND;
		if (speed > bound)
			bound = speed;
	}
	return bound;
}

/* (the host) a skater's hit, told by a client (machine_index) or by its own
engine (NONE): decided, unless the same collision was (then its verdict is
sent again) or it is not to be believed */
static void skate_authority_decide(long machine_index, struct skate_report const *report)
{
	struct player_datum *player;
	struct object_datum *skater;
	struct object_datum *target;
	struct skate_collision *collision;
	struct skate_verdict_input input;
	struct skate_verdict_outcome outcome;
	struct skate_verdict verdict;
	long skater_index;
	byte entry[SKATE_VERDICT_SIZE];
	long now = game_time_get();
	short index;

	if (report->player_index >= SKATE_NETWORK_PLAYERS || !(player = distributed_player(report->player_index)) ||
		(machine_index == NONE ? !skate_authority_runs(player) :
		!distributed_machine_has_player(machine_index, report->player_index)))
	{
		return;
	}
	skater_index = distributed_living_unit(player);
	if (skater_index == NONE || !skate_unit_is_skating(skater_index) || report->target_index == skater_index ||
		!distributed_object_index_valid(report->target_index) || !(target = object_try_and_get(report->target_index)) ||
		!(target->object.type == _object_type_biped || target->object.type == _object_type_vehicle) ||
		TEST_FLAG(target->object.damage_flags, _object_dead_bit))
	{
		return;
	}
	skater = object_get(skater_index);
	collision = skate_authority_collision(skater_index, report->target_index);
	if (collision)
	{
		collision->last_time = now;
		collision->send_until = now + SKATE_AUTHORITY_REPEAT_TICKS;
		/* (the host's own skater's engine seeing it after: as decided) */
		if (machine_index == NONE)
			skate_authority_settle(&collision->verdict);
		return;
	}
	/* (no more than a few verdicts a second for a skater's hits) */
	if (now - skate_authority_globals.decisions_time[report->player_index] >= TICKS_PER_SECOND ||
		now < skate_authority_globals.decisions_time[report->player_index])
	{
		skate_authority_globals.decisions_time[report->player_index] = now;
		skate_authority_globals.decisions[report->player_index] = 0;
	}
	if (skate_authority_globals.decisions[report->player_index] >= SKATE_AUTHORITY_DECISIONS_PER_SECOND)
		return;
	memset(&input, 0, sizeof(input));
	input.parties[0].skater = TRUE;
	input.parties[0].riding = skate_authority_riding(skater_index);
	input.parties[0].wants_bail = TEST_FLAG(report->flags, _skate_report_bail_held_bit);
	input.parties[1].skater = target->object.type == _object_type_biped && skate_unit_is_skating(report->target_index);
	input.parties[1].riding = input.parties[1].skater && skate_authority_riding(report->target_index);
	input.parties[1].pushable = target->object.type == _object_type_biped && !input.parties[1].skater &&
		target->object.parent_object_index == NONE;
	input.claimed = report->closing;
	input.bound = skate_authority_bound(skater_index, report->target_index);
	input.distance = (real)sqrt(distance_squared3d(&skater->object.position, &target->object.position));
	if (target->object.type == _object_type_vehicle)
		input.distance -= target->object.bounding_sphere_radius;
	input.allowed = SKATE_VERDICT_REACH + (magnitude3d(&skater->object.translational_velocity) +
		magnitude3d(&target->object.translational_velocity)) * SKATE_VERDICT_LAG_TICKS;
	input.bail_speed = skate_obstacle_bail_speed();
	input.normal = report->normal;
	input.run_over_told = TEST_FLAG(report->flags, _skate_report_run_over_bit);
	skate_verdict_decide(&input, &outcome);
	if (!outcome.believed)
	{
		platform_log("skate verdict refused: player %d's hit at %.1f m/s, %.2f world units apart (%.2f believed)",
			report->player_index, report->closing * 3.048f, input.distance, input.allowed);
		return;
	}
	skate_authority_globals.decisions[report->player_index]++;
	memset(&verdict, 0, sizeof(verdict));
	verdict.number = ++skate_authority_globals.next_verdict_number;
	if (!verdict.number)
		verdict.number = ++skate_authority_globals.next_verdict_number;
	verdict.flags = (byte)((outcome.bails[0] ? FLAG(_skate_verdict_first_bails_bit) : 0) |
		(outcome.bails[1] ? FLAG(_skate_verdict_second_bails_bit) : 0) |
		(input.parties[1].skater ? FLAG(_skate_verdict_second_skater_bit) : 0) |
		(target->object.type == _object_type_vehicle ? FLAG(_skate_verdict_second_vehicle_bit) : 0) |
		(input.parties[1].pushable ? FLAG(_skate_verdict_second_on_foot_bit) : 0) |
		(input.parties[1].pushable && magnitude3d(&outcome.push) > 0.f ? FLAG(_skate_verdict_second_pushed_bit) : 0) |
		(outcome.run_over ? FLAG(_skate_verdict_second_run_over_bit) : 0));
	verdict.player_indices[0] = report->player_index;
	verdict.player_indices[1] = skate_authority_player_of(report->target_index);
	verdict.object_indices[0] = skater_index;
	verdict.object_indices[1] = report->target_index;
	verdict.closing = outcome.closing;
	verdict.push.i = outcome.push.i / TICKS_PER_SECOND;
	verdict.push.j = outcome.push.j / TICKS_PER_SECOND;
	verdict.push.k = outcome.push.k / TICKS_PER_SECOND;
	verdict.report_number = report->number;
	/* (as every machine has it: through the wire's own rounding) */
	skate_verdict_write(&verdict, entry);
	skate_verdict_read(entry, &verdict);
	collision = &skate_collisions[0];
	for (index = 0; index < SKATE_AUTHORITY_COLLISIONS; index++)
	{
		if (!skate_collisions[index].used)
		{
			collision = &skate_collisions[index];
			break;
		}
		if (skate_collisions[index].last_time < collision->last_time)
			collision = &skate_collisions[index];
	}
	collision->used = TRUE;
	collision->last_time = now;
	collision->send_until = now + SKATE_AUTHORITY_REPEAT_TICKS;
	collision->verdict = verdict;
	platform_log("skate verdict %u: player %d's hit told at %.1f m/s (%s, its engine %s%s), the host's bound %.1f m/s, "
		"%.2f world units apart", verdict.number, report->player_index, report->closing * 3.048f,
		machine_index == NONE ? "the host's own skater" : "another machine's skater",
		input.parties[0].wants_bail ? "held a bail" : "held no bail",
		input.run_over_told ? ", went through them" : "", input.bound * 3.048f, input.distance);
	skate_authority_apply(&verdict, TRUE);
}

void skate_authority_engine_hit(long skater_index, long target_index, real closing, real_vector3d const *normal,
	boolean touched, boolean bail_held, boolean run_over)
{
	struct skate_local_hit *hit = NULL;
	long now = game_time_get();
	short index;

	/* (a contact closing at nothing, or parting) */
	if (!(closing > 0.f))
		closing = 0.f;
	/* (nobody decides now: a bail held is the engine's own after all) */
	if (!skate_authority_holds())
	{
		if (bail_held)
			skate_force_bail(skater_index, closing);
		return;
	}
	if (touched)
		hit = skate_authority_local_hit(skater_index, target_index);
	else
	{
		/* (a bail held without a contact this tick: the latest hit's) */
		for (index = 0; index < SKATE_AUTHORITY_HITS; index++)
		{
			struct skate_local_hit *candidate = &skate_local_hits[index];

			if (candidate->target_index != NONE && candidate->skater_index == skater_index &&
				now - candidate->last_time <= SKATE_AUTHORITY_APART_TICKS &&
				(!hit || candidate->last_time > hit->last_time))
			{
				hit = candidate;
			}
		}
		if (!hit)
			return;
	}
	if (!hit)
	{
		hit = skate_authority_new_local_hit(skater_index, target_index);
		hit->send_until = now + SKATE_AUTHORITY_REPEAT_TICKS;
		platform_log("skate hit %d: this machine's skater (player %d) hit %s at %.1f m/s%s, for the host to decide",
			hit->number, skate_authority_player_of(skater_index) == SKATE_AUTHORITY_NO_PLAYER ? -1 :
			skate_authority_player_of(skater_index),
			skate_unit_is_skating(target_index) ? "a skater" : object_try_and_get(target_index) &&
			object_get(target_index)->object.type == _object_type_vehicle ? "a vehicle" : "a person on foot",
			closing * 3.048f, run_over ? " and went through them, running them over" : "");
	}
	if (touched)
	{
		hit->last_time = now;
		if (closing > hit->closing)
		{
			hit->closing = closing;
			hit->normal = *normal;
		}
	}
	if (run_over && !hit->run_over)
	{
		hit->run_over = TRUE;
		if (!hit->resolved)
			hit->send_until = now + SKATE_AUTHORITY_REPEAT_TICKS;
	}
	if (bail_held && !hit->bail_held)
	{
		hit->bail_held = TRUE;
		/* (the host told of the wish while it has not decided) */
		if (!hit->resolved)
			hit->send_until = now + SKATE_AUTHORITY_REPEAT_TICKS;
	}
	/* the host's own skater's: decided at once */
	if (!hit->resolved && game_connection() == _game_connection_network_server)
	{
		struct skate_report report;
		byte player_index = skate_authority_player_of(skater_index);

		if (player_index == SKATE_AUTHORITY_NO_PLAYER)
			return;
		memset(&report, 0, sizeof(report));
		report.player_index = player_index;
		report.flags = (byte)((hit->bail_held ? FLAG(_skate_report_bail_held_bit) : 0) |
			(hit->run_over ? FLAG(_skate_report_run_over_bit) : 0));
		report.number = hit->number;
		report.target_index = hit->target_index;
		report.closing = hit->closing;
		report.normal = hit->normal;
		report.time = hit->first_time;
		skate_authority_decide(NONE, &report);
	}
}

short skate_authority_reports(byte *out, short capacity)
{
	long skater_index = skate_local_skater_unit();
	byte player_index = skate_authority_player_of(skater_index);
	long now = game_time_get();
	short count = 0;
	short index;

	if (game_connection() != _game_connection_network_client || player_index == SKATE_AUTHORITY_NO_PLAYER)
		return 0;
	for (index = 0; index < SKATE_AUTHORITY_HITS && count < capacity; index++)
	{
		struct skate_local_hit const *hit = &skate_local_hits[index];
		struct skate_report report;

		if (hit->target_index == NONE || hit->skater_index != skater_index || hit->resolved ||
			hit->send_until == NONE || now > hit->send_until)
		{
			continue;
		}
		memset(&report, 0, sizeof(report));
		report.player_index = player_index;
		report.flags = (byte)((hit->bail_held ? FLAG(_skate_report_bail_held_bit) : 0) |
			(hit->run_over ? FLAG(_skate_report_run_over_bit) : 0));
		report.number = hit->number;
		report.target_index = hit->target_index;
		report.closing = hit->closing;
		report.normal = hit->normal;
		report.time = hit->first_time;
		skate_report_write(&report, out + count * SKATE_REPORT_SIZE);
		count++;
	}
	return count;
}

short skate_authority_verdicts(byte *out, short capacity)
{
	long now = game_time_get();
	short count = 0;
	short index;

	if (game_connection() != _game_connection_network_server)
		return 0;
	for (index = 0; index < SKATE_AUTHORITY_COLLISIONS && count < capacity; index++)
	{
		struct skate_collision const *collision = &skate_collisions[index];

		if (collision->used && now <= collision->send_until)
			skate_verdict_write(&collision->verdict, out + SKATE_VERDICT_SIZE * count++);
	}
	/* (and every half second with none: the host decides) */
	if (!count && capacity > 0 && now % SKATE_AUTHORITY_ANNOUNCE_TICKS == 0)
	{
		struct skate_verdict announce;

		memset(&announce, 0, sizeof(announce));
		announce.flags = FLAG(_skate_verdict_announce_bit);
		announce.player_indices[0] = SKATE_AUTHORITY_NO_PLAYER;
		announce.player_indices[1] = SKATE_AUTHORITY_NO_PLAYER;
		announce.object_indices[0] = NONE;
		announce.object_indices[1] = NONE;
		skate_verdict_write(&announce, out + SKATE_VERDICT_SIZE * count++);
	}
	return count;
}

void skate_authority_handle_reports(long machine_index, byte const *data, byte const *end, short count)
{
	short index;

	if (game_connection() != _game_connection_network_server)
		return;
	for (index = 0; index < count && end - data >= SKATE_REPORT_SIZE; index++, data += SKATE_REPORT_SIZE)
	{
		struct skate_report report;

		if (skate_report_read(data, &report))
			skate_authority_decide(machine_index, &report);
	}
}

void skate_authority_handle_verdicts(byte const *data, byte const *end, short count)
{
	short index;

	if (game_connection() != _game_connection_network_client)
		return;
	for (index = 0; index < count && end - data >= SKATE_VERDICT_SIZE; index++, data += SKATE_VERDICT_SIZE)
	{
		struct skate_verdict verdict;
		short heard;

		if (!skate_verdict_read(data, &verdict))
			continue;
		skate_authority_globals.announced_time = game_time_get();
		if (TEST_FLAG(verdict.flags, _skate_verdict_announce_bit))
			continue;
		/* (each once: it comes a few times over) */
		for (heard = 0; heard < SKATE_AUTHORITY_HEARD; heard++)
		{
			if (skate_heard[heard].used && skate_heard[heard].verdict.number == verdict.number)
				break;
		}
		if (heard < SKATE_AUTHORITY_HEARD)
			continue;
		skate_heard[skate_authority_globals.next_heard].used = TRUE;
		skate_heard[skate_authority_globals.next_heard].time = game_time_get();
		skate_heard[skate_authority_globals.next_heard].verdict = verdict;
		skate_authority_globals.next_heard = (short)((skate_authority_globals.next_heard + 1) % SKATE_AUTHORITY_HEARD);
		skate_authority_apply(&verdict, FALSE);
	}
}

void skate_authority_update(void)
{
	long now = game_time_get();
	short index;

	if (game_connection() != _game_connection_network_client && game_connection() != _game_connection_network_server)
		return;
	/* (the host) its players' velocities */
	if (game_connection() == _game_connection_network_server)
	{
		for (index = 0; index < SKATE_NETWORK_PLAYERS; index++)
		{
			struct player_datum *player = distributed_player(index);
			long unit_index = player ? distributed_living_unit(player) : NONE;
			real_vector3d *velocity = &skate_authority_globals.velocities[index][now % SKATE_AUTHORITY_HISTORY];

			if (unit_index != NONE)
				*velocity = object_get(unit_index)->object.translational_velocity;
			else
				velocity->i = velocity->j = velocity->k = 0.f;
		}
	}
	/* this machine's skater's hits: a bail held with no verdict in time is the
	engine's own after all; old ones forgotten */
	for (index = 0; index < SKATE_AUTHORITY_HITS; index++)
	{
		struct skate_local_hit *hit = &skate_local_hits[index];

		if (hit->target_index == NONE)
			continue;
		if (!hit->resolved && hit->bail_held && now - hit->first_time > SKATE_AUTHORITY_WAIT_TICKS)
		{
			hit->resolved = TRUE;
			hit->bails = TRUE;
			platform_log("skate hit %d: no verdict came in time, this machine's skater bails as its engine held",
				hit->number);
			skate_force_bail(hit->skater_index, hit->closing);
		}
		if (now - hit->last_time > SKATE_AUTHORITY_APART_TICKS + SKATE_AUTHORITY_WAIT_TICKS || now < hit->first_time)
			hit->target_index = NONE;
	}
	/* pushes measured half a second on */
	for (index = 0; index < SKATE_AUTHORITY_MEASURES; index++)
	{
		struct skate_push_measure *measure = &skate_measures[index];
		struct object_datum *object;

		if (measure->object_index == NONE || now - measure->time < SKATE_AUTHORITY_MEASURE_TICKS)
			continue;
		object = object_try_and_get(measure->object_index);
		if (object)
		{
			platform_log("skate verdict %u: the person pushed went %.2f world units along the push in half a "
				"second", measure->number, (object->object.position.x - measure->from.x) * measure->along.i +
				(object->object.position.y - measure->from.y) * measure->along.j);
		}
		measure->object_index = NONE;
	}
	/* people run over: whether they died, and who killed them, as this
	machine has it */
	for (index = 0; index < SKATE_AUTHORITY_MEASURES; index++)
	{
		struct skate_run_over_watch *watch = &skate_run_over_watches[index];
		struct object_datum *object;
		struct player_datum *killer;
		struct player_datum *victim;
		byte killer_index = SKATE_AUTHORITY_NO_PLAYER;
		boolean friendly_fire = FALSE;
		boolean by_vehicle = FALSE;

		if (watch->object_index == NONE)
			continue;
		object = object_try_and_get(watch->object_index);
		if (object && !TEST_FLAG(object->object.damage_flags, _object_dead_bit))
		{
			if (now - watch->time > SKATE_RUN_OVER_WATCH_TICKS || now < watch->time)
			{
				platform_log("skate verdict %u: the person run over lived, shield %.2f, body %.2f", watch->number,
					object->object.shield_vitality, object->object.body_vitality);
				watch->object_index = NONE;
			}
			continue;
		}
		watch->object_index = NONE;
		if (watch->victim_player == SKATE_AUTHORITY_NO_PLAYER ||
			!distributed_get_death(watch->victim_player, &killer_index, &friendly_fire, &by_vehicle))
		{
			platform_log("skate verdict %u: the person run over died here", watch->number);
			continue;
		}
		killer = killer_index != SKATE_AUTHORITY_NO_PLAYER ? distributed_player(killer_index) : NULL;
		victim = distributed_player(watch->victim_player);
		platform_log("skate verdict %u: the person run over died here, killed by player %d%s (the skater, player %d): "
			"player %d has %d kills, player %d %d deaths", watch->number,
			killer_index == SKATE_AUTHORITY_NO_PLAYER ? -1 : killer_index, friendly_fire ? ", friendly fire" : "",
			watch->skater_player, killer_index == SKATE_AUTHORITY_NO_PLAYER ? -1 : killer_index,
			killer ? killer->statistics.kills[0] : 0, watch->victim_player, victim ? victim->statistics.deaths : 0);
	}
}
