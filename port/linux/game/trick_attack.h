/* trick_attack.h: the Trick Attack game type (trick_attack.c;
port/linux/README.md's "Trick Attack"): Slayer (free for all) with
game_engine_variant.slayer.trick_attack set. Everyone is always on a
skateboard, unarmed (Infection's rules for the infected, for all); the
score is the tricks each skater lands, as Skate 3's own scoring has them,
chained into combos that bank once landed clean; the highest score when the
round's time runs out wins. The host keeps the scores; a client's skater's
tricks come to it in the skate report message, and every machine's view of
the combos from it in the skate verdict message, as entries of their own
kind that older builds skip (no change of the network version). */

#ifndef HALO_PORT_TRICK_ATTACK_H
#define HALO_PORT_TRICK_ATTACK_H

/* the variant's defaults (game_engine.h's slayer_variant: 0 is these) */
#define TRICK_ATTACK_DEFAULT_MINUTES 5
/* tenths of a second */
#define TRICK_ATTACK_DEFAULT_BANK_DELAY 20
/* percent */
#define TRICK_ATTACK_DEFAULT_STEAL 50
/* tenths */
#define TRICK_ATTACK_DEFAULT_HOT_SPOT_MULTIPLIER 20
/* seconds */
#define TRICK_ATTACK_DEFAULT_HOT_SPOT_SECONDS 60

/* a combo's multiplier: one more for each sequence chained, to this */
#define TRICK_ATTACK_MAXIMUM_MULTIPLIER 10
/* unbanked points from which a skater is marked on everyone's HUD (and a
bot that hunts goes for them), and a banked combo is called out to everyone
(debug.trick_mark_points: the tests' own) */
#define TRICK_ATTACK_MARK_POINTS 5000
long trick_attack_mark_points(void);
/* the hot spot's reach (world units) */
#define TRICK_ATTACK_HOT_SPOT_RADIUS 5.0f

/* the entries' kind (their first byte) in the skate report and verdict
messages (skate_authority.c), which builds without Trick Attack skip as of
another format */
#define TRICK_ATTACK_ENTRY_FORMAT 0x54

struct game_variant;
/* the variant's Trick Attack settings left 0 made the game type's own */
void trick_attack_variant_defaults(struct game_variant *variant);

/* the game being played is Trick Attack */
boolean trick_attack_active(void);
/* the unit is a living player's biped in Trick Attack: always on a board;
it may not fire, melee, throw, act, swap, reload or use equipment
(infection.c's infection_unit_restricted takes these too) */
boolean trick_attack_unit_restricted(long unit_index);

/* game_engine.c's hooks: the game starting, each tick (every machine: the
host scores, a client keeps the host's word), the game over (the host logs
the scores); the round's time limit (ticks), the shorter of the variant's
minutes and the game's time limit (minutes, 0: none) */
void trick_attack_game_starting(void);
void trick_attack_update(void);
void trick_attack_game_over(void);
long trick_attack_time_limit_ticks(long time_limit_minutes);

/* skate_authority.c, the host deciding a skater's hit on another skater
(absolute player indices): ramming one mid-combo knocks them off their
board and takes the variant's share of the combo (the rammer bails not);
the bails are changed so */
void trick_attack_ram(long rammer_absolute, long target_absolute, real closing, boolean *rammer_bails,
	boolean *target_bails);
/* skate_authority.c: a run-over's share of the damage in Trick Attack (none:
boards knock people over, never kill), else share */
real trick_attack_run_over_share(real share);

/* the wire (skate_authority.c): (a client) its skater's tricks for the host
into out (capacity entries of SKATE_REPORT_SIZE): how many; (the host) a
report entry of a client's machine; (the host) the combos and the hot spot
for every client into out (capacity entries of SKATE_VERDICT_SIZE): how
many; (a client) such an entry */
short trick_attack_reports(byte *out, short capacity);
void trick_attack_handle_report(long machine_index, byte const *entry);
short trick_attack_entries(byte *out, short capacity);
void trick_attack_handle_entry(byte const *entry);

/* what every machine knows (the host's word): the player's (a datum)
unbanked combo, its multiplier (0: no combo going) */
long trick_attack_unbanked(long player_index);
long trick_attack_multiplier(long player_index);
/* the player (a datum) with the biggest unbanked combo of at least
least_points, but except (NONE for none) */
long trick_attack_combo_leader(long least_points, long except);
/* the hot spot: where it is, its seconds left and its multiplier; FALSE
for none (yet) */
boolean trick_attack_hot_spot(real *x, real *y, real *z, long *seconds_left, real *multiplier);
/* the ticks a combo rides clean after its last landing before it banks */
long trick_attack_bank_ticks(void);

/* hud.c: the local player's combo, the leaders, the hot spot, marks over
big combos and the callouts */
void trick_attack_draw(short local_player_index);

#endif
