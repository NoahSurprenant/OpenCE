/* bots.c: players the host plays itself (bots.h)

The host's roster of bots: each is a player of a bot machine
(network_server_manager.c's network_game_server_port_add_bot), added to the
lobby or to the game being played as any player is, so that every machine,
a build that knows nothing of bots too, has it as another machine's player:
in the scores, the kill messages, moving as anyone's player moves. Nothing
on the wire is new (port/linux/NETCODE.md, "Bots").

Each tick of the host (player_queues_new.c's update_server_next_update,
before the tick is played) every bot's brain (bots_brain.c) gives its
player's input, which the host takes as it takes its own players' and sends
on to the other machines as it sends everyone's. A bot never runs on a
client.

server.bots is how many bots a host's games have (a dedicated server's, and
a game the menus host); they are added once the host has a lobby or a game,
and stay from game to game as the other players do. Co-op games have none. */

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "game/player_queues_new.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_server_manager.h"
#include "networking/network_server_manager_internal.h"
#include "objects/objects.h"
#include "text/unicode.h"
#include "units/units.h"

#include "bots_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (port_config.c, the port's log) */
long config_integer(const char *name);
const char *config_string(const char *name);
void platform_log(const char *format, ...);
/* console.c's */
void console_printf(boolean clear, const char *format, ...);
void console_warning(const char *format, ...);

/* a player's name's characters, its terminator among them (players.h's network_player) */
#define BOT_NAME_LENGTH 12

/* the call-signs after the name tag: a bot's name, as a player's, is at most
11 characters */
static wchar_t const *const bot_names[] =
{
	L"Alpha", L"Bravo", L"Delta", L"Echo", L"Golf", L"Hotel", L"India", L"Kilo",
	L"Lima", L"Mike", L"Oscar", L"Papa", L"Romeo", L"Tango", L"Zulu", L"Ash",
	L"Brick", L"Cobra", L"Dutch", L"Ember", L"Flint", L"Ghost", L"Hawk", L"Iron",
	L"Jade", L"Knox", L"Lynx", L"Moss", L"Nova", L"Onyx", L"Pike", L"Rook",
};

static struct
{
	struct bot bots[BOTS_MAXIMUM];
	/* how many the host's games have: server.bots when the host began */
	short wanted;
	boolean hosting;
	/* the bots added since the host began (each one's random numbers) */
	unsigned long added_count;
	/* the game tick the bots last told the log what they had done */
	long reported_tick;
} bots_globals;

/* how often (ticks) the bots tell the log what they have done */
#define BOTS_REPORT_TICKS (20 * TICKS_PER_SECOND)

/* server.bot_difficulty: easy, normal, hard or legendary (normal for
anything else) */
static short bots_difficulty_setting(
	void)
{
	static char const *const names[NUMBER_OF_BOT_DIFFICULTIES] = { "easy", "normal", "hard", "legendary" };
	char const *setting = config_string("server.bot_difficulty");
	short index;

	for (index = 0; setting && index < NUMBER_OF_BOT_DIFFICULTIES; index++)
	{
		if (!strcmp(setting, names[index]))
			return index;
	}
	return _bot_difficulty_normal;
}

/* ---------- the roster */

static void bots_name_text(
	wchar_t const *wide,
	char *text,
	int size)
{
	int index;

	for (index = 0; index < size - 1 && wide[index]; index++)
		text[index] = wide[index] >= 32 && wide[index] < 127 ? (char)wide[index] : '?';
	text[index] = 0;
}

/* whether a player of the host's game has the name */
static boolean bots_name_taken(
	struct network_game const *game,
	wchar_t const *name)
{
	long index;

	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		struct network_player const *player = &game->players[index];

		if (network_player_is_valid((struct network_player *)player) &&
			!ustrncmp(player->name, name, NUMBEROF(player->name)))
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* a name for a new bot that no player of the game has */
static void bots_new_name(
	struct network_game const *game,
	wchar_t *name,
	int size)
{
	short index;

	for (index = 0; index < (short)NUMBEROF(bot_names); index++)
	{
		ustrncpy(name, BOTS_NAME_TAG, size - 1);
		name[size - 1] = 0;
		ustrncat(name, bot_names[index], size - 1 - (int)ustrlen(name));
		if (!bots_name_taken(game, name))
			return;
	}
	/* (all taken: the host's numbering makes it unique) */
}

static short bots_count(
	void)
{
	short count = 0;
	short index;

	for (index = 0; index < BOTS_MAXIMUM; index++)
	{
		if (bots_globals.bots[index].used)
			count++;
	}
	return count;
}

boolean bots_add(
	short difficulty)
{
	struct network_game_server *server = global_network_game_server_get();
	struct network_game *game = server ? network_game_server_get_game(server) : NULL;
	struct network_player added;
	wchar_t name[BOT_NAME_LENGTH];
	char text[BOT_NAME_LENGTH];
	short index;

	if (!game)
		return FALSE;
	for (index = 0; index < BOTS_MAXIMUM && bots_globals.bots[index].used; index++)
		;
	if (index == BOTS_MAXIMUM)
		return FALSE;
	bots_new_name(game, name, NUMBEROF(name));
	if (!network_game_server_port_add_bot(name, &added))
		return FALSE;
	csmemset(&bots_globals.bots[index], 0, sizeof(bots_globals.bots[index]));
	bots_globals.bots[index].used = TRUE;
	bots_globals.bots[index].machine_index = added.machine_index;
	bots_globals.bots[index].controller_index = added.controller_index;
	bots_globals.bots[index].difficulty = PIN(difficulty, 0, NUMBER_OF_BOT_DIFFICULTIES - 1);
	bots_globals.bots[index].slot = index;
	bot_brain_reset(&bots_globals.bots[index].brain,
		(unsigned long)config_integer("debug.bot_seed") * 7919UL + ++bots_globals.added_count);
	bots_name_text(added.name, text, (int)sizeof(text));
	platform_log("bots: %s joined (machine %d, controller %d, team %d)", text, (int)added.machine_index,
		(int)added.controller_index, (int)added.team_index);
	return TRUE;
}

boolean bots_remove(
	long machine_index,
	long controller_index)
{
	short index;

	for (index = 0; index < BOTS_MAXIMUM; index++)
	{
		struct bot *bot = &bots_globals.bots[index];

		if (bot->used && bot->machine_index == machine_index && bot->controller_index == controller_index)
		{
			if (!network_game_server_port_remove_bot(machine_index, controller_index))
				return FALSE;
			bot->used = FALSE;
			/* (one fewer from now on: the host asked for it) */
			if (bots_globals.wanted > 0)
				bots_globals.wanted--;
			platform_log("bots: the bot of machine %ld, controller %ld left", machine_index, controller_index);
			return TRUE;
		}
	}
	return FALSE;
}

boolean bots_player_is_bot(
	struct player_datum const *player)
{
	return player && global_network_game_server_get() &&
		network_game_server_port_machine_is_bot(player->network_player_data.machine_index);
}

/* every BOTS_REPORT_TICKS of a game, what each bot has done (the tests
read it) */
static void bots_report(
	void)
{
	struct network_game_server *server = global_network_game_server_get();
	long now;
	short index;

	if (!game_in_progress() || !server)
		return;
	now = game_time_get();
	if (now < bots_globals.reported_tick)
		bots_globals.reported_tick = now;
	if (now - bots_globals.reported_tick < BOTS_REPORT_TICKS)
		return;
	bots_globals.reported_tick = now;
	for (index = 0; index < BOTS_MAXIMUM; index++)
	{
		struct bot *bot = &bots_globals.bots[index];
		struct network_player const *player = bot->used ?
			network_game_server_port_bot_player(bot->machine_index, bot->controller_index) : NULL;
		struct bot_statistics const *statistics = &bot->brain.statistics;
		char name[BOT_NAME_LENGTH];

		if (!player)
			continue;
		bots_name_text(player->name, name, (int)sizeof(name));
		platform_log("bots: %s at tick %ld: %ld goals reached, %ld paths (%ld failed), stuck %ld times, went %.0f world units"
			" (%ld drops, %ld jumps, %ld teleporters, %ld ladders up, %ld down); %ld targets seen, fired %ld ticks, "
			"%ld weapons taken",
			name, now, statistics->goals_reached, statistics->paths_planned, statistics->paths_failed, statistics->stuck,
			statistics->travelled, statistics->moves[_bots_nav_drop], statistics->moves[_bots_nav_jump],
			statistics->moves[_bots_nav_teleport], statistics->moves[_bots_nav_climb], statistics->moves[_bots_nav_descend],
			statistics->targets_seen, statistics->ticks_fired, statistics->weapons_taken);
	}
}

/* the roster kept to the host's game: bots no longer in it forgotten (the
host gone, a new one begun), and as many as are wanted */
void bots_update(
	void)
{
	struct network_game_server *server = global_network_game_server_get();
	struct network_game *game = server ? network_game_server_get_game(server) : NULL;
	short count;
	short index;

	if (!game)
	{
		if (bots_globals.hosting)
			csmemset(&bots_globals, 0, sizeof(bots_globals));
		return;
	}
	if (!bots_globals.hosting)
	{
		bots_globals.hosting = TRUE;
		bots_globals.wanted = (short)PIN(config_integer("server.bots"), 0, BOTS_MAXIMUM);
	}
	for (index = 0; index < BOTS_MAXIMUM; index++)
	{
		struct bot *bot = &bots_globals.bots[index];

		if (bot->used && !network_game_server_port_bot_player(bot->machine_index, bot->controller_index))
			bot->used = FALSE;
	}
	/* (a co-op game, which has no game engine, has none) */
	if (!game->variant.game_engine_index)
		return;
	count = bots_count();
	/* (the walking graph of the map being played, once there are bots) */
	bots_nav_update(count > 0);
	bots_report();
	/* (one a frame) */
	if (count < bots_globals.wanted)
	{
		bots_add(bots_difficulty_setting());
	}
	else if (count > bots_globals.wanted)
	{
		for (index = BOTS_MAXIMUM - 1; index >= 0; index--)
		{
			struct bot *bot = &bots_globals.bots[index];

			if (bot->used)
			{
				short wanted = bots_globals.wanted;

				bots_remove(bot->machine_index, bot->controller_index);
				bots_globals.wanted = wanted;
				break;
			}
		}
	}
}

/* ---------- each tick */

/* the player datum of a bot in the game being played (NONE if none) */
static long bots_player_index(
	struct bot const *bot)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (!player->quit_out_of_game &&
			player->network_player_data.machine_index == bot->machine_index &&
			player->network_player_data.controller_index == bot->controller_index)
		{
			return iterator.datum_index;
		}
	}
	return NONE;
}

void bots_take_actions(
	void)
{
	short index;

	if (!global_network_game_server_get() || !game_in_progress() || !player_data || !player_data->valid)
		return;
	for (index = 0; index < BOTS_MAXIMUM; index++)
	{
		struct bot *bot = &bots_globals.bots[index];
		long player_index;
		struct player_datum *player;
		struct player_action action;

		if (!bot->used || (player_index = bots_player_index(bot)) == NONE)
			continue;
		player = player_get(player_index);
		if (player->unit_index == NONE)
		{
			/* (dead: still, until the game spawns it again) */
			bot->brain.unit_index = NONE;
			bot_action_idle(&action, bot->brain.yaw, 0.0f);
		}
		else
		{
			bot_brain_think(bot, player_index, player->unit_index, &action);
		}
		update_server_take_bot_action(player_index, &action);
	}
}

/* ---------- the host's bot command (hs.c) */

/* whether the text's first count characters are the same as the name's, in either case */
static boolean bots_same_text(
	char const *name,
	char const *text,
	size_t count)
{
	size_t index;

	for (index = 0; index < count; index++)
	{
		char a = name[index] >= 'A' && name[index] <= 'Z' ? name[index] - 'A' + 'a' : name[index];
		char b = text[index] >= 'A' && text[index] <= 'Z' ? text[index] - 'A' + 'a' : text[index];

		if (a != b)
			return FALSE;
		if (!a)
			return TRUE;
	}
	return TRUE;
}

/* the bot named (the whole name, the call-sign alone, or the start of
either, in either case), NULL (said) if none or several */
static struct bot *bots_named(
	char const *text)
{
	struct network_game_server *server = global_network_game_server_get();
	struct network_game *game = server ? network_game_server_get_game(server) : NULL;
	struct bot *found = NULL;
	short matches = 0;
	short index;

	for (index = 0; game && index < BOTS_MAXIMUM; index++)
	{
		struct bot *bot = &bots_globals.bots[index];
		struct network_player const *player = bot->used ?
			network_game_server_port_bot_player(bot->machine_index, bot->controller_index) : NULL;
		char name[BOT_NAME_LENGTH];
		char const *candidates[2];
		short candidate;

		if (!player)
			continue;
		bots_name_text(player->name, name, (int)sizeof(name));
		candidates[0] = name;
		candidates[1] = name + (strncmp(name, "[BOT] ", 6) ? 0 : 6);
		for (candidate = 0; candidate < 2; candidate++)
		{
			if (bots_same_text(candidates[candidate], text, strlen(text)))
			{
				if (strlen(candidates[candidate]) == strlen(text))
				{
					/* (the whole name: this one) */
					return bot;
				}
				if (found != bot)
					matches++;
				found = bot;
				break;
			}
		}
	}
	if (matches == 1)
		return found;
	console_warning(matches ? "bot: %d bots' names begin with \"%s\"" : "bot: no bot is named \"%s\"",
		matches ? matches : (int)strlen(text), text);
	return NULL;
}

static void bots_list(
	void)
{
	short index;
	short count = 0;

	for (index = 0; index < BOTS_MAXIMUM; index++)
	{
		struct bot *bot = &bots_globals.bots[index];
		struct network_player const *player = bot->used ?
			network_game_server_port_bot_player(bot->machine_index, bot->controller_index) : NULL;
		char name[BOT_NAME_LENGTH];

		if (!player)
			continue;
		bots_name_text(player->name, name, (int)sizeof(name));
		console_printf(FALSE, "%s (team %d)", name, (int)player->team_index);
		platform_log("bots: listed %s (team %d)", name, (int)player->team_index);
		count++;
	}
	console_printf(FALSE, "%d bots (server.bots asks for %d)", (int)count, (int)bots_globals.wanted);
}

boolean bots_console_command(
	char const *text)
{
	char word[16];
	char argument[64];
	long length;

	if (!global_network_game_server_get())
	{
		console_warning("bot: only the host of a game has bots");
		return FALSE;
	}
	while (*text == ' ' || *text == '\t')
		text++;
	for (length = 0; *text && *text != ' ' && *text != '\t' && length < (long)sizeof(word) - 1; length++)
		word[length] = *text++;
	word[length] = 0;
	while (*text == ' ' || *text == '\t' || *text == '"')
		text++;
	for (length = 0; *text && *text != '"' && length < (long)sizeof(argument) - 1; length++)
		argument[length] = *text++;
	while (length > 0 && (argument[length - 1] == ' ' || argument[length - 1] == '\t'))
		length--;
	argument[length] = 0;

	if (!strcmp(word, "add"))
	{
		long count = argument[0] ? atol(argument) : 1;
		long added = 0;

		while (added < count && bots_count() < BOTS_MAXIMUM && bots_add(bots_difficulty_setting()))
		{
			added++;
			bots_globals.wanted = MAX(bots_globals.wanted, bots_count());
		}
		if (added < count)
			console_warning("bot add: %ld of %ld added (the game is full, or between games, or has %d bots)",
				added, count, BOTS_MAXIMUM);
		return added > 0;
	}
	if (!strcmp(word, "remove"))
	{
		if (bots_same_text(argument, "all", 4))
		{
			short index;
			boolean removed = TRUE;

			for (index = 0; index < BOTS_MAXIMUM; index++)
			{
				struct bot *bot = &bots_globals.bots[index];

				if (bot->used)
					removed &= bots_remove(bot->machine_index, bot->controller_index);
			}
			bots_globals.wanted = bots_count();
			return removed;
		}
		else
		{
			struct bot *bot = argument[0] ? bots_named(argument) : NULL;

			if (!argument[0])
				console_warning("bot remove: give a bot's name, or all");
			return bot && bots_remove(bot->machine_index, bot->controller_index);
		}
	}
	if (!strcmp(word, "list") || !word[0])
	{
		bots_list();
		return TRUE;
	}
	console_warning("bot: \"bot add [count]\", \"bot remove <name|all>\" or \"bot list\"");
	return FALSE;
}
