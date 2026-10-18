/* bots_skate.c: bots on boards (bots_internal.h; port/skate/README.md, "Bots
on boards")

On a host with Skate 3 mode and its data (skate.c's skate_bots_available),
a bot rides a board to travel and steps off to fight: a skater's weapon is
holstered and the host takes no shot from one. Each tick, after the bot's
brain has chosen where to go (bots_brain.c: its path along the walking
graph, its objective, a weapon, a noise) and how to fight, this decides
whether the bot is on a board and, if so, the pad it holds (skate.c's
skate_bot_drive runs the skate engine for it):

- it gets on when the way ahead along its path rolls (walks and small drops,
  not too steep) for BOT_SKATE_BOARD_DISTANCE or more, it fights nobody, holds
  no flag or ball, and has not just stepped off;
- it steers the board toward where its brain wanted to walk (the brain's
  throttle, which is toward its next point along the path), pushes while
  slower than the speed the turn ahead allows, ollies when it stalls against a
  lip, and steps back on when a fall left it walking;
- it steps off for a fight (a target in sight), a ladder, jump or teleporter
  coming, its goal near, a flag or ball in hand, or being stuck;
- a bot that may fight (not peaceful) rides at a person on foot it sees
  near, to run them over (skate_authority.c: as a vehicle does), and a
  peaceful one never runs anyone over (skate_bot_drive's run_over). */

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "game/player_queues_new.h"
#include "memory/data.h"
#include "objects/objects.h"
#include "units/units.h"

#include "bots_internal.h"
#include "skate.h"
#include "../../skate/include/halo_skate.h"

#include <math.h>
#include <string.h>

void platform_log(const char *format, ...);
const char *config_string(const char *name);
int config_boolean(const char *name);

/* how a bot goes about boards (debug.bot_skate, for the tests) */
enum
{
	_bot_skate_normal,
	/* it stays on its board in a fight, and rides at people on foot */
	_bot_skate_ram,
	/* it never gets on one */
	_bot_skate_never,
	/* it stands still on foot (a target for one that rams) */
	_bot_skate_stand
};

/* how far along its path the way must roll for a bot to get on (world
units: 15, about 46 m); how near its goal (or a part of the way that does
not roll) it steps off */
#define BOT_SKATE_BOARD_DISTANCE 15.0f
#define BOT_SKATE_OFF_DISTANCE 2.5f
/* ... and how far for a bot that rams (debug.bot_skate) */
#define BOT_SKATE_RAM_BOARD_DISTANCE 4.0f
/* a riding bot's path: a node this near is passed (world units), and the
point it steers for at least this far ahead, further the faster (seconds
of its speed) */
#define BOT_SKATE_NODE_PASSED 1.5f
#define BOT_SKATE_LOOKAHEAD 2.5f
#define BOT_SKATE_LOOKAHEAD_SECONDS 1.0f
/* the steepest walk a board goes up and down (rise over run) */
#define BOT_SKATE_STEEPEST_UP 0.45f
#define BOT_SKATE_STEEPEST_DOWN 0.8f
/* the highest drop a board rolls off (world units) */
#define BOT_SKATE_HIGHEST_DROP 1.0f
/* the ticks a bot stays on foot after stepping off (or failing to get on) */
#define BOT_SKATE_OFF_TICKS (4 * TICKS_PER_SECOND)
/* the speeds it rides at: pushing up to the fast one on a straight way, the
slow one in a turn (world units a second: 9 m/s and 3 m/s) */
#define BOT_SKATE_FAST_SPEED (9.0f / 3.048f)
#define BOT_SKATE_SLOW_SPEED (3.0f / 3.048f)
/* the turn (radians off its heading) the left stick is pushed fully for */
#define BOT_SKATE_FULL_TURN 0.6f
/* pushes: the button held this many ticks of each cycle */
#define BOT_SKATE_PUSH_TICKS 15
#define BOT_SKATE_PUSH_CYCLE 30
/* it pushes while the way to go is within this of its course (radians): a
board too slow turns no better; riding at a person, nearly whatever the turn */
#define BOT_SKATE_PUSH_TURN 1.6f
#define BOT_SKATE_RAM_PUSH_TURN 2.6f
/* stalled pushing (slower than this, world units a second) this many ticks:
an ollie (a lip in the way); stalled this many: off, stuck */
#define BOT_SKATE_STALL_SPEED 0.15f
#define BOT_SKATE_STALL_OLLIE_TICKS 20
#define BOT_SKATE_STUCK_TICKS (4 * TICKS_PER_SECOND)
/* the ollie: the right stick down so many ticks, then up (skate.c's
scripted pad's timings) */
#define BOT_SKATE_OLLIE_DOWN_TICKS 8
#define BOT_SKATE_OLLIE_UP_TICKS 2
/* off the board, walking, this many ticks: Y, back on */
#define BOT_SKATE_STEP_ON_TICKS 15
/* ramming: a person on foot it sees within this (world units), and as high
within this, is ridden at */
#define BOT_SKATE_RAM_DISTANCE 15.0f
#define BOT_SKATE_RAM_HEIGHT 1.2f
/* grinds: the most rails and points read; a rail beside the way within reach
(world units across, no nearer than the nearest) and running within the
angle of it (radians), with this much of it left ahead; ridden at this
speed or faster (world units a second); the ollie onto it this near across;
and the ticks before another try */
#define BOT_SKATE_MAXIMUM_RAILS 2048
#define BOT_SKATE_MAXIMUM_RAIL_POINTS 8192
#define BOT_SKATE_GRIND_REACH 1.5f
#define BOT_SKATE_GRIND_NEAREST 0.05f
#define BOT_SKATE_GRIND_ALIGN 0.35f
#define BOT_SKATE_GRIND_LENGTH 2.5f
#define BOT_SKATE_GRIND_SPEED 1.2f
#define BOT_SKATE_GRIND_OLLIE_ACROSS 0.45f
#define BOT_SKATE_GRIND_COOLDOWN_TICKS (3 * TICKS_PER_SECOND)
/* the pad's buttons (halo_skate.h: XINPUT's) */
#define BOT_SKATE_A 0x1000
#define BOT_SKATE_Y 0x8000
#define BOT_SKATE_FULL 32767

/* the bot's way with boards: its entry of debug.bot_skate ("ram,never": one
for each bot, by its place among them), else normal */
static short bot_skate_mode(
	struct bot const *bot)
{
	char const *list = config_string("debug.bot_skate");
	char name[16];
	short index = 0;

	/* ("*ram": every bot so) */
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
			if (!strcmp(name, "ram"))
				return _bot_skate_ram;
			if (!strcmp(name, "never"))
				return _bot_skate_never;
			if (!strcmp(name, "stand"))
				return _bot_skate_stand;
			return _bot_skate_normal;
		}
	}
	return _bot_skate_normal;
}

/* the bot's unit: alive, a biped on foot (none in a vehicle skates) */
static boolean bot_skate_unit_able(
	long unit_index)
{
	struct object_datum *object = object_try_and_get(unit_index);

	return object && object->object.type == _object_type_biped && object->object.parent_object_index == NONE &&
		!TEST_FLAG(object->object.damage_flags, _object_dead_bit);
}

/* whether an edge of the walking graph rolls: a walk not too steep, or a
small drop */
static boolean bot_skate_edge_rolls(
	struct bots_nav const *nav,
	struct bots_nav_edge const *edge,
	struct bots_nav_point const *from)
{
	struct bots_nav_point const *to = &nav->nodes[edge->target].point;
	real dx = to->x - from->x, dy = to->y - from->y;
	real run = (real)sqrt(dx * dx + dy * dy);
	real rise = to->z - from->z;

	if (edge->type == _bots_nav_drop)
		return from->z - to->z <= BOT_SKATE_HIGHEST_DROP;
	if (edge->type != _bots_nav_walk)
		return FALSE;
	if (run < 0.05f)
		return fabs(rise) < 0.1f;
	return rise <= run * BOT_SKATE_STEEPEST_UP && -rise <= run * BOT_SKATE_STEEPEST_DOWN;
}

/* along the bot's path from where it is: how far the way rolls before a part
that does not (or its end), and whether it ends there (the goal) rather than
at such a part */
static real bot_skate_rolling_distance(
	struct bot_brain const *brain,
	struct bots_nav const *nav,
	real_point3d const *position,
	boolean *to_goal)
{
	struct bots_nav_point from;
	real distance = 0.0f;
	long step;

	*to_goal = FALSE;
	if (!nav || brain->path_next == NONE || brain->path_next < 0)
		return 0.0f;
	from.x = position->x;
	from.y = position->y;
	from.z = position->z;
	for (step = brain->path_next; step >= 0; step--)
	{
		struct bots_nav_edge const *edge = &nav->edges[brain->path[step]];
		struct bots_nav_point const *to = &nav->nodes[edge->target].point;
		real dx = to->x - from.x, dy = to->y - from.y;

		if (!bot_skate_edge_rolls(nav, edge, &from))
			return distance;
		distance += (real)sqrt(dx * dx + dy * dy);
		from = *to;
	}
	*to_goal = TRUE;
	return distance;
}

/* the point a riding bot steers for (pure pursuit): along its path, the
first node at least lookahead world units away, the nodes it passed within
BOT_SKATE_NODE_PASSED taken as reached (a board turns too wide to come to a
node's middle: it would circle it); FALSE without a path, or with its goal
reached */
static boolean bot_skate_pursuit(
	struct bot_brain *brain,
	struct bots_nav const *nav,
	real_point3d const *position,
	real lookahead,
	struct bots_nav_point *target)
{
	long step;

	if (!nav || brain->path_next == NONE || brain->path_next < 0)
		return FALSE;
	/* (the nodes passed) */
	while (brain->path_next >= 0)
	{
		struct bots_nav_point const *node = &nav->nodes[nav->edges[brain->path[brain->path_next]].target].point;
		real dx = node->x - position->x, dy = node->y - position->y;

		if (dx * dx + dy * dy > BOT_SKATE_NODE_PASSED * BOT_SKATE_NODE_PASSED || fabs(node->z - position->z) > 1.0f)
			break;
		brain->path_next--;
	}
	if (brain->path_next < 0)
	{
		brain->statistics.goals_reached++;
		brain->path_next = NONE;
		brain->goal_node = NONE;
		return FALSE;
	}
	for (step = brain->path_next; step >= 0; step--)
	{
		struct bots_nav_point const *node = &nav->nodes[nav->edges[brain->path[step]].target].point;
		real dx = node->x - position->x, dy = node->y - position->y;

		*target = *node;
		if (dx * dx + dy * dy >= lookahead * lookahead)
			break;
	}
	return TRUE;
}

static real bot_skate_angle(
	real angle)
{
	while (angle > _pi)
		angle -= 2.0f * _pi;
	while (angle < -_pi)
		angle += 2.0f * _pi;
	return angle;
}

/* the person on foot the bot rides at: its target, seen, near, about as high,
neither skating nor in a vehicle; NONE for none */
static long bot_skate_ram_target(
	struct bot const *bot,
	real_point3d const *position)
{
	struct player_datum *player;
	struct object_datum *object;
	real dx, dy;

	if (bot->difficulty == _bot_difficulty_peaceful || bot->brain.combat.target_player == NONE ||
		!bot->brain.combat.target_visible)
	{
		return NONE;
	}
	player = player_try_and_get(bot->brain.combat.target_player);
	if (!player || player->unit_index == NONE || !bot_skate_unit_able(player->unit_index) ||
		skate_unit_is_skating(player->unit_index))
	{
		return NONE;
	}
	object = object_get(player->unit_index);
	dx = object->object.position.x - position->x;
	dy = object->object.position.y - position->y;
	if (dx * dx + dy * dy > BOT_SKATE_RAM_DISTANCE * BOT_SKATE_RAM_DISTANCE ||
		fabs(object->object.position.z - position->z) > BOT_SKATE_RAM_HEIGHT)
	{
		return NONE;
	}
	return player->unit_index;
}

/* (debug.bot_skate "ram") a bot that rams, seeing nobody, heads for the
nearest other player on foot, wherever they are (a test's bot finds its
target without searching the map for it) */
static void bot_skate_ram_seek(
	struct bot *bot,
	long player_index,
	struct bots_nav *nav,
	real_point3d const *position)
{
	struct data_iterator iterator;
	struct player_datum *player;
	real best = 0.0f;
	long node = NONE;

	if (!nav || bot->difficulty == _bot_difficulty_peaceful || (game_time_get() + bot->slot) % TICKS_PER_SECOND != 0)
		return;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct object_datum *object;
		real dx, dy, dz, distance;

		if (iterator.datum_index == player_index || player->unit_index == NONE ||
			!bot_skate_unit_able(player->unit_index) || skate_unit_is_skating(player->unit_index))
		{
			continue;
		}
		object = object_get(player->unit_index);
		dx = object->object.position.x - position->x;
		dy = object->object.position.y - position->y;
		dz = object->object.position.z - position->z;
		distance = dx * dx + dy * dy + dz * dz;
		if (node == NONE || distance < best)
		{
			long found = bots_nav_locate(nav, object->object.position.x, object->object.position.y,
				object->object.position.z);

			if (found != NONE)
			{
				node = found;
				best = distance;
			}
		}
	}
	if (node != NONE && node != bot->brain.goal_node)
	{
		bot->brain.goal_node = node;
		bot->brain.path_next = NONE;
	}
}

/* ---------- grinds
The skate engine finds the map's grind rails (halo-skate's rails.rs: edges
where the ground drops away and nothing rises - a ledge's lip, a wall's top,
a railing - chained), and grinds by itself when the board lands on one. A
riding bot looks for a rail beside its way, running about the way it goes,
rides alongside it and ollies onto it. */

/* the rails of the map being played (skate_bot_rails), and the graph they
were read for (another: another map) */
static struct
{
	struct bots_nav const *nav;
	long read_tick;
	int point_count;
	int rail_count;
	float points[BOT_SKATE_MAXIMUM_RAIL_POINTS][3];
	int ends[BOT_SKATE_MAXIMUM_RAILS];
} bot_skate_rails;

/* the map's rails, read once a skater has them (again every few seconds
until one does) */
static void bot_skate_rails_read(
	struct bots_nav const *nav)
{
	long now = game_time_get();

	if (bot_skate_rails.nav == nav && (bot_skate_rails.rail_count > 0 ||
		(now >= bot_skate_rails.read_tick && now - bot_skate_rails.read_tick < 5 * TICKS_PER_SECOND)))
	{
		return;
	}
	bot_skate_rails.nav = nav;
	bot_skate_rails.read_tick = now;
	bot_skate_rails.rail_count = skate_bot_rails(&bot_skate_rails.points[0][0], BOT_SKATE_MAXIMUM_RAIL_POINTS,
		bot_skate_rails.ends, BOT_SKATE_MAXIMUM_RAILS);
	if (bot_skate_rails.rail_count < 0)
		bot_skate_rails.rail_count = 0;
	bot_skate_rails.point_count = bot_skate_rails.rail_count ? bot_skate_rails.ends[bot_skate_rails.rail_count - 1] : 0;
	if (bot_skate_rails.rail_count)
		platform_log("bots: %d grind rails (%d points) for the boards", bot_skate_rails.rail_count, bot_skate_rails.point_count);
}

/* a rail beside a riding bot's way: within BOT_SKATE_GRIND_REACH across,
running within BOT_SKATE_GRIND_ALIGN of the way it goes (course, radians) and
of the way it heads for (heading), a little above the ground or at it, with
BOT_SKATE_GRIND_LENGTH or more of it left ahead. TRUE with the point on it
nearest, the way along it, and how far across */
static boolean bot_skate_rail_beside(
	real_point3d const *position,
	real course,
	real heading,
	real *at,
	real *along,
	real *across)
{
	real cx = (real)cos(course), cy = (real)sin(course);
	real best = BOT_SKATE_GRIND_REACH;
	boolean found = FALSE;
	int rail, first = 0;

	for (rail = 0; rail < bot_skate_rails.rail_count; first = bot_skate_rails.ends[rail], rail++)
	{
		int point;

		for (point = first; point + 1 < bot_skate_rails.ends[rail]; point++)
		{
			float const *a = bot_skate_rails.points[point];
			float const *b = bot_skate_rails.points[point + 1];
			real dx = b[0] - a[0], dy = b[1] - a[1];
			real length = (real)sqrt(dx * dx + dy * dy);
			real ux, uy, t, px, py, side, height, left;

			if (length < 0.05f)
				continue;
			ux = dx / length;
			uy = dy / length;
			/* (the way along it that goes as the bot does) */
			if (ux * cx + uy * cy < 0.0f)
			{
				ux = -ux;
				uy = -uy;
			}
			if (ux * cx + uy * cy < (real)cos(BOT_SKATE_GRIND_ALIGN) ||
				fabs(bot_skate_angle((real)atan2(uy, ux) - heading)) > BOT_SKATE_GRIND_ALIGN)
			{
				continue;
			}
			t = (position->x - a[0]) * dx / length + (position->y - a[1]) * dy / length;
			if (t < 0.0f || t > length)
				continue;
			px = a[0] + dx / length * t;
			py = a[1] + dy / length * t;
			side = (real)sqrt((position->x - px) * (position->x - px) + (position->y - py) * (position->y - py));
			height = a[2] + (b[2] - a[2]) * t / length - position->z;
			/* (what is left ahead, this segment's: the next may turn away) */
			left = (dx / length * ux + dy / length * uy) > 0.0f ? length - t : t;
			if (side < BOT_SKATE_GRIND_NEAREST || side >= best || height < -0.15f || height > 0.5f ||
				left < BOT_SKATE_GRIND_LENGTH)
			{
				continue;
			}
			best = side;
			found = TRUE;
			at[0] = px;
			at[1] = py;
			at[2] = position->z + height;
			*along = (real)atan2(uy, ux);
			*across = side;
		}
	}
	return found;
}

/* the pad that rides toward heading (radians) at up to speed (world units a
second) */
static void bot_skate_pad(
	struct bot_brain *brain,
	struct skate_bot_view const *view,
	real heading,
	real speed_wanted,
	struct halo_skate_pad *pad)
{
	struct bot_skate *skate = &brain->skate;
	real speed = (real)sqrt(view->velocity[0] * view->velocity[0] + view->velocity[1] * view->velocity[1]);
	/* (the way it goes; at a standstill, the way the board points) */
	real course = speed > 0.3f ? (real)atan2(view->velocity[1], view->velocity[0]) :
		(real)atan2(view->forward[1], view->forward[0]);
	real error = bot_skate_angle(heading - course);
	real steer = PIN(error / BOT_SKATE_FULL_TURN, -1.0f, 1.0f);

	memset(pad, 0, sizeof(*pad));
	/* (the left stick to the right turns right: clockwise, a yaw falling) */
	pad->left[0] = (short)(-steer * BOT_SKATE_FULL);
	if (config_boolean("debug.bot_trace") && game_time_get() % 15 == 0)
	{
		platform_log("bots: skate trace at tick %ld: %s at (%.2f %.2f %.2f), %.2f world units a second, course %.0f, "
			"heading %.0f, error %.0f degrees, stick %d, wanted %.2f", game_time_get(), view->state, view->position[0],
			view->position[1], view->position[2], speed, course * 180.0f / _pi, heading * 180.0f / _pi,
			error * 180.0f / _pi, pad->left[0], speed_wanted);
	}
	/* off the board and walking (a fall): back on */
	if (view->off_board && !view->bailing)
	{
		if (++skate->walking_ticks >= BOT_SKATE_STEP_ON_TICKS)
		{
			pad->buttons |= BOT_SKATE_Y;
			skate->walking_ticks = 0;
		}
		return;
	}
	skate->walking_ticks = 0;
	if (view->bailing || view->airborne)
		return;
	/* an ollie under way */
	if (skate->ollie_ticks > 0)
	{
		skate->ollie_ticks--;
		pad->right[1] = (short)(skate->ollie_ticks >= BOT_SKATE_OLLIE_UP_TICKS ? -BOT_SKATE_FULL : BOT_SKATE_FULL);
		if (!skate->ollie_ticks)
			brain->statistics.skate_ollies++;
		return;
	}
	/* pushing, while slower than wanted and facing about the way to go */
	if (speed < speed_wanted && fabs(error) < (skate->ramming ? BOT_SKATE_RAM_PUSH_TURN : BOT_SKATE_PUSH_TURN))
	{
		if (game_time_get() % BOT_SKATE_PUSH_CYCLE < BOT_SKATE_PUSH_TICKS)
			pad->buttons |= BOT_SKATE_A;
		/* (stalled pushing: a lip in the way - an ollie over it) */
		if (speed < BOT_SKATE_STALL_SPEED)
		{
			if (++skate->stall_ticks == BOT_SKATE_STALL_OLLIE_TICKS)
				skate->ollie_ticks = BOT_SKATE_OLLIE_DOWN_TICKS + BOT_SKATE_OLLIE_UP_TICKS;
		}
		else
			skate->stall_ticks = 0;
	}
	else if (speed >= BOT_SKATE_STALL_SPEED)
	{
		skate->stall_ticks = 0;
	}
}

/* (bots_brain.c's bot_brain_think, last) the bot on a board or off it, and
on one its pad, the action it chose made one a skater can do: nothing but its
facing (the board carries it; it neither fires nor throws) */
void bot_skate_think(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct player_action *action)
{
	struct bot_brain *brain = &bot->brain;
	struct bot_skate *skate = &brain->skate;
	struct bots_nav *nav = bots_nav_current();
	struct skate_bot_view view;
	struct halo_skate_pad pad;
	real_point3d position = object_get(unit_index)->object.position;
	boolean has_skater;
	boolean want;
	boolean to_goal = FALSE;
	boolean fighting;
	long ram_unit;
	real rolling;
	real board_distance = BOT_SKATE_BOARD_DISTANCE;
	long now = game_time_get();

	short mode = bot_skate_mode(bot);

	if (mode == _bot_skate_stand)
	{
		bot_action_idle(action, brain->yaw, brain->pitch);
		skate->on = FALSE;
		return;
	}
	if (!skate_bots_available() || !bot_skate_unit_able(unit_index) || mode == _bot_skate_never)
	{
		skate->on = FALSE;
		return;
	}
	has_skater = skate_bot_frame(player_index, &view);
	rolling = bot_skate_rolling_distance(brain, nav, &position, &to_goal);
	fighting = brain->combat.target_player != NONE && brain->combat.target_visible;
	ram_unit = view.skating ? bot_skate_ram_target(bot, &position) : NONE;
	/* (a bot that rams fights from its board, gets on one for less, and
	seeing nobody heads for the nearest person on foot) */
	if (mode == _bot_skate_ram && bot->difficulty != _bot_difficulty_peaceful)
	{
		fighting = FALSE;
		board_distance = BOT_SKATE_RAM_BOARD_DISTANCE;
		if (!brain->combat.target_visible)
			bot_skate_ram_seek(bot, player_index, nav, &position);
	}
	/* (a life, a game: afresh) */
	if (skate->unit_index != unit_index || now < skate->changed_tick)
	{
		memset(skate, 0, sizeof(*skate));
		skate->unit_index = unit_index;
		skate->changed_tick = now;
		skate->off_until_tick = now + BOT_SKATE_OFF_TICKS;
	}
	if (skate->on)
	{
		/* off: a fight (but a person on foot to run over), a flag or ball in
		hand, its goal near, a part ahead that does not roll, stuck */
		char const *reason = NULL;

		if (fighting && ram_unit == NONE)
			reason = "a fight";
		else if (bot_holds_flag(unit_get(unit_index)))
			reason = "a flag in hand";
		else if (ram_unit == NONE && brain->path_next != NONE && rolling < BOT_SKATE_OFF_DISTANCE &&
			!(to_goal && mode == _bot_skate_ram))
		{
			reason = to_goal ? "its goal near" : "the way ahead does not roll";
		}
		else if (skate->stuck_ticks > BOT_SKATE_STUCK_TICKS)
			reason = "stuck";
		else if (has_skater && !view.skating && !view.ready && now - skate->changed_tick > 30 * TICKS_PER_SECOND)
			reason = "its skater never got ready";
		/* (still waiting for its skater, and the way left too short for it) */
		else if (!view.skating && rolling < board_distance / 2.0f)
			reason = "the way left is short";
		if (reason)
		{
			skate->on = FALSE;
			skate->changed_tick = now;
			skate->off_until_tick = now + BOT_SKATE_OFF_TICKS;
			if (view.skating)
				platform_log("bots: player %ld steps off the board (%s) at tick %ld", DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index),
					reason, now);
		}
	}
	else if (now >= skate->off_until_tick && !fighting && !bot_holds_flag(unit_get(unit_index)) &&
		rolling >= board_distance)
	{
		skate->on = TRUE;
		skate->changed_tick = now;
		skate->stuck_ticks = 0;
		skate->stall_ticks = 0;
	}
	want = skate->on;
	if (!want)
	{
		/* (its skater, if it has one, off the board; a while on foot, it lets
		it go for another bot: skate.c gives up one not asked for) */
		if (has_skater && (view.skating || now - skate->changed_tick < 2 * TICKS_PER_SECOND))
			skate_bot_drive(player_index, unit_index, FALSE, NULL, FALSE);
		skate->riding = FALSE;
		return;
	}
	memset(&pad, 0, sizeof(pad));
	if (view.skating)
	{
		real heading;
		real speed_wanted = BOT_SKATE_FAST_SPEED;
		real moved = (real)sqrt(view.velocity[0] * view.velocity[0] + view.velocity[1] * view.velocity[1]);

		if (!skate->riding)
		{
			skate->riding = TRUE;
			brain->statistics.skate_boardings++;
			platform_log("bots: player %ld gets on a board at tick %ld, %.0f world units of the way rolling",
				DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), now, rolling);
		}
		brain->statistics.skate_ticks++;
		brain->statistics.skate_travelled += brain->last_step;
		if (view.bailing && !skate->bailing)
			brain->statistics.skate_bails++;
		skate->bailing = view.bailing;
		if (view.grinding && !skate->grinding)
		{
			brain->statistics.skate_grinds++;
			platform_log("bots: player %ld grinds (%s) at tick %ld", DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), view.state, now);
		}
		skate->grinding = view.grinding;
		if (ram_unit != NONE)
		{
			/* at the person, a little ahead of where they go */
			struct object_datum *target = object_get(ram_unit);
			real dx = target->object.position.x - position.x, dy = target->object.position.y - position.y;
			/* (near, straight at them) */
			real lead = dx * dx + dy * dy < 9.0f ? 0.0f : 6.0f;
			real tx = target->object.position.x + target->object.translational_velocity.i * lead;
			real ty = target->object.position.y + target->object.translational_velocity.j * lead;

			heading = (real)atan2(ty - position.y, tx - position.x);
			if (!skate->ramming)
			{
				brain->statistics.skate_rams++;
				platform_log("bots: player %ld rides at a person on foot at tick %ld", DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index),
					now);
			}
			skate->ramming = TRUE;
		}
		else
		{
			/* along its path (pure pursuit), else where the brain wanted to
			walk */
			real along = (real)sqrt(action->throttle.i * action->throttle.i + action->throttle.j * action->throttle.j);
			struct bots_nav_point target;

			skate->ramming = FALSE;
			if (bot_skate_pursuit(brain, nav, &position, MAX(BOT_SKATE_LOOKAHEAD, moved * BOT_SKATE_LOOKAHEAD_SECONDS),
				&target))
			{
				real at[3], rail_along, rail_across;
				real course = (real)atan2(view.velocity[1], view.velocity[0]);

				heading = (real)atan2(target.y - position.y, target.x - position.x);
				/* a rail beside the way: along it, nearer, and up onto it */
				bot_skate_rails_read(nav);
				if (!view.grinding && !view.airborne && moved > BOT_SKATE_GRIND_SPEED && now >= skate->grind_after_tick &&
					bot_skate_rail_beside(&position, course, heading, at, &rail_along, &rail_across))
				{
					real toward = (real)atan2(at[1] - position.y, at[0] - position.x);

					if (!skate->lining_up)
					{
						skate->lining_up = TRUE;
						brain->statistics.skate_grind_tries++;
					}
					/* (alongside: the rail's way, turned a little toward it) */
					heading = rail_along + bot_skate_angle(toward - rail_along) * (rail_across > 0.6f ? 0.35f : 0.15f);
					if (rail_across < BOT_SKATE_GRIND_OLLIE_ACROSS && skate->ollie_ticks == 0)
					{
						skate->ollie_ticks = BOT_SKATE_OLLIE_DOWN_TICKS + BOT_SKATE_OLLIE_UP_TICKS;
						skate->grind_after_tick = now + BOT_SKATE_GRIND_COOLDOWN_TICKS;
						skate->lining_up = FALSE;
					}
				}
				else
					skate->lining_up = FALSE;
			}
			else if (along > 0.1f)
				heading = brain->yaw + (real)atan2(action->throttle.j, action->throttle.i);
			else
			{
				heading = (real)atan2(view.forward[1], view.forward[0]);
				speed_wanted = 0.0f;
			}
			/* (slower in a turn, and near the end of the rolling way) */
			if (moved > 0.3f && fabs(bot_skate_angle(heading - (real)atan2(view.velocity[1], view.velocity[0]))) > 0.5f)
				speed_wanted = BOT_SKATE_SLOW_SPEED;
			if (rolling < 2.0f * BOT_SKATE_OFF_DISTANCE + 2.0f)
				speed_wanted = MIN(speed_wanted, BOT_SKATE_SLOW_SPEED);
		}
		bot_skate_pad(brain, &view, heading, speed_wanted, &pad);
		/* (getting nowhere on the board: off, after a while) */
		if (speed_wanted > 0.0f && moved < BOT_SKATE_STALL_SPEED && !view.bailing)
			skate->stuck_ticks++;
		else
			skate->stuck_ticks = 0;
	}
	if (!skate_bot_drive(player_index, unit_index, TRUE, &pad, bot->difficulty != _bot_difficulty_peaceful))
	{
		/* (no skater to be had: on foot a while) */
		skate->on = FALSE;
		skate->off_until_tick = now + 10 * BOT_SKATE_OFF_TICKS;
		return;
	}
	if (!view.skating)
	{
		/* (its skater still being made, or not on yet: it walks on) */
		skate->riding = FALSE;
		return;
	}
	/* on the board: the brain's moves, fire and throws come to nothing (the
	board carries it, its weapon holstered); it faces the way it rides */
	{
		real yaw = (real)atan2(view.forward[1], view.forward[0]);

		bot_action_idle(action, yaw < 0.0f ? yaw + 2.0f * _pi : yaw, 0.0f);
	}
}
