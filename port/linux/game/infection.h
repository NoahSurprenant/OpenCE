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

/* game_engine.c: a team's change colour (the infected green) */
real_rgb_color const *infection_change_color(long team_index);

/* hud.c: the local player's side and the survivors left, at the top */
void infection_draw(short local_player_index);

#endif
