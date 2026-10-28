/* bots_tricks.c: a riding bot's skate tricks (bots_internal.h; port/skate/README.md,
"Bots on boards")

A bot on a board (bots_skate.c) rides to get somewhere; this makes it skate
while it does, through the same pad a person holds: it flicks the right
stick through the gestures the skate engine knows tricks by (read from the
Skate 3 data: skate_bot_gesture), holds a trigger in the air for a grab and
the right stick part way for a manual, and lines up with the rails it
passes. The skate engine's own scoring (Skate 3's) says what came of it
(skate_bot_tricks): the tricks landed, the combos, the points, the bails.

Its chances:
- a flatground flip now and then, rolling fast and straight with room left
  on its way;
- an ollie (or a flip) over a lip in its way, off a drop, over a gap;
- in the air, with time enough before it lands, a late flip as it leaves
  the ground or a grab held to the landing;
- on a grind, a flip off it after a while;
- after landing, a manual for a moment;
how often, which flips, and how cleanly by its difficulty: peaceful and easy
bots rarely, the simplest flips, now and then a late flip begun too late (a
bail); normal ones often, with grinds; hard and legendary ones whenever
they can, chaining flips, grinds and grabs into combos, and hardly ever
bailing.

Tricks never steer: the riding code steers along the way and this only adds
the right stick, the triggers, and lets go of the push. None is begun where
the way ahead turns or ends soon, nor with an enemy about (a target seen in
the last few seconds, gunfire heard, a person ridden at): then a bot does
no more than ollie what is in its way, so that it is on its wheels when it
has to step off and fight (a skater's weapon is holstered). */

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "physics/collisions.h"

#include "bots_internal.h"
#include "skate.h"
#include "trick_attack.h"
#include "../../skate/include/halo_skate.h"

#include <math.h>
#include <string.h>

void platform_log(const char *format, ...);
const char *config_string(const char *name);
int config_boolean(const char *name);

/* the kinds of gesture a bot flicks */
enum
{
	/* a flatground flip, or over something in its way */
	_bot_gesture_flip,
	/* in the air, as it leaves the ground */
	_bot_gesture_late_flip,
	/* in the air, too late: a bail (a sloppy bot) */
	_bot_gesture_sloppy,
	/* off a grind */
	_bot_gesture_pop,
	/* an ollie over what is in its way (an enemy about: no flip) */
	_bot_gesture_ollie
};

/* how a bot skates at its difficulty */
struct bot_trick_skill
{
	/* flatground flips: how often it begins one, riding fast and straight
	with room ahead (a second) */
	real flips_per_second;
	/* the flips it flicks: bot_trick_flips[first] to [last] */
	short first_flip;
	short last_flip;
	/* the chance it takes, each time: a flip over what is in its way or off
	a drop (else an ollie); in the air, a late flip, a grab; a flip off a
	grind; a manual after landing; lining up with a rail it passes */
	real obstacle_flip_chance;
	real late_flip_chance;
	real grab_chance;
	real pop_chance;
	real manual_chance;
	real grind_chance;
	/* how far across it looks for a rail beside its way (or a little ahead),
	and the most the rail may run off its way (radians) */
	real grind_reach;
	real grind_align;
	/* the chance a late flip it means to do is begun too late (a bail) */
	real sloppy_chance;
	/* the ticks after a trick before it begins another on the ground */
	long cooldown_ticks;
};

/* the flips, simplest first (the gestures' names in skater.pat) */
static char const *const bot_trick_flips[] =
{
	"Ollie", "Kickflip", "Heelflip", "PopShuvit", "Nollie",
	"FSPopShuvit", "VarialKickflip", "VarialHeelflip", "360PopShuvit", "N_Kickflip", "N_Heelflip",
	"360Flip", "Hardflip", "InwardHeelflip", "Laserflip", "360Hardflip", "360InwardHeelflip", "N_360Flip"
};
#define BOT_TRICK_FLIP_COUNT ((short)(sizeof(bot_trick_flips) / sizeof(bot_trick_flips[0])))
/* ... the late flips (skater_air.pat; not its late shove-its, which bail
more often than not) */
static char const *const bot_trick_late_flips[] =
{
	"L_F_Kickflip", "L_F_Heelflip", "L_B_Kickflip", "L_B_Heelflip"
};
#define BOT_TRICK_LATE_FLIP_COUNT ((short)(sizeof(bot_trick_late_flips) / sizeof(bot_trick_late_flips[0])))

static struct bot_trick_skill const bot_trick_skills[NUMBER_OF_BOT_DIFFICULTIES] =
{
	/* peaceful: as easy (it skates as anyone; it only never rams) */
	{ 0.05f, 0, 4, 0.1f, 0.2f, 0.1f, 0.0f, 0.0f, 0.4f, 1.5f, 0.35f, 0.5f, 6 * TICKS_PER_SECOND },
	/* easy */
	{ 0.05f, 0, 4, 0.1f, 0.2f, 0.1f, 0.0f, 0.0f, 0.4f, 1.5f, 0.35f, 0.5f, 6 * TICKS_PER_SECOND },
	/* normal */
	{ 0.2f, 1, 10, 0.5f, 0.4f, 0.5f, 0.4f, 0.3f, 0.8f, 2.5f, 0.45f, 0.05f, (5 * TICKS_PER_SECOND) / 2 },
	/* hard */
	{ 0.8f, 3, 17, 0.85f, 0.7f, 0.7f, 0.8f, 0.5f, 1.0f, 3.5f, 0.55f, 0.01f, TICKS_PER_SECOND },
	/* legendary */
	{ 1.0f, 5, 17, 0.95f, 0.8f, 0.8f, 0.9f, 0.6f, 1.0f, 4.0f, 0.6f, 0.0f, (4 * TICKS_PER_SECOND) / 5 }
};

/* the gestures read from the data, by name (none: not there, and not
flicked) */
#define BOT_TRICK_GESTURES (BOT_TRICK_FLIP_COUNT + BOT_TRICK_LATE_FLIP_COUNT)
static struct
{
	boolean read;
	short count;
	float points[BOT_TRICK_MAXIMUM_POINTS][2];
} bot_trick_gestures[BOT_TRICK_GESTURES];

/* the pad's full stick, a trigger pulled, and the push button (halo_skate.h) */
#define BOT_TRICK_FULL 32767.0f
#define BOT_TRICK_TRIGGER 255
#define BOT_TRICK_A 0x1000
/* the crouch before a flip (ticks the stick holds the gesture's first point):
on the ground, and off a grind */
#define BOT_TRICK_WIND_TICKS 6
#define BOT_TRICK_POP_WIND_TICKS 3
/* a flatground flip: the slowest it rides for one (world units a second:
4.5 m/s), the most the way ahead may turn from its course (radians), and the
least of the way left rolling (world units, or seconds of its speed) */
#define BOT_TRICK_FLIP_SPEED (4.5f / 3.048f)
#define BOT_TRICK_STRAIGHT 0.45f
#define BOT_TRICK_ROOM 5.0f
#define BOT_TRICK_ROOM_SECONDS 1.5f
/* how often it looks for a flatground flip (ticks) */
#define BOT_TRICK_LOOK_TICKS 15
/* a flip begins only rolling (the engine's state PhysicsGround), this many
ticks or more after its last push (a foot still down takes no flip) */
#define BOT_TRICK_PUSHED_TICKS 8
/* in the air within this many ticks of a pop of its own: its own air (the
only air it grabs or flips in) */
#define BOT_TRICK_POPPED_TICKS 10
/* in the air: the least time it must have left before landing for a late
flip begun as it leaves the ground, and for a grab (seconds); how soon
(air ticks) a late flip must begin; a sloppy one begins about halfway */
#define BOT_TRICK_LATE_FLIP_AIR 0.7f
#define BOT_TRICK_GRAB_AIR 0.5f
/* ... and the most (a long fall: no trick, it lands as it can) */
#define BOT_TRICK_MOST_AIR 1.2f
#define BOT_TRICK_LATE_FLIP_TICKS 3
/* the skate engine's gravity (world units a second squared; measured in the
air as it goes) to start with */
#define BOT_TRICK_GRAVITY (9.81f / 3.048f)
/* in the way: looking ahead (seconds of its speed and world units), the
highest lip it ollies (world units above the ground), the lowest drop it
pops off, and the ticks before it looks for another */
#define BOT_TRICK_AHEAD_SECONDS 0.3f
#define BOT_TRICK_AHEAD 0.3f
#define BOT_TRICK_LIP 0.45f
#define BOT_TRICK_DROP 0.25f
/* (a face this steep or more is a lip: the up part of its normal below
this; and how far ahead the slope ridden on is measured, world units) */
#define BOT_TRICK_STEEP 0.5f
#define BOT_TRICK_SLOPE_RUN 0.3f
#define BOT_TRICK_OBSTACLE_TICKS 20
/* an enemy about for so long after it was seen or heard (ticks) */
#define BOT_TRICK_DANGER_TICKS (3 * TICKS_PER_SECOND)
/* a manual: the right stick part way down, for so many ticks */
#define BOT_TRICK_MANUAL_STICK 0.5f
#define BOT_TRICK_MANUAL_TICKS 15
#define BOT_TRICK_MANUAL_MORE_TICKS 20
/* off a grind: after so many ticks on it, and up to so many more */
#define BOT_TRICK_POP_TICKS 12
#define BOT_TRICK_POP_MORE_TICKS 30

/* the difficulty the bot does tricks at: its own, or its entry of
debug.bot_tricks ("off,hard": one for each bot, by its place among them;
"*off": every bot) - a difficulty's name (the tests: peaceful bots, who
fight nobody and ride the more, tricking as hard ones do), or "off" (NONE:
no tricks) */
static char const *const bot_trick_difficulty_names[NUMBER_OF_BOT_DIFFICULTIES] =
	{ "peaceful", "easy", "normal", "hard", "legendary" };

static short bot_tricks_level(
	struct bot const *bot)
{
	char const *list = config_string("debug.bot_tricks");
	char name[16];
	short index = 0;
	boolean every = list && *list == '*';

	if (every)
		list++;
	while (list && *list)
	{
		short length = 0;

		while (*list == ',' || *list == ' ')
			list++;
		while (*list && *list != ',' && length < (short)sizeof(name) - 1)
			name[length++] = *list++;
		name[length] = 0;
		if (length > 0 && (every || index++ == bot->slot))
		{
			short difficulty;

			if (!strcmp(name, "off"))
				return NONE;
			for (difficulty = 0; difficulty < NUMBER_OF_BOT_DIFFICULTIES; difficulty++)
			{
				if (!strcmp(name, bot_trick_difficulty_names[difficulty]))
					return difficulty;
			}
			break;
		}
	}
	return bot->difficulty;
}

static struct bot_trick_skill const *bot_trick_skill(
	struct bot const *bot)
{
	short difficulty = bot_tricks_level(bot);

	if (difficulty < 0 || difficulty >= NUMBER_OF_BOT_DIFFICULTIES)
		difficulty = _bot_difficulty_normal;
	return &bot_trick_skills[difficulty];
}

/* the gesture of that index (flips, then late flips), read from the data
once: its points, how many (0: none) */
static short bot_trick_gesture(
	short index,
	float const (**points)[2])
{
	char const *name = index < BOT_TRICK_FLIP_COUNT ? bot_trick_flips[index] :
		bot_trick_late_flips[index - BOT_TRICK_FLIP_COUNT];

	if (!bot_trick_gestures[index].read)
	{
		int count = skate_bot_gesture(name, &bot_trick_gestures[index].points[0][0], BOT_TRICK_MAXIMUM_POINTS);

		bot_trick_gestures[index].count = (short)(count > 0 ? count : 0);
		/* (none read: the data not there yet - asked again next time) */
		bot_trick_gestures[index].read = count > 0;
	}
	*points = (float const (*)[2])bot_trick_gestures[index].points;
	return bot_trick_gestures[index].count;
}

/* the gesture's name of that index */
static char const *bot_trick_gesture_name(
	short index)
{
	return index < BOT_TRICK_FLIP_COUNT ? bot_trick_flips[index] : bot_trick_late_flips[index - BOT_TRICK_FLIP_COUNT];
}

/* notes the trick begun, for the log */
static void bot_trick_doing(
	struct bot_tricks *tricks,
	char const *what,
	char const *name,
	real air)
{
	tricks->doing = what;
	tricks->doing_name = name;
	tricks->doing_tick = game_time_get();
	tricks->doing_air = air;
}

/* begins flicking the gesture of that index (its first point held wind
ticks): FALSE when the data has none */
static boolean bot_trick_flick(
	struct bot_tricks *tricks,
	short index,
	short kind,
	short wind)
{
	float const (*points)[2];
	short count = bot_trick_gesture(index, &points);

	if (count < 2)
		return FALSE;
	memcpy(tricks->gesture, points, sizeof(float) * 2 * count);
	tricks->gesture_count = count;
	tricks->gesture_next = 0;
	tricks->gesture_ticks = wind;
	tricks->gesture_kind = kind;
	tricks->gesture_index = index;
	{
		static char const *const kinds[] = { "a flip", "a late flip", "a late flip begun late", "a flip off a grind", "an ollie" };

		bot_trick_doing(tricks, kinds[kind], bot_trick_gesture_name(index), 0.0f);
	}
	return TRUE;
}

/* the pad for the gesture under way: its point this tick (the first held its
wind ticks, each other one), then a tick let go, and done; no push meanwhile */
static void bot_trick_gesture_step(
	struct bot_tricks *tricks,
	struct skate_bot_view const *view,
	struct halo_skate_pad *pad)
{
	if (tricks->gesture_next < tricks->gesture_count)
	{
		pad->right[0] = (short)(tricks->gesture[tricks->gesture_next][0] * BOT_TRICK_FULL);
		pad->right[1] = (short)(tricks->gesture[tricks->gesture_next][1] * BOT_TRICK_FULL);
		/* (held until its skater has stepped with it) */
		if (view->steps_now && --tricks->gesture_ticks <= 0)
		{
			tricks->gesture_next++;
			tricks->gesture_ticks = 1;
		}
	}
	else
	{
		pad->right[0] = 0;
		pad->right[1] = 0;
		if (view->steps_now)
			tricks->gesture_count = 0;
	}
	pad->buttons &= (unsigned short)~BOT_TRICK_A;
	if (tricks->grabbing)
		pad->triggers[tricks->grab_trigger] = BOT_TRICK_TRIGGER;
}

/* a flip of its difficulty (an index into bot_trick_flips): none of the
last few it flicked, where it knows enough others (the scoring pays less
for a trick repeated) */
static short bot_trick_choose_flip(
	struct bot *bot)
{
	struct bot_trick_skill const *skill = bot_trick_skill(bot);
	struct bot_tricks *tricks = &bot->brain.skate.tricks;
	short span = (short)(skill->last_flip - skill->first_flip + 1);
	short pick = skill->first_flip;
	short tries;

	for (tries = 0; tries < 8; tries++)
	{
		short recent;
		boolean repeated = FALSE;

		pick = (short)PIN(skill->first_flip + (short)(bot_random_real(&bot->brain) * span), 0, BOT_TRICK_FLIP_COUNT - 1);
		for (recent = 0; recent < BOT_TRICK_RECENT && recent < span - 1; recent++)
			repeated |= tricks->recent[recent] == pick + 1;
		if (!repeated)
			break;
	}
	memmove(&tricks->recent[1], &tricks->recent[0], sizeof(tricks->recent) - sizeof(tricks->recent[0]));
	tricks->recent[0] = (short)(pick + 1);
	return pick;
}

/* the level (and its scenery) in the way from point along vector: TRUE with
how far along (0 to 1) and the up part of the surface's normal (1: flat
ground, 0: a wall) */
static boolean bot_trick_ray(
	real_point3d const *point,
	real_vector3d const *vector,
	long ignore,
	real *t,
	real *up)
{
	struct collision_result collision;

	if (!collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit) |
		FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) |
		FLAG(_collision_test_objects_machines_bit), (real_point3d *)point, (real_vector3d *)vector, ignore, &collision))
	{
		return FALSE;
	}
	*t = collision.t;
	*up = collision.plane.n.k;
	return TRUE;
}

/* the ground under (x, y) from z down (world units: 4 at most); FALSE for
none */
static boolean bot_trick_ground(
	real x,
	real y,
	real z,
	long ignore,
	real *ground)
{
	real_point3d point;
	real_vector3d down;
	real t, up;

	point.x = x;
	point.y = y;
	point.z = z;
	down.i = 0.0f;
	down.j = 0.0f;
	down.k = -4.0f;
	if (!bot_trick_ray(&point, &down, ignore, &t, &up))
		return FALSE;
	*ground = z - 4.0f * t;
	return TRUE;
}

/* what is in its way along its course (unit x, y), riding at speed on
ground at ground: TRUE for a lip low enough to ollie (a steep face low
ahead, nothing at the height above it: not a ramp), or a drop or a gap (the
ground ahead well below where the slope it rides on would have it), each
near enough to pop now */
static boolean bot_trick_in_the_way(
	real_point3d const *position,
	real ground,
	real cx,
	real cy,
	real speed,
	long ignore)
{
	real ahead = speed * BOT_TRICK_AHEAD_SECONDS + BOT_TRICK_AHEAD;
	real_point3d low, high;
	real_vector3d along;
	real t, up, near, there;

	along.i = cx * (ahead + 0.3f);
	along.j = cy * (ahead + 0.3f);
	along.k = 0.0f;
	low.x = high.x = position->x;
	low.y = high.y = position->y;
	low.z = ground + 0.08f;
	high.z = ground + BOT_TRICK_LIP;
	if (bot_trick_ray(&low, &along, ignore, &t, &up))
		return up < BOT_TRICK_STEEP && !bot_trick_ray(&high, &along, ignore, &t, &up);
	/* (the slope it rides on: the ground a little ahead) */
	if (!bot_trick_ground(position->x + cx * BOT_TRICK_SLOPE_RUN, position->y + cy * BOT_TRICK_SLOPE_RUN, ground + 0.5f,
		ignore, &near))
	{
		near = ground;
	}
	if (!bot_trick_ground(position->x + cx * ahead, position->y + cy * ahead, ground + 0.5f, ignore, &there))
		there = ground - 10.0f;
	return there < ground + (near - ground) * ahead / BOT_TRICK_SLOPE_RUN - BOT_TRICK_DROP;
}

/* an enemy about: a target seen lately, gunfire heard lately, or a person
it rides at */
static boolean bot_trick_danger(
	struct bot const *bot,
	long now)
{
	struct bot_combat const *combat = &bot->brain.combat;

	if (bot->brain.skate.ramming)
		return TRUE;
	/* (Trick Attack: nobody can shoot - only a skater riding at it is) */
	if (trick_attack_active())
		return FALSE;
	if (combat->target_player != NONE && (combat->target_visible || now - combat->last_seen_tick < BOT_TRICK_DANGER_TICKS))
		return TRUE;
	return combat->heard_tick != 0 && now >= combat->heard_tick && now - combat->heard_tick < BOT_TRICK_DANGER_TICKS;
}

/* Trick Attack (trick_attack.c): the unbanked combo past which a bot stops
starting tricks and rides on clean, for it to bank, by its trick
difficulty (an easy one banks what little it has; a legendary one chains
on), and a hunter this near (world units) makes it bank now */
static long const bot_trick_bank_points[NUMBER_OF_BOT_DIFFICULTIES] = { 1500, 1500, 4000, 8000, 12000 };
#define BOT_TRICK_HUNTER_NEAR 12.0f

/* Trick Attack: whether the bot, the player, holds off new tricks for its
combo to bank (told once each time, for the tests) */
static boolean bot_trick_banking(
	struct bot *bot,
	long player_index,
	long unit_index)
{
	short level = bot_tricks_level(bot);
	long unbanked;
	boolean banking = FALSE;

	if (!trick_attack_active() || level == NONE)
		return FALSE;
	unbanked = trick_attack_unbanked(player_index);
	if (unbanked >= bot_trick_bank_points[PIN(level, 0, NUMBER_OF_BOT_DIFFICULTIES - 1)])
		banking = TRUE;
	/* (hard and better: another skater close, it banks what it has) */
	else if (unbanked > 0 && level >= _bot_difficulty_hard)
	{
		struct data_iterator iterator;
		struct player_datum *player;
		real_point3d position;

		object_get_origin(unit_index, &position);
		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			real_point3d other;

			if (iterator.datum_index == player_index || player->unit_index == NONE ||
				!object_try_and_get(player->unit_index))
			{
				continue;
			}
			object_get_origin(player->unit_index, &other);
			if ((other.x - position.x) * (other.x - position.x) + (other.y - position.y) * (other.y - position.y) <
				BOT_TRICK_HUNTER_NEAR * BOT_TRICK_HUNTER_NEAR && fabs(other.z - position.z) < 2.0f)
			{
				banking = TRUE;
				break;
			}
		}
	}
	if (banking && !bot->brain.skate.tricks.banking)
	{
		platform_log("bots: player %ld rides on to bank a combo of %ld at tick %ld",
			DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), unbanked, game_time_get());
	}
	bot->brain.skate.tricks.banking = banking;
	return banking;
}

/* the ticks after a trick before it begins another: its skill's; in Trick
Attack, chaining a combo, soon enough for the next landing to come within
the bank delay (normal and better) */
static long bot_trick_cooldown(
	struct bot const *bot,
	struct bot_trick_skill const *skill)
{
	short level = bot_tricks_level(bot);

	if (trick_attack_active() && level >= _bot_difficulty_normal)
		return MIN(skill->cooldown_ticks, trick_attack_bank_ticks() / 3);
	return skill->cooldown_ticks;
}

/* (bots_skate.c) how far across the bot looks for a rail beside its way
(world units) and how far off its way the rail may run (radians): its
difficulty's; doing no tricks, an easy bot's */
void bot_tricks_grind_reach(
	struct bot const *bot,
	real *reach,
	real *align)
{
	struct bot_trick_skill const *skill = bot_tricks_level(bot) == NONE ? &bot_trick_skills[_bot_difficulty_easy] :
		bot_trick_skill(bot);

	*reach = skill->grind_reach;
	*align = skill->grind_align;
}

/* (bots_skate.c) whether the bot lines up with the rail it found beside
its way: its difficulty's chance, once for each rail (FALSE: it lets this
one go by). A grind is a way to go as good as any: an enemy about changes
nothing */
boolean bot_tricks_grind_wanted(
	struct bot *bot)
{
	/* (no tricks: every rail, as riding alone does) */
	if (bot_tricks_level(bot) == NONE)
		return TRUE;
	return bot_random_real(&bot->brain) < bot_trick_skill(bot)->grind_chance;
}

/* the skate engine's tally for the bot's skater: what changed since the
last look into its statistics, and each sequence landed or bailed told of */
static void bot_trick_tally(
	struct bot *bot,
	long player_index)
{
	struct bot_tricks *tricks = &bot->brain.skate.tricks;
	struct bot_statistics *statistics = &bot->brain.statistics;
	struct halo_skate_tricks now;
	int session;

	if (!skate_bot_tricks(player_index, &now, &session))
		return;
	/* (another session, or counting afresh: from here) */
	if (session != tricks->session || now.sequences < tricks->sequences || now.bails < tricks->bails ||
		now.landed < tricks->landed)
	{
		tricks->session = session;
		tricks->sequences = now.sequences;
		tricks->bails = now.bails;
		tricks->landed = now.landed;
		tricks->combos = now.combos;
		tricks->points = now.points;
		return;
	}
	if (now.sequences > tricks->sequences)
	{
		long landed = (long)(now.landed - tricks->landed);
		real points = now.points - tricks->points;
		char name[32];
		/* (ID_TRICK_FLIP_KICKFLIP: the part after ID_TRICK_) */
		char const *last = strncmp(now.last_trick, "ID_TRICK_", 9) ? now.last_trick : now.last_trick + 9;

		strncpy(name, last, sizeof(name) - 1);
		name[sizeof(name) - 1] = 0;
		statistics->skate_tricks += landed;
		statistics->skate_trick_points += points;
		statistics->skate_combos += (long)(now.combos - tricks->combos);
		/* (a sequence at a time, nearly always: if several, the most) */
		if (now.sequences - tricks->sequences == 1 && landed > statistics->skate_best_combo)
			statistics->skate_best_combo = landed;
		if (points > statistics->skate_best_points)
			statistics->skate_best_points = points;
			platform_log("bots: player %ld lands %ld tricks for %.0f points (last %s) at tick %ld",
			DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), landed, points, name, game_time_get());
	}
	if (now.bails > tricks->bails)
	{
		statistics->skate_trick_bails += (long)(now.bails - tricks->bails);
		platform_log("bots: player %ld bails out of a trick at tick %ld (%s %s begun %ld ticks before, %.2f seconds of air "
			"expected)", DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), game_time_get(), tricks->doing ? tricks->doing : "nothing",
			tricks->doing_name ? tricks->doing_name : "", game_time_get() - tricks->doing_tick, tricks->doing_air);
	}
	tricks->sequences = now.sequences;
	tricks->bails = now.bails;
	tricks->landed = now.landed;
	tricks->combos = now.combos;
	tricks->points = now.points;
}

void bot_tricks_think(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct skate_bot_view const *view,
	struct bot_trick_way const *way,
	struct halo_skate_pad *pad)
{
	struct bot_skate *skate = &bot->brain.skate;
	struct bot_tricks *tricks = &skate->tricks;
	struct bot_statistics *statistics = &bot->brain.statistics;
	struct bot_trick_skill const *skill = bot_trick_skill(bot);
	long now = game_time_get();
	real speed = (real)sqrt(view->velocity[0] * view->velocity[0] + view->velocity[1] * view->velocity[1]);
	real cx = 1.0f, cy = 0.0f;
	real ground = view->position[2];
	real_point3d position;
	boolean has_ground;
	boolean danger = bot_trick_danger(bot, now);
	boolean allowed = (bot_tricks_level(bot) != NONE);
	boolean on_wheels = !view->airborne && !view->grinding && !view->bailing && !view->off_board;
	boolean was_airborne = tricks->airborne;
	boolean was_grinding = tricks->grinding;
	boolean rolling;

	bot_trick_tally(bot, player_index);
	/* (Trick Attack: a combo big enough, or a hunter near, rides on to bank) */
	if (bot_trick_banking(bot, player_index, unit_index))
		allowed = FALSE;
	position.x = view->position[0];
	position.y = view->position[1];
	position.z = view->position[2];
	if (speed > 0.3f)
	{
		cx = view->velocity[0] / speed;
		cy = view->velocity[1] / speed;
	}
	else if (view->forward[0] * view->forward[0] + view->forward[1] * view->forward[1] > 0.01f)
	{
		real length = (real)sqrt(view->forward[0] * view->forward[0] + view->forward[1] * view->forward[1]);

		cx = view->forward[0] / length;
		cy = view->forward[1] / length;
	}
	has_ground = bot_trick_ground(position.x, position.y, position.z + 0.2f, unit_index, &ground);
	/* (how high its origin rides over the ground: to tell how far it is
	above the ground in the air) */
	if (on_wheels && has_ground && position.z - ground < 1.0f)
		tricks->ride_height = tricks->ride_height_known ? tricks->ride_height * 0.8f + (position.z - ground) * 0.2f :
			position.z - ground;
	tricks->ride_height_known |= on_wheels && has_ground;

	/* ---------- the air: since when, its gravity, the time left */
	tricks->airborne = view->airborne;
	tricks->grinding = view->grinding;
	/* (a pop of its own under way: a gesture, or its riding's ollie; pushing) */
	if (tricks->gesture_count > 0 || way->ollie_busy)
		tricks->popped_tick = now;
	/* (a push it means to make: not while it waits to flip, when none is made) */
	if ((pad->buttons & BOT_TRICK_A) && tricks->flip_until_tick == NONE)
		tricks->pushed_tick = now;
	if (view->airborne && !was_airborne)
	{
		tricks->air_tick = now;
		/* (in the air by its own pop, not rolled off an edge or thrown) */
		tricks->popped = now - tricks->popped_tick <= BOT_TRICK_POPPED_TICKS;
		tricks->grabbing = FALSE;
		tricks->sloppy_tick = NONE;
	}
	if (view->airborne)
	{
		/* (the fall's own measure of gravity, once going down) */
		if (was_airborne && tricks->last_vz > view->velocity[2])
		{
			real measured = (tricks->last_vz - view->velocity[2]) * TICKS_PER_SECOND;

			if (measured > 1.0f && measured < 20.0f)
				tricks->gravity = tricks->gravity > 0.0f ? tricks->gravity * 0.7f + measured * 0.3f : measured;
		}
	}
	tricks->last_vz = view->velocity[2];
	if (tricks->gravity <= 0.0f)
		tricks->gravity = BOT_TRICK_GRAVITY;
	/* landed: a manual for a moment, now and then */
	if (!view->airborne && was_airborne && on_wheels)
	{
		tricks->grabbing = FALSE;
		if (allowed && !danger && way->rolling > BOT_TRICK_ROOM && fabs(way->turn) < BOT_TRICK_STRAIGHT &&
			bot_random_real(&bot->brain) < skill->manual_chance)
		{
			tricks->manual_ticks = (short)(BOT_TRICK_MANUAL_TICKS +
				(short)(bot_random_real(&bot->brain) * BOT_TRICK_MANUAL_MORE_TICKS));
			statistics->skate_manuals++;
			bot_trick_doing(tricks, "a manual", "", 0.0f);
		}
	}
	if (view->grinding && !was_grinding)
	{
		tricks->pop_tick = NONE;
		if (allowed && !danger && bot_random_real(&bot->brain) < skill->pop_chance)
			tricks->pop_tick = now + BOT_TRICK_POP_TICKS + (long)(bot_random_real(&bot->brain) * BOT_TRICK_POP_MORE_TICKS);
	}

	/* ---------- a gesture under way: its points in turn (nothing else) */
	if (tricks->gesture_count > 0 && !view->bailing && !view->off_board)
	{
		bot_trick_gesture_step(tricks, view, pad);
		return;
	}
	tricks->gesture_count = 0;
	if (view->bailing || view->off_board)
	{
		tricks->manual_ticks = 0;
		tricks->grabbing = FALSE;
		return;
	}
	/* (the riding code's own ollie under way: its stick) */
	if (way->ollie_busy)
		return;

	/* ---------- in the air */
	if (view->airborne)
	{
		long air_ticks = now - tricks->air_tick;
		real height = has_ground ? position.z - ground - (tricks->ride_height_known ? tricks->ride_height : 0.0f) : 1.0f;
		real vz = view->velocity[2];
		real left = (vz + (real)sqrt(vz * vz + 2.0f * tricks->gravity * MAX(height, 0.0f))) / tricks->gravity;

		if (air_ticks <= 1 && allowed && !danger && tricks->popped && left <= BOT_TRICK_MOST_AIR)
		{
			real chance = bot_random_real(&bot->brain);

			/* (a late flip it means: begun too late now and then, about halfway
			through the air) */
			if (left >= BOT_TRICK_LATE_FLIP_AIR && chance < skill->late_flip_chance &&
				bot_random_real(&bot->brain) < skill->sloppy_chance)
			{
				tricks->sloppy_tick = now + (long)(left * TICKS_PER_SECOND * 0.45f);
				tricks->sloppy_air = left;
			}
			else if (left >= BOT_TRICK_LATE_FLIP_AIR && chance < skill->late_flip_chance)
			{
				short index = (short)(BOT_TRICK_FLIP_COUNT +
					(short)(bot_random_real(&bot->brain) * BOT_TRICK_LATE_FLIP_COUNT));

				if (bot_trick_flick(tricks, index, _bot_gesture_late_flip, 1))
				{
					statistics->skate_late_flips++;
					tricks->doing_air = left;
				}
			}
			/* (a grab, or after a late flip with time to spare, both) */
			if (left >= BOT_TRICK_GRAB_AIR && bot_random_real(&bot->brain) < skill->grab_chance &&
				(tricks->gesture_count == 0 || left >= BOT_TRICK_LATE_FLIP_AIR + BOT_TRICK_GRAB_AIR))
			{
				tricks->grabbing = TRUE;
				tricks->grab_trigger = (short)(bot_random_real(&bot->brain) < 0.5f ? 0 : 1);
				statistics->skate_grabs++;
				if (tricks->gesture_count == 0)
					bot_trick_doing(tricks, "a grab", "", left);
			}
			if (tricks->gesture_count > 0)
			{
				bot_trick_gesture_step(tricks, view, pad);
				return;
			}
		}
		if (tricks->sloppy_tick != NONE && now >= tricks->sloppy_tick)
		{
			short index = (short)(BOT_TRICK_FLIP_COUNT +
				(short)(bot_random_real(&bot->brain) * BOT_TRICK_LATE_FLIP_COUNT));

			tricks->sloppy_tick = NONE;
			if (bot_trick_flick(tricks, index, _bot_gesture_sloppy, 1))
			{
				statistics->skate_late_flips++;
				tricks->doing_air = tricks->sloppy_air;
			}
		}
		if (tricks->grabbing)
			pad->triggers[tricks->grab_trigger] = BOT_TRICK_TRIGGER;
		return;
	}
	tricks->grabbing = FALSE;
	tricks->sloppy_tick = NONE;

	/* ---------- on a grind: a flip off it, after a while */
	if (view->grinding)
	{
		if (tricks->pop_tick != NONE && now >= tricks->pop_tick)
		{
			tricks->pop_tick = NONE;
			if (bot_trick_flick(tricks, bot_trick_choose_flip(bot), _bot_gesture_pop, BOT_TRICK_POP_WIND_TICKS))
			{
				statistics->skate_pops++;
				tricks->next_tick = now + bot_trick_cooldown(bot, skill);
			}
		}
		return;
	}

	/* ---------- rolling */
	if (tricks->manual_ticks > 0)
	{
		tricks->manual_ticks--;
		pad->right[1] = (short)(-BOT_TRICK_MANUAL_STICK * BOT_TRICK_FULL);
		pad->buttons &= (unsigned short)~BOT_TRICK_A;
		/* (the way turning, or someone about: down off it) */
		if (danger || fabs(way->turn) > BOT_TRICK_STRAIGHT * 2.0f)
			tricks->manual_ticks = 0;
		return;
	}
	rolling = !strcmp(view->state, "PhysicsGround") && now - tricks->pushed_tick > BOT_TRICK_PUSHED_TICKS;
	/* something in its way: an ollie (or, no enemy about, a flip) over it */
	if (has_ground && now >= tricks->obstacle_tick && speed > 0.6f && !way->lining_up &&
		bot_trick_in_the_way(&position, ground, cx, cy, speed, unit_index))
	{
		boolean flip = allowed && !danger && rolling && speed >= BOT_TRICK_FLIP_SPEED &&
			bot_random_real(&bot->brain) < skill->obstacle_flip_chance;

		tricks->obstacle_tick = now + BOT_TRICK_OBSTACLE_TICKS;
		if (bot_trick_flick(tricks, flip ? bot_trick_choose_flip(bot) : 0, flip ? _bot_gesture_flip : _bot_gesture_ollie,
			BOT_TRICK_POP_WIND_TICKS))
		{
			statistics->skate_obstacle_pops++;
			if (flip)
				statistics->skate_flips++;
			bot_trick_gesture_step(tricks, view, pad);
		}
		return;
	}
	/* a flatground flip, now and then: meant (a look every so often), then
	flicked once rolling with its feet on the board (it stops pushing for it) */
	if (allowed && !danger && tricks->flip_until_tick == NONE && now >= tricks->next_tick &&
		(now + bot->slot) % BOT_TRICK_LOOK_TICKS == 0 && speed >= BOT_TRICK_FLIP_SPEED &&
		fabs(way->turn) < BOT_TRICK_STRAIGHT && !way->lining_up &&
		way->rolling >= MAX(BOT_TRICK_ROOM, speed * BOT_TRICK_ROOM_SECONDS) &&
		bot_random_real(&bot->brain) < skill->flips_per_second * BOT_TRICK_LOOK_TICKS / TICKS_PER_SECOND)
	{
		tricks->flip_until_tick = now + 2 * BOT_TRICK_LOOK_TICKS;
	}
	if (tricks->flip_until_tick == NONE)
		return;
	if (now > tricks->flip_until_tick || danger || !allowed)
	{
		tricks->flip_until_tick = NONE;
		return;
	}
	pad->buttons &= (unsigned short)~BOT_TRICK_A;
	if (rolling && speed >= BOT_TRICK_FLIP_SPEED && fabs(way->turn) < BOT_TRICK_STRAIGHT && !way->lining_up &&
		way->rolling >= MAX(BOT_TRICK_ROOM, speed * BOT_TRICK_ROOM_SECONDS))
	{
		tricks->flip_until_tick = NONE;
		if (bot_trick_flick(tricks, bot_trick_choose_flip(bot), _bot_gesture_flip, BOT_TRICK_WIND_TICKS))
		{
			statistics->skate_flips++;
			tricks->next_tick = now + bot_trick_cooldown(bot, skill);
			bot_trick_gesture_step(tricks, view, pad);
		}
	}
}

boolean bot_tricks_under_way(
	struct bot const *bot,
	struct skate_bot_view const *view)
{
	struct bot_tricks const *tricks = &bot->brain.skate.tricks;

	return tricks->gesture_count > 0 || tricks->manual_ticks > 0 || tricks->grabbing ||
		(view->airborne && tricks->doing_tick >= tricks->air_tick - BOT_TRICK_POPPED_TICKS && tricks->doing &&
		strcmp(tricks->doing, "an ollie"));
}

/* (bots.c, the report) the bot's tricks, its own line */
void bot_tricks_report(
	struct bot const *bot,
	char const *name,
	long now)
{
	struct bot_statistics const *statistics = &bot->brain.statistics;

	if (!statistics->skate_boardings)
		return;
	platform_log("bots: %s's tricks at tick %ld: %ld landed, %.0f points, %ld combos (best %ld tricks, %.0f points), "
		"%ld bails; %ld flips, %ld late flips, %ld grabs, %ld manuals, %ld flips off grinds, %ld pops over things in the way",
		name, now, statistics->skate_tricks, statistics->skate_trick_points, statistics->skate_combos,
		statistics->skate_best_combo, statistics->skate_best_points, statistics->skate_trick_bails,
		statistics->skate_flips, statistics->skate_late_flips, statistics->skate_grabs, statistics->skate_manuals,
		statistics->skate_pops, statistics->skate_obstacle_pops);
}
