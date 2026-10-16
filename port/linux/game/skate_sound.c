/* skate_sound.c: the skater's sounds (skate_sound.h, port/skate/README.md,
"Sound")

The skate engine makes the sounds itself (port/skate/halo-skate: sound.rs
turns its ticks into events and loops, mixer.rs plays them, and dsound_sdl.c
mixes them into the game's output). The game tells it, each tick, where they
are heard from (the camera), what the board is on (the material of the level
under it) and how loud they are: audio.effects_volume times skate_volume
(config.toml's audio.skate_volume, or the console's for the session), all
under audio.volume.

In a network game each machine's engine makes its own skater's sounds alone;
skate_network.c sends them to the others (their one-shots and their loops'
state, port/skate/README.md "Multiplayer"), and plays other machines'
skaters' here, at their boards (halo-skate's sound_remote.rs), heard from the
same camera. So the camera is told of every tick, skating or not. A
dedicated server plays none.

Halo's own footsteps never play on the board: they come from biped_update
(bipeds.c: biped_try_to_make_footsteps, the landings' and jumps' material
effects, and unit_update_animation, which plays the animations' sound
frames), which a skating biped skips. */

#include "cseries.h"
#include "math/real_math.h"
#include "camera/observer.h"
#include "main/console.h"
#include "physics/collisions.h"

#include "skate.h"
#include "skate_sound.h"

#include <math.h>
#include <string.h>

#ifdef HALO_SKATE

#include "../../skate/include/halo_skate_sound.h"

/* config.toml (port_config.c), and how often it changed */
double config_real(const char *name);
int config_boolean(const char *name);
unsigned long config_changes(void);
void platform_log(const char *format, ...);

/* how far above the board the probe for the surface starts, and how far
down it looks (world units: 15 cm and 1.5 m) */
#define SKATE_SOUND_PROBE_ABOVE 0.05f
#define SKATE_SOUND_PROBE_DOWN 0.5f
#define SKATE_SOUND_MAXIMUM_VOLUME 4.f

static struct
{
	/* skate_volume: the console's for the session, else audio.skate_volume */
	real volume;
	boolean volume_set_at_console;
	boolean log;
	boolean playing;
	int surface;
	unsigned long config_read_at;
} skate_sound = { 1.f, FALSE, TRUE, FALSE, HALO_SKATE_SURFACE_CONCRETE, (unsigned long)-1 };

/* the sound set's surface for one of Halo's material types (game_globals.c's
global_material_type_strings) */
static int skate_sound_surface_from_material(short material_type)
{
	switch (material_type)
	{
	case 0: /* dirt */
	case 1: /* sand */
	case 3: /* snow */
	case 28: /* water */
	case 29: /* leaves */
		return HALO_SKATE_SURFACE_ROUGH;
	case 4: /* wood */
		return HALO_SKATE_SURFACE_WOOD;
	case 5: /* metal (hollow) */
	case 6: /* metal (thin) */
	case 7: /* metal (thick) */
	case 10: /* force field */
		return HALO_SKATE_SURFACE_METAL;
	default: /* stone, glass, rubber, plastic, ice, ... */
		return HALO_SKATE_SURFACE_CONCRETE;
	}
}

/* the surface under the board, else the last one found (in the air) */
static int skate_sound_surface(long unit_index, float const *position)
{
	struct collision_result collision;
	real_point3d start;
	real_vector3d down;

	start.x = position[0];
	start.y = position[1];
	start.z = position[2] + SKATE_SOUND_PROBE_ABOVE;
	down.i = 0.f;
	down.j = 0.f;
	down.k = -SKATE_SOUND_PROBE_DOWN;
	if (collision_test_vector(_collision_test_environment_flags, &start, &down, unit_index, &collision) &&
		collision.material_type != NONE)
	{
		skate_sound.surface = skate_sound_surface_from_material(collision.material_type);
	}
	return skate_sound.surface;
}

static void skate_sound_send_volume(void)
{
	real effects = (real)config_real("audio.effects_volume");

	if (!skate_sound.volume_set_at_console)
		skate_sound.volume = (real)config_real("audio.skate_volume");
	if (!(effects >= 0.f))
		effects = 0.f;
	if (effects > 1.f)
		effects = 1.f;
	if (!(skate_sound.volume >= 0.f && skate_sound.volume <= SKATE_SOUND_MAXIMUM_VOLUME))
		skate_sound.volume = 1.f;
	halo_skate_sound_set_volume(effects * skate_sound.volume);
}

void skate_sound_initialize(char const *assets)
{
	skate_sound_send_volume();
	skate_sound.config_read_at = config_changes();
	halo_skate_sound_set_log(skate_sound.log);
	halo_skate_sound_load(assets);
}

/* a dedicated server (server.dedicated) plays no sound */
static boolean skate_sound_dedicated(void)
{
	static int dedicated = -1;

	if (dedicated < 0)
		dedicated = config_boolean("server.dedicated") ? 1 : 0;
	return dedicated != 0;
}

void skate_sound_update(boolean skating, long unit_index, float const *position)
{
	struct observer_result const *camera;
	float camera_position[3], forward[3], up[3];

	if (skate_sound_dedicated())
		return;
	if (skate_sound.config_read_at != config_changes())
	{
		skate_sound.config_read_at = config_changes();
		skate_sound_send_volume();
	}
	if (!skating || !position)
	{
		if (skate_sound.playing)
			halo_skate_sound_stop();
		skate_sound.playing = FALSE;
	}
	else
	{
		skate_sound.playing = TRUE;
		skate_sound_surface(unit_index, position);
	}
	/* (every skater's sounds are heard from the camera: this machine's, and
	other machines' skaters' while this one is on foot) */
	camera = observer_get_camera(0);
	if (!camera)
		return;
	camera_position[0] = camera->position.x;
	camera_position[1] = camera->position.y;
	camera_position[2] = camera->position.z;
	forward[0] = camera->forward.i;
	forward[1] = camera->forward.j;
	forward[2] = camera->forward.k;
	up[0] = camera->up.i;
	up[1] = camera->up.j;
	up[2] = camera->up.k;
	halo_skate_sound_listener(camera_position, forward, up, skate_sound.surface);
}

short skate_sound_take_local(struct skate_sound_event *events, short capacity, struct skate_sound_loops *loops)
{
	struct halo_skate_sound_event taken[2 * SKATE_SOUND_EVENTS];
	struct halo_skate_sound_loops state;
	short count;
	short index;

	if (!events || capacity <= 0)
	{
		halo_skate_sound_take(NULL, 0, NULL);
		return 0;
	}
	if (capacity > (short)NUMBEROF(taken))
		capacity = (short)NUMBEROF(taken);
	memset(&state, 0, sizeof(state));
	count = (short)halo_skate_sound_take(taken, capacity, &state);
	if (loops)
	{
		loops->wheels = (short)state.wheels;
		loops->grind = (short)state.grind;
		loops->powerslide = state.powerslide != 0;
		loops->surface = (short)skate_sound.surface;
		loops->speed = state.speed;
	}
	for (index = 0; index < count; index++)
	{
		events[index].sound = (short)taken[index].sound;
		events[index].strength = taken[index].strength;
		events[index].speed = taken[index].speed;
	}
	return count;
}

void skate_sound_remote_loops(short player_index, struct skate_sound_loops const *loops, real const *position)
{
	struct halo_skate_sound_loops state;
	float at[3];

	if (skate_sound_dedicated())
		return;
	state.wheels = loops->wheels;
	state.speed = loops->speed;
	state.grind = loops->grind;
	state.powerslide = loops->powerslide ? 1 : 0;
	at[0] = position[0];
	at[1] = position[1];
	at[2] = position[2];
	halo_skate_sound_remote_loops(player_index, &state, at, loops->surface);
}

boolean skate_sound_remote_event(short player_index, struct skate_sound_event const *event, short surface,
	real const *position)
{
	struct halo_skate_sound_event played;
	float at[3];

	if (skate_sound_dedicated())
		return FALSE;
	played.sound = event->sound;
	played.strength = event->strength;
	played.speed = event->speed;
	at[0] = position[0];
	at[1] = position[1];
	at[2] = position[2];
	return halo_skate_sound_remote_event(player_index, &played, at, surface) != 0;
}

void skate_sound_remote_stop(short player_index)
{
	if (!skate_sound_dedicated())
		halo_skate_sound_remote_stop(player_index);
}

int skate_sound_console_setting(char const *word, real value, boolean valid, boolean given)
{
	if (!strcmp(word, "skate_volume"))
	{
		if (given && !(valid && value >= 0.f && value <= SKATE_SOUND_MAXIMUM_VOLUME))
		{
			console_printf(FALSE, "skate_volume: a factor from 0 (silent) to %.0f (skate_volume 0.5)",
				SKATE_SOUND_MAXIMUM_VOLUME);
			return FALSE;
		}
		if (given)
		{
			skate_sound.volume = value;
			skate_sound.volume_set_at_console = TRUE;
		}
		skate_sound_send_volume();
		console_printf(FALSE, "skate_volume %.2f (default 1, audio.skate_volume %.2f; of audio.effects_volume %.2f)",
			skate_sound.volume, (real)config_real("audio.skate_volume"), (real)config_real("audio.effects_volume"));
		return TRUE;
	}
	if (!strcmp(word, "skate_sound_log"))
	{
		if (given && !(valid && (value == 0.f || value == 1.f)))
		{
			console_printf(FALSE, "skate_sound_log: 1 tells of each sound in the log, 0 does not");
			return FALSE;
		}
		if (given)
		{
			skate_sound.log = value != 0.f;
			halo_skate_sound_set_log(skate_sound.log);
		}
		console_printf(FALSE, "skate_sound_log %d (default 1: each pop, landing, grind and bail in the log)",
			skate_sound.log ? 1 : 0);
		return TRUE;
	}
	return -1;
}

#else

void skate_sound_initialize(char const *assets)
{
}

void skate_sound_update(boolean skating, long unit_index, float const *position)
{
}

int skate_sound_console_setting(char const *word, real value, boolean valid, boolean given)
{
	return -1;
}

short skate_sound_take_local(struct skate_sound_event *events, short capacity, struct skate_sound_loops *loops)
{
	return 0;
}

void skate_sound_remote_loops(short player_index, struct skate_sound_loops const *loops, real const *position)
{
}

boolean skate_sound_remote_event(short player_index, struct skate_sound_event const *event, short surface,
	real const *position)
{
	return FALSE;
}

void skate_sound_remote_stop(short player_index)
{
}

#endif
