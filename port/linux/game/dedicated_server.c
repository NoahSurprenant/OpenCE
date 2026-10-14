/*
DEDICATED_SERVER.C

A dedicated server's own part of each frame (server.dedicated in
config.toml, HALO_DEDICATED; port/linux/README.md's "Dedicated server"):
network_test.c hosts its maps in turn, with no player of its own, and this
logs what happens on it for whoever reads its log - the players who join and
leave, each game's start, and its end with the scores - instead of every
second's units, as the tests log them.

And, for every run, quitting on SIGTERM or SIGINT (the platform layer's
platform_quit_requested, as is debug.exit_after without a window): a game
being played ends for its players, they are told the host is going, and the
game exits with 0, the server browser's listing withdrawn on the way out
(p2p.c's exit hook). Before, without a window, the signals went unheard.

Called from the main loop every frame (main.c).
*/

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "main/main.h"
#include "networking/network_game_globals.h"
#include "networking/network_server_manager.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "memory/data.h"

#include <xtl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src) */
int config_boolean(char const *name);
void platform_log(char const *format, ...);
int platform_quit_requested(void);
void platform_exit_deadline(int status, unsigned seconds);

/* the players the server's list is asked for at most */
#define DEDICATED_SERVER_MAXIMUM_PLAYERS 64
/* the server's state in a game (network_server_manager.c's enum
network_game_server_state) */
#define DEDICATED_SERVER_STATE_INGAME 1
/* the seconds a dedicated server has to finish going (its exit hooks: the
server browser's listing withdrawn, p2p.c), past which it is ended at once */
#define DEDICATED_SERVER_EXIT_SECONDS 5
/* the exit status of a dedicated server stopped by a fatal error */
#define DEDICATED_SERVER_EXIT_FATAL 1

static struct
{
	boolean checked;
	boolean dedicated;
	/* the players on the server when last looked, by name (its other
	machines': it has none of its own) */
	char names[DEDICATED_SERVER_MAXIMUM_PLAYERS][NETWORK_GAME_SERVER_NAME_TEXT_SIZE];
	short name_count;
	unsigned long looked_at;
	/* whether a game was being played, and was still being scored, when
	last looked */
	boolean playing;
	boolean scoring;
} dedicated_server;

/* a player's name in ASCII */
static void dedicated_server_player_name(
	wchar_t const *wide,
	char *name,
	short size)
{
	short index;

	for (index = 0; index < size - 1 && wide[index]; index++)
		name[index] = wide[index] >= 32 && wide[index] < 127 ? (char)wide[index] : '?';
	name[index] = 0;
}

/* whether a name is in a list of them (names taken from it as they are
found, so that two players of one name count twice) */
static boolean dedicated_server_take_name(
	char (*names)[NETWORK_GAME_SERVER_NAME_TEXT_SIZE],
	boolean *taken,
	short count,
	char const *name)
{
	short index;

	for (index = 0; index < count; index++)
	{
		if (!taken[index] && !strcmp(names[index], name))
		{
			taken[index] = TRUE;
			return TRUE;
		}
	}
	return FALSE;
}

/* the players who joined and left since the last look */
static void dedicated_server_log_players(
	void)
{
	char names[DEDICATED_SERVER_MAXIMUM_PLAYERS][NETWORK_GAME_SERVER_NAME_TEXT_SIZE];
	boolean taken_before[DEDICATED_SERVER_MAXIMUM_PLAYERS];
	boolean taken_now[DEDICATED_SERVER_MAXIMUM_PLAYERS];
	short count = 0;
	short index;

	if (global_network_game_server_get())
		count = network_game_server_matching_player_names("", names, DEDICATED_SERVER_MAXIMUM_PLAYERS);
	csmemset(taken_before, 0, sizeof(taken_before));
	csmemset(taken_now, 0, sizeof(taken_now));
	for (index = 0; index < count; index++)
	{
		if (!dedicated_server_take_name(dedicated_server.names, taken_before, dedicated_server.name_count, names[index]))
			platform_log("server: %s joined (%d players)", names[index], (int)count);
	}
	for (index = 0; index < dedicated_server.name_count; index++)
	{
		if (!dedicated_server_take_name(names, taken_now, count, dedicated_server.names[index]))
			platform_log("server: %s left (%d players)", dedicated_server.names[index], (int)count);
	}
	csmemcpy(dedicated_server.names, names, sizeof(names[0]) * count);
	dedicated_server.name_count = count;
}

/* each player's score, kills and deaths (and each team's, with teams) */
static void dedicated_server_log_scores(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	boolean teams = game_engine_get_variant()->universal_variant.teams;

	if (teams)
		platform_log("server: score: red %ld, blue %ld", game_engine_get_team_score(0), game_engine_get_team_score(1));
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		char name[13];

		dedicated_server_player_name(player->name, name, (short)sizeof(name));
		platform_log("server: score: %s%s %ld, %d kills, %d deaths", name,
			teams ? (player->team_index == 0 ? " (red)" : " (blue)") : "",
			game_engine && game_engine->get_player_score ?
				game_engine->get_player_score(iterator.datum_index, _get_score_individual) : 0L,
			(int)player->statistics.kills[0], (int)player->statistics.deaths);
	}
}

/* a game started, or ended */
static void dedicated_server_log_game(
	void)
{
	boolean playing = game_in_progress() && game_engine_running();
	boolean scoring = playing && game_engine_can_score();

	if (playing && !dedicated_server.playing)
	{
		char const *map = main_get_multiplayer_map_name();
		char const *slash = strrchr(map, '\\');

		/* (the map's name, not its path; the game engine's name: the built-in
		variants have no description) */
		platform_log("server: the game starts: %s%s on %s", game_engine && game_engine->name ? game_engine->name : "?",
			game_engine_get_variant()->universal_variant.teams ? " (teams)" : "", slash ? slash + 1 : map);
	}
	if (dedicated_server.scoring && !scoring && playing)
	{
		platform_log("server: the game is over");
		dedicated_server_log_scores();
	}
	dedicated_server.playing = playing;
	dedicated_server.scoring = scoring;
}

/* SIGTERM, SIGINT or debug.exit_after: the players told, and out */
static void dedicated_server_quit(
	int signal_number)
{
	struct network_game_server *server = global_network_game_server_get();

	/* (the rest of the way out given its own few seconds: a deadline the
	signal set may be nearly over, sdl_platform.c) */
	platform_exit_deadline(signal_number > 0 ? 128 + signal_number : EXIT_SUCCESS, DEDICATED_SERVER_EXIT_SECONDS);
	if (signal_number > 0)
		platform_log("quitting on signal %d", signal_number);
	else
		platform_log("exiting after debug.exit_after");
	if (server)
	{
		/* a game being played ends first: the server tells its machines the
		host is going only in the lobby, before or after one */
		if (network_game_server_get_state(server, NULL) == DEDICATED_SERVER_STATE_INGAME)
			network_game_server_switch_to_postgame(server);
		dispose_global_network_game_server();
		/* (the messages carried out: the internet tunnel's thread sends them
		on, p2p.c) */
		Sleep(500);
		platform_log("server: closed; the players were told");
	}
	else if (global_network_game_client_get())
	{
		dispose_global_network_game_client();
	}
	exit(EXIT_SUCCESS);
}

void dedicated_server_update(
	void)
{
	int quit = platform_quit_requested();

	if (quit)
		dedicated_server_quit(quit);
	if (!dedicated_server.checked)
	{
		dedicated_server.checked = TRUE;
		dedicated_server.dedicated = config_boolean("server.dedicated") ? TRUE : FALSE;
	}
	if (!dedicated_server.dedicated)
		return;
	dedicated_server_log_game();
	/* (the players looked at twice a second) */
	if (system_milliseconds() - dedicated_server.looked_at >= 500)
	{
		dedicated_server.looked_at = system_milliseconds();
		dedicated_server_log_players();
	}
}

/* a dedicated server the game cannot go on with (main.c: halt_and_catch_fire
after a fatal error, and main_loop_of_death, the damaged disc screen a
player is left on when a map cannot be read: display_error_damaged_media): the error's last lines on the
log, and out with status 1 - rather than drawing that screen for nobody,
forever, deaf to SIGTERM, as it did. A desktop game keeps its screen (this
returns at once). errors is the game's recent messages (error_get), lines
ending in "\r\n". */
void dedicated_server_fatal(
	char const *errors)
{
	static boolean exiting;
	char const *start;
	char const *end;
	short lines = 0;

	if (!config_boolean("server.dedicated"))
		return;
	/* (an error on the way out: out at once) */
	if (exiting)
		platform_exit_deadline(DEDICATED_SERVER_EXIT_FATAL, 0);
	exiting = TRUE;
	if (!errors)
		errors = "";
	/* (the last three lines) */
	end = errors + strlen(errors);
	while (end > errors && (end[-1] == '\r' || end[-1] == '\n'))
		end--;
	start = end;
	while (start > errors)
	{
		if (start[-1] == '\n' && ++lines == 3)
			break;
		start--;
	}
	{
		char text[512];
		short index = 0;

		for (; start < end && index < (short)sizeof(text) - 3; start++)
		{
			if (*start == '\r')
				continue;
			if (*start == '\n')
			{
				text[index++] = ';';
				text[index++] = ' ';
			}
			else
			{
				text[index++] = *start;
			}
		}
		text[index] = 0;
		platform_log("server: a fatal error (%s); exiting (debug.txt, in the save root or paths.logs, has the rest)",
			index ? text : "no message");
	}
	platform_exit_deadline(DEDICATED_SERVER_EXIT_FATAL, DEDICATED_SERVER_EXIT_SECONDS);
	exit(DEDICATED_SERVER_EXIT_FATAL);
}
