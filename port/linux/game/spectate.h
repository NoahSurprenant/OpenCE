/* spectate.h: watching a network game without playing in it (spectate.c)

A spectator's machine joins a game as any other does, but brings no player
to it: it takes no player slot, never spawns, is on no team and on nobody's
scoreboard. The host already sends a machine with no player in the world
(one that is dead or joining) every player's unit and what is near them,
so the spectator sees the game with no change to the wire: hosts and
clients that know nothing of spectators play with one in the game.

The spectator's view is the first local player's view (director.c,
observer.c), which nobody else uses on that machine: a free camera that
flies through walls, or a camera that follows a player from behind or
looks through their eyes. */

#ifndef __SPECTATE_H
#define __SPECTATE_H

struct observer_command;

/* the spectator's cameras */
enum
{
	_spectate_mode_free,
	_spectate_mode_follow,
	_spectate_mode_first_person,
	NUMBER_OF_SPECTATE_MODES
};

/* whether this machine joins games as a spectator (network.spectate, or
the console's "spectate on"): it asks for no player */
boolean spectate_enabled(
	void);
/* whether this machine watches the game being played now: a spectator
client in a game, with no player of its own */
boolean spectate_watching(
	void);
/* whether the local player's view (director.c, observer.c, sound) is the
spectator's: the first one's, while spectate_watching() */
boolean spectate_view(
	short local_player_index);
/* (main.c, render.c) whether the game window is drawn from the spectator's
view: spectate_view(0), but not while the game shows one screen for all
(the scores after a game) */
boolean spectate_window(
	void);
/* (render_objects.c) the unit the spectator looks through the eyes of,
whose body is not drawn (NONE: none) */
long spectate_first_person_unit(
	void);
/* (director.c, each frame while spectate_view(0)) the spectator's input
read and its camera moved: what the observer is to do */
void spectate_update(
	real seconds,
	struct observer_command *command);
/* (render.c) the spectator's own lines over the view: who is watched,
their health and shields, names over players, the scores */
void spectate_draw(
	void);
/* (hs.c) the console's "spectate" command: "on", "off", "next",
"previous", "free", "follow", "first", "player <name>", "speed <n>",
"scores" (shown until asked again); with
nothing, what it watches */
boolean spectate_console_command(
	char const *text);
/* (network_test.c, each second) the spectator's view for the log: its
mode and the player it watches (NONE: none) */
void spectate_status(
	short *mode,
	long *player_index);

#endif
