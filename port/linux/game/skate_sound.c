/* skate_sound.c: the skater's sounds (skate_sound.h, port/skate/README.md,
"Sound")

The skate engine makes the sounds itself (port/skate/halo-skate: sound.rs
turns its ticks into events and loops, mixer.rs plays them, and dsound_sdl.c
mixes them into the game's output). The game tells it, each tick, where they
are heard from (the camera), what the board is on (the material of the level
under it) and how loud they are: audio.effects_volume times skate_volume
(config.toml's audio.skate_volume, or the console's for the session), all
under audio.volume.

Halo's own footsteps never play on the board: they come from biped_update
(bipeds.c: biped_try_to_make_footsteps, the landings' and jumps' material
effects, and unit_update_animation, which plays the animations' sound
frames), which a skating biped skips. */

#include "cseries.h"
#include "math/real_math.h"
#include "camera/observer.h"
#include "main/console.h"
#include "physics/collisions.h"

#include "skate_sound.h"

#include <math.h>
#include <string.h>

#ifdef HALO_SKATE

#include "../../skate/include/halo_skate_sound.h"

/* config.toml (port_config.c), and how often it changed */
double config_real(const char *name);
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

void skate_sound_update(boolean skating, long unit_index, float const *position)
{
	struct observer_result const *camera;
	float camera_position[3], forward[3], up[3];

	if (!skating || !position)
	{
		if (skate_sound.playing)
			halo_skate_sound_stop();
		skate_sound.playing = FALSE;
		return;
	}
	skate_sound.playing = TRUE;
	if (skate_sound.config_read_at != config_changes())
	{
		skate_sound.config_read_at = config_changes();
		skate_sound_send_volume();
	}
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
	halo_skate_sound_listener(camera_position, forward, up, skate_sound_surface(unit_index, position));
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

#endif
