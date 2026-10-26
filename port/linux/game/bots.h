/* bots.h: players the host plays itself (bots.c)

A bot is an ordinary network player whom the host gives input every tick,
on a machine slot of its own (network_server_manager.c's bot machines).
Every machine sees it as it sees any other machine's player, so builds that
know nothing of bots play with them. Only the host has bots: a client runs
none of this. */

#ifndef __BOTS_H
#define __BOTS_H

struct player_datum;

/* the most bots the bot array holds (four to a bot machine): a full lobby.
Only server.bots_fill lets the setting reach it - a bot leaves at once for
a person who joins a full game (bots_make_room); without it the setting
stays at BOTS_MAXIMUM - 1, so a person can always join (bots.c's
bots_maximum) */
#define BOTS_MAXIMUM HALO_PORT_MAXIMUM_NETWORK_PLAYERS
/* the tag every bot's name starts with, so that people know who is one
(nothing on the wire says so) */
#define BOTS_NAME_TAG L"[BOT] "

/* the bots' skill (server.bot_difficulty); peaceful bots never fight */
enum
{
	_bot_difficulty_peaceful,
	_bot_difficulty_easy,
	_bot_difficulty_normal,
	_bot_difficulty_hard,
	_bot_difficulty_legendary,
	NUMBER_OF_BOT_DIFFICULTIES
};

/* (player_queues_new.c's update_server_next_update, the host's every tick)
each bot's input for the tick */
void bots_take_actions(
	void);
/* (main.c, every frame) the bots the settings ask for, added and removed */
/* whether a game this machine hosts may start its clock as far as its bots
go: none, or their graphs of its map done (bots_nav_game.c builds them aside
once the map is loaded; network_server_manager.c waits for them as for the
machines still loading) */
boolean bots_graphs_ready(void);
void bots_update(
	void);
/* a bot added to the host's lobby or game; FALSE (said) if it cannot be */
boolean bots_add(
	short difficulty);
/* the bot of the bot machine and controller removed from the host's lobby
or game (network_server_manager.c's kick and ban of one); FALSE if it
cannot be now */
boolean bots_remove(
	long machine_index,
	long controller_index);
/* (hs.c) the host's bot command: "add [count] [difficulty]", "remove
<name|all>", "difficulty <level> [name]", "list" */
boolean bots_console_command(
	char const *text);
/* (network_server_manager.c) a person joins a game full of players and
bots: a bot leaves at once */
void bots_make_room(
	void);
/* (damage.c, the host's) an object hurt by the damage, killed or not: a
bot's grenade's hit on an enemy, its grenade's kill (an enemy, itself, a
team mate), or its team kill, for its statistics */
struct damage_data;
void bots_note_damage(
	long object_index,
	struct damage_data const *damage,
	real amount,
	boolean killed);
/* (infection.c) whether a skateboard reaches the point (world units) from
the spawns' main part along the map's walking graph (bots_skate.c: rolling
up a slope no steeper than a riding bot takes, down any, off any drop; no
jumps, ladders or teleporters): 1 it does, 0 it does not, NONE unknown (no
graph yet - bots_nav_game.c builds it aside - or no node there). The graph
node found is written to node_x/y/z when not NULL */
long bots_board_reaches(
	real x,
	real y,
	real z,
	real *node_x,
	real *node_y,
	real *node_z);
/* (infection.c) the map's walking graph is wanted even with no bots (a
host's, bots_update: built aside as for bots); a client builds its own here
(it has no bots_update): call each tick while wanted */
void bots_graph_wanted(
	boolean wanted);
/* whether the player is one of the host's bots */
boolean bots_player_is_bot(
	struct player_datum const *player);

#endif
