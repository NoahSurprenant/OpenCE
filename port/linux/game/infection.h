/* infection.h: the Infection game type (infection.c; port/linux/README.md's
"Infection"): Team Slayer with game_engine_variant.slayer.infection set. The
survivors (red, team 0) have their weapons; the infected (blue, team 1, drawn
green) are always on a skateboard, unarmed, and infect a survivor by running
them over. A survivor who dies for any reason comes back infected. The
survivors win if any live when the round's time runs out; the infected win
once everyone is infected. Who is infected is the player's team, which every
machine has from the host (a unit's owner_team_index: no wire change). */

#ifndef HALO_PORT_INFECTION_H
#define HALO_PORT_INFECTION_H

/* the variant's run-over damage of an infected skater (percent of any
skater's; tuned with the bots: tools/test_infection.py) */
#define INFECTION_DEFAULT_RUN_OVER 300
/* ... and the damage the infected take (percent of anyone's) */
#define INFECTION_DEFAULT_DAMAGE_TAKEN 35
/* ... and how much wider a survivor is to an infected's board (hundredths of
a world unit): none, the board has to hit them */
#define INFECTION_DEFAULT_REACH 0
/* ... and their boards' speed (percent of any skater's) */
#define INFECTION_DEFAULT_SPEED 140
/* ... and how sharply their boards turn (percent of any skater's) */
#define INFECTION_DEFAULT_TURNING 175
/* ... and the seconds a survivor may stay where no board reaches before it
hurts them (INFECTION_OUT_OF_REACH_NEVER: never) */
#define INFECTION_DEFAULT_OUT_OF_REACH 10
#define INFECTION_OUT_OF_REACH_NEVER 255


struct game_variant;
/* the variant's Infection settings left 0 (a Slayer variant, a gametype
saved before them) made the game type's own; the round and run-over's 0 were
already read so */
void infection_variant_defaults(struct game_variant *variant);

/* the teams */


#define INFECTION_TEAM_SURVIVORS 0
#define INFECTION_TEAM_INFECTED 1

/* the game being played is Infection */
boolean infection_active(void);
/* the player (a datum) is infected: Infection, on the infected team */
boolean infection_player_infected(long player_index);
/* the unit is an infected player's, alive: always on a board; it may not
fire, melee, throw, act (pick up, board), swap, reload or use equipment */
boolean infection_unit_restricted(long unit_index);
/* players.c, before the player's action reaches their unit: what the
infected may not do taken out of control_flags */
void infection_filter_action(long player_index, long unit_index, unsigned long *control_flags);

/* game_engine.c's hooks: the game starting (the host picks the first
infected), a player added (one joining later starts infected), a player
killed (the host infects a dead survivor), each tick (every machine: the
survival times; the host: the outcome, a new infected when the last left),
the game over (the host logs the winner) */
void infection_game_starting(void);
void infection_player_added(long player_index);
void infection_player_killed(long dead_player_index, long killing_player_index);
void infection_update(void);
void infection_game_over(void);
/* the round's time limit (ticks), the shorter of the variant's minutes and the
game's time limit (minutes, 0: none) */
long infection_time_limit_ticks(long time_limit_minutes);

/* game_engine_slayer.c: the scores. The team scores are the outcome (1 for the
side winning now); a survivor's own score is their kills, an infected's the
survivors they infected */
long infection_survivors(void);
long infection_infected(void);
/* the player's seconds as a survivor (to now, or their infection) */
long infection_survival_seconds(long player_index);

/* skate_authority.c: the share of a vehicle's run-over damage a skater's hit
deals, scaled for an infected skater (the variant's infection_run_over) */
real infection_run_over_share(long skater_unit_index, real share);
/* game_engine_get_damage_multiplier: the scale of the damage the player (a
datum, NONE: none) takes - the variant's infection_damage_taken for the
infected, else 1 */
real infection_damage_taken_scale(long damaged_player_index);
/* skate.c: how much wider (world units) a survivor's biped is to an infected
skater's board, the variant's infection_reach (0 for any other) */
real infection_board_reach(long skater_unit_index, long biped_index);
/* skate.c: how much faster than the game the unit's skate session steps -
the variant's infection_speed for an infected skater (their board is that
much quicker, pushing and at speed), else 1 */
real infection_skate_time_scale(long unit_index);
/* skate.c: how much more sharply the unit's board turns - the variant's
infection_turning for an infected skater, else 1 */
real infection_skate_turn_scale(long unit_index);

/* out of reach (the anti-camping rule): a survivor standing where no board
reaches (bots_board_reaches: a roof, a tower top) for the variant's
infection_out_of_reach seconds is hurt each second until they come down.
The host decides and deals it; every machine works the same out for its
own players to warn them (infection_draw). Unknown (the map's graph still
being built, no node there) holds the count. The ticks the player (a datum)
has been out of reach, 0 for none (bots_skate.c: a survivor bot comes
down) */
long infection_out_of_reach_ticks(long player_index);

/* game_engine.c: a team's change colour (the infected green) */
real_rgb_color const *infection_change_color(long team_index);

/* hud.c: the local player's side and the survivors left, at the top */
void infection_draw(short local_player_index);

#endif
