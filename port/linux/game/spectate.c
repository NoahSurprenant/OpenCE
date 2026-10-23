/*
SPECTATE.C

Watching a network game without playing in it (spectate.h). The machine
joins as any other but asks for no player (network_client_manager.c), so the
host gives it no slot, no team and no line on the scoreboard; the host sends
a machine with no player in the world every player's unit and what is near
them (network_objects.c), which is all a spectator needs.

The view is the first local player's, which nobody else uses on this
machine: director.c asks here for the observer's command each frame,
observer.c moves the camera, the sound is heard from it (sound_manager.c),
and the frame is drawn from it with no player's HUD (main.c, render.c) but
the lines drawn here.

Four cameras, the action button (E, X) switching between them
(network.spectate_camera the first):
- FREE flies where it looks, through walls: the movement keys or left
  stick, jump and crouch up and down, the weapon and grenade switches (the
  mouse wheel) faster and slower;
- FOLLOW stays behind the player watched, turning with them once the look
  is let go; forward and back bring it nearer and further;
- FIRST PERSON looks through their eyes;
- DIRECTOR follows whoever is most worth watching, cutting between them as
  a broadcast does (spectate_director.c).
Fire (the left mouse button, the right trigger) watches the next player, the
grenade (the right mouse button, the left trigger) the one before; the
scoreboard key (Tab, Back) shows the scores; the flashlight hides the names.
*/

#include "cseries.h"
#include "camera/first_person_camera.h"
#include "camera/observer.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "main/console.h"
#include "game/players.h"
#include "input/input.h"
#include "interface/hud_messaging.h"
#include "memory/data.h"
#include "networking/network_game_manager.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "render/render_cameras_internal.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"

#include "spectate.h"
#include "spectate_director.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

/* the game's controls (input_abstraction.c's enum), as the spectator reads them */
enum
{
	_control_jump,
	_control_switch_grenades,
	_control_action,
	_control_switch_weapons,
	_control_melee,
	_control_flashlight,
	_control_grenade,
	_control_primary_trigger,
	_control_start,
	_control_back,
	_control_crouch,
	_control_zoom,
	NUMBER_OF_CONTROLS
};

/* the free camera's speed in world units a second: at first, and the least
and most the switches take it to (a player runs about 2.3, a Warthog 9) */
#define FREE_SPEED 6.0f
#define FREE_SPEED_MINIMUM 0.75f
#define FREE_SPEED_MAXIMUM 48.0f
/* how fast the sticks turn the camera (radians a second at full tilt) */
#define STICK_TURN_RATE 3.0f
/* the follow camera's distance behind the player: at first, and nearest and
furthest; and its pitch (radians) */
#define FOLLOW_DISTANCE 3.5f
#define FOLLOW_DISTANCE_MINIMUM 1.25f
#define FOLLOW_DISTANCE_MAXIMUM 12.0f
#define FOLLOW_PITCH -0.3f
/* the director's distance behind the player it watches */
#define DIRECTOR_DISTANCE 4.5f
/* seconds after the look is let go that the follow camera turns back behind
the player, and how much of the turn it makes each second */
#define FOLLOW_RETURN_SECONDS 2.0f
#define FOLLOW_RETURN_RATE 3.0f
/* how high over a unit's origin the follow camera looks (its chest) */
#define FOLLOW_HEIGHT 0.45f
/* the field of view (dead_camera.c's) */
#define FIELD_OF_VIEW DEGREES_TO_RADIANS(70.0f)
/* the observer's glide from one player to the next (seconds) */
#define SWITCH_SECONDS 0.35f
/* names drawn over players no further than this (world units) */
#define NAME_DISTANCE 40.0f

/* ---------- globals */

static struct
{
	/* the console's "spectate on" or "off" (the setting otherwise) */
	boolean console_set;
	boolean console_enabled;
	short mode;
	/* the player watched (NONE: none) */
	long player_index;
	/* the free camera */
	real_point3d position;
	real yaw;
	real pitch;
	real speed;
	/* the follow camera: its turn about the player, and its distance */
	real follow_yaw;
	real follow_pitch;
	real follow_distance;
	real look_idle_seconds;
	/* where it last looked (the focus held while the player is dead) */
	real_point3d focus;
	boolean focus_valid;
	/* a new map: the game time last seen, and whether the camera is placed */
	long last_game_time;
	boolean placed;
	real switch_seconds;
	boolean names_hidden;
	/* the console's "spectate scores": the scores shown until asked again */
	boolean scores_shown;
	/* whether the first camera (network.spectate_camera) was taken */
	boolean mode_set;
	/* the controls held last frame (a press is one frame's) */
	boolean held[NUMBER_OF_CONTROLS];
} spectate = { FALSE, FALSE, _spectate_mode_follow, NONE };

static char const *const spectate_mode_names[NUMBER_OF_SPECTATE_MODES] =
{
	"free", "follow", "first person", "director"
};
/* network.spectate_camera's names for them */
static char const *const spectate_camera_settings[NUMBER_OF_SPECTATE_MODES] =
{
	"free", "follow", "first", "director"
};

/* the platform layer's (port_config.c) and input's (input_abstraction.c,
xinput_sdl.c) */
int config_boolean(char const *name);
const char *config_string(char const *name);
void platform_log(char const *format, ...);
void input_abstraction_port_input(short controller_index, real *forward, real *strafe, real *yaw,
	real *pitch, byte *buttons, short button_count);
int halo_linux_mouse_look(short gamepad_index, real *yaw, real *pitch);

/* ---------- private code */

static real angle_wrap(
	real angle)
{
	while (angle > _pi)
		angle -= 2.0f * _pi;
	while (angle < -_pi)
		angle += 2.0f * _pi;
	return angle;
}

static void vector_from_angles(
	real yaw,
	real pitch,
	real_vector3d *forward)
{
	forward->i = (real)cos(yaw) * (real)cos(pitch);
	forward->j = (real)sin(yaw) * (real)cos(pitch);
	forward->k = (real)sin(pitch);
}

/* the living unit of a player (NONE: none, or no such player) */
static long player_unit(
	long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	return player && player->unit_index != NONE && object_try_and_get_and_verify_type(player->unit_index, _object_mask_unit) ?
		player->unit_index : NONE;
}

/* the player after (direction 1) or before (-1) `from`, wrapping round,
the living first (NONE: no players) */
static long next_player(
	long from,
	short direction)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long players[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	long count = 0;
	long index;
	long at = NONE;
	short pass;

	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL && count < (long)NUMBEROF(players))
	{
		if (iterator.datum_index == from)
			at = count;
		players[count++] = iterator.datum_index;
	}
	if (!count)
		return NONE;
	/* (alive first: a dead player is taken only if nobody else is alive) */
	for (pass = 0; pass < 2; pass++)
	{
		for (index = 1; index <= count; index++)
		{
			long candidate = at == NONE ?
				players[direction > 0 ? index - 1 : count - index] :
				players[((at + direction * index) % count + count) % count];

			if (pass == 1 || player_unit(candidate) != NONE)
				return candidate;
		}
	}
	return NONE;
}

/* where the follow camera looks on a unit: its chest as drawn */
static void unit_focus(
	long unit_index,
	real_point3d *focus)
{
	object_get_origin(unit_index, focus);
	focus->z += FOLLOW_HEIGHT;
}

static void watch(
	long player_index)
{
	if (player_index == spectate.player_index)
		return;
	spectate.player_index = player_index;
	spectate.switch_seconds = SWITCH_SECONDS;
	spectate.look_idle_seconds = FOLLOW_RETURN_SECONDS;
	if (player_index != NONE)
	{
		char name[16];

		wide_to_ascii(player_get(player_index)->name, name, sizeof(name));
		platform_log("spectate: watching %s (player %ld, %s)", name,
			(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), spectate_mode_names[spectate.mode]);
	}
}

static void set_mode(
	short mode)
{
	struct observer_result const *camera = observer_get_camera(0);

	if (mode == spectate.mode)
		return;
	/* the free camera starts where the view is, looking where it looks */
	if (mode == _spectate_mode_free && camera)
	{
		spectate.position = camera->position;
		spectate.yaw = (real)atan2(camera->forward.j, camera->forward.i);
		spectate.pitch = (real)asin(PIN(camera->forward.k, -1.0f, 1.0f));
	}
	spectate.mode = mode;
	spectate.switch_seconds = SWITCH_SECONDS;
	platform_log("spectate: %s camera", spectate_mode_names[mode]);
	if (mode != _spectate_mode_free && mode != _spectate_mode_director && spectate.player_index == NONE)
		watch(next_player(NONE, 1));
}

/* a new map (or the first): the camera behind the first living player, or
over the map's first spawn looking across it */
static void place(
	void)
{
	/* (the first: the camera network.spectate_camera names) */
	if (!spectate.mode_set)
	{
		char const *setting = config_string("network.spectate_camera");
		short mode;

		spectate.mode_set = TRUE;
		for (mode = 0; setting && mode < NUMBER_OF_SPECTATE_MODES; mode++)
		{
			if (!strcmp(setting, spectate_camera_settings[mode]))
				spectate.mode = mode;
		}
		platform_log("spectate: %s camera", spectate_mode_names[spectate.mode]);
	}
	spectate_director_reset();
	spectate.placed = TRUE;
	spectate.player_index = NONE;
	spectate.focus_valid = FALSE;
	spectate.follow_distance = FOLLOW_DISTANCE;
	spectate.follow_pitch = FOLLOW_PITCH;
	spectate.speed = spectate.speed > 0.0f ? spectate.speed : FREE_SPEED;
	spectate.position = *global_origin3d;
	spectate.yaw = 0.0f;
	spectate.pitch = -0.2f;
	if (spectate.mode != _spectate_mode_director)
		watch(next_player(NONE, 1));
	if (spectate.player_index != NONE && player_unit(spectate.player_index) != NONE)
	{
		unit_focus(player_unit(spectate.player_index), &spectate.position);
		spectate.position.z += 3.0f;
	}
	spectate.switch_seconds = 0.0f;
}

/* a control pressed this frame (held now, not the last) */
static boolean pressed(
	byte const *buttons,
	short control)
{
	return buttons[control] != 0 && !spectate.held[control];
}

/* ---------- public code */

boolean spectate_enabled(
	void)
{
	return spectate.console_set ? spectate.console_enabled : config_boolean("network.spectate");
}

boolean spectate_watching(
	void)
{
	return spectate_enabled() &&
		game_connection() == _game_connection_network_client &&
		game_in_progress() &&
		local_player_count() == 0;
}

boolean spectate_view(
	short local_player_index)
{
	return local_player_index == 0 && spectate_watching();
}

boolean spectate_window(
	void)
{
	return spectate_view(0) && !game_engine_force_single_screen();
}

long spectate_first_person_unit(
	void)
{
	return spectate.mode == _spectate_mode_first_person && spectate_window() ?
		player_unit(spectate.player_index) : NONE;
}

void spectate_update(
	real seconds,
	struct observer_command *command)
{
	real forward = 0.0f, strafe = 0.0f, yaw = 0.0f, pitch = 0.0f;
	real mouse_yaw = 0.0f, mouse_pitch = 0.0f;
	byte buttons[NUMBER_OF_CONTROLS];
	boolean looked;
	long unit_index;
	real_point3d focus;
	real_vector3d direction;
	real distance = 0.0f;
	short control;

	if (spectate.last_game_time > game_time_get() || !spectate.placed)
		place();
	spectate.last_game_time = game_time_get();

	input_abstraction_port_input(0, &forward, &strafe, &yaw, &pitch, buttons, NUMBER_OF_CONTROLS);
	looked = yaw != 0.0f || pitch != 0.0f;
	if (halo_linux_mouse_look(0, &mouse_yaw, &mouse_pitch))
		looked = TRUE;
	yaw = yaw * STICK_TURN_RATE * seconds + mouse_yaw;
	pitch = pitch * STICK_TURN_RATE * seconds + mouse_pitch;

	if (pressed(buttons, _control_action))
		set_mode((short)((spectate.mode + 1) % NUMBER_OF_SPECTATE_MODES));
	/* (a person picking whom to watch takes over from the director) */
	if (pressed(buttons, _control_primary_trigger) || pressed(buttons, _control_grenade))
	{
		if (spectate.mode == _spectate_mode_director)
			set_mode(_spectate_mode_follow);
		watch(next_player(spectate.player_index, pressed(buttons, _control_primary_trigger) ? 1 : -1));
	}
	if (pressed(buttons, _control_switch_weapons))
		spectate.speed = MIN(spectate.speed * 2.0f, FREE_SPEED_MAXIMUM);
	if (pressed(buttons, _control_switch_grenades))
		spectate.speed = MAX(spectate.speed * 0.5f, FREE_SPEED_MINIMUM);
	if (pressed(buttons, _control_flashlight))
		spectate.names_hidden = !spectate.names_hidden;
	for (control = 0; control < NUMBER_OF_CONTROLS; control++)
		spectate.held[control] = buttons[control] != 0;

	/* the player watched gone (they left): the next */
	if (spectate.player_index != NONE && !player_try_and_get(spectate.player_index))
	{
		spectate.player_index = NONE;
		watch(next_player(NONE, 1));
	}
	else if (spectate.player_index == NONE && spectate.mode != _spectate_mode_free &&
		spectate.mode != _spectate_mode_director)
	{
		watch(next_player(NONE, 1));
	}
	/* the director's choice: a glide to someone near, else a hard cut, the
	camera straight behind them */
	if (spectate.mode == _spectate_mode_director)
	{
		boolean glide;
		long chosen = spectate_director_update(seconds, spectate.player_index, &glide);

		if (chosen != spectate.player_index)
		{
			watch(chosen);
			if (!glide && player_unit(chosen) != NONE)
			{
				struct object_datum *unit = object_get(player_unit(chosen));

				spectate.switch_seconds = 0.0f;
				spectate.follow_yaw = (real)atan2(unit->object.forward.j, unit->object.forward.i);
				spectate.follow_pitch = FOLLOW_PITCH;
			}
		}
		/* (no hand on the camera: always back round behind them) */
		yaw = pitch = forward = 0.0f;
		looked = FALSE;
		spectate.look_idle_seconds = FOLLOW_RETURN_SECONDS;
		spectate.follow_distance = DIRECTOR_DISTANCE;
	}
	unit_index = player_unit(spectate.player_index);

	csmemset(command, 0, sizeof(*command));
	command->flags = FLAG(_observer_command_valid_bit);
	command->field_of_view = FIELD_OF_VIEW;
	command->focus_offset = *global_zero_vector3d;
	command->focus_velocity = *global_zero_vector3d;

	switch (spectate.mode)
	{
	case _spectate_mode_first_person:
		if (unit_index != NONE)
		{
			first_person_camera_deterministic(unit_index, &focus, &direction);
			spectate.focus = focus;
			spectate.focus_valid = TRUE;
			break;
		}
		/* (dead: watched from behind until they are back) */
	case _spectate_mode_follow:
	case _spectate_mode_director:
		spectate.follow_yaw = angle_wrap(spectate.follow_yaw + yaw);
		spectate.follow_pitch = PIN(spectate.follow_pitch + pitch, -1.4f, 1.2f);
		spectate.follow_distance = PIN(spectate.follow_distance - forward * 6.0f * seconds,
			FOLLOW_DISTANCE_MINIMUM, FOLLOW_DISTANCE_MAXIMUM);
		spectate.look_idle_seconds = looked ? 0.0f : spectate.look_idle_seconds + seconds;
		if (unit_index != NONE)
		{
			struct object_datum *unit = object_get(unit_index);

			unit_focus(unit_index, &spectate.focus);
			spectate.focus_valid = TRUE;
			/* let go of the look: back round behind them */
			if (spectate.look_idle_seconds >= FOLLOW_RETURN_SECONDS)
			{
				real behind = (real)atan2(unit->object.forward.j, unit->object.forward.i);
				real turn = angle_wrap(behind - spectate.follow_yaw);
				real step = MIN(1.0f, FOLLOW_RETURN_RATE * seconds);

				spectate.follow_yaw = angle_wrap(spectate.follow_yaw + turn * step);
				spectate.follow_pitch += (FOLLOW_PITCH - spectate.follow_pitch) * step;
			}
		}
		else if (!spectate.focus_valid)
		{
			spectate.focus = spectate.position;
			spectate.focus_valid = TRUE;
		}
		focus = spectate.focus;
		vector_from_angles(spectate.follow_yaw, spectate.follow_pitch, &direction);
		distance = spectate.follow_distance;
		break;
	default:
		spectate.yaw = angle_wrap(spectate.yaw + yaw);
		spectate.pitch = PIN(spectate.pitch + pitch, -1.5f, 1.5f);
		vector_from_angles(spectate.yaw, spectate.pitch, &direction);
		{
			real side_i = -(real)sin(spectate.yaw);
			real side_j = (real)cos(spectate.yaw);
			real rise = (buttons[_control_jump] ? 1.0f : 0.0f) - (buttons[_control_crouch] ? 1.0f : 0.0f);
			real step = spectate.speed * seconds;

			spectate.position.x += (direction.i * forward + side_i * strafe) * step;
			spectate.position.y += (direction.j * forward + side_j * strafe) * step;
			spectate.position.z += (direction.k * forward + rise) * step;
		}
		focus = spectate.position;
		/* (through walls) */
		SET_FLAG(command->flags, _observer_command_ignore_obstructions_bit, TRUE);
		break;
	}

	command->focus_position = focus;
	command->focus_distance = distance;
	command->forward = direction;
	observer_up_from_forward(&command->forward, &command->up);
	command->timer = spectate.switch_seconds;
	spectate.switch_seconds = MAX(0.0f, spectate.switch_seconds - seconds);
	/* (the free camera is wherever the view is, should it be taken next) */
	if (spectate.mode != _spectate_mode_free)
	{
		struct observer_result const *camera = observer_get_camera(0);

		if (camera)
			spectate.position = camera->position;
	}
}

/* a line of text in the HUD's font: justification 0 left, 1 right, 2 centred */
static void draw_text(
	rectangle2d const *bounds,
	short justification,
	real_argb_color const *color,
	wchar_t const *text)
{
	long font_index = hud_get_font_index();

	if (font_index == NONE)
		return;
	draw_string_set_draw_mode(font_index, NONE, justification, 0, color);
	rasterizer_draw_unicode_string(bounds, NULL, NULL, 0, text);
}

/* the names of the players in view, over their heads */
static void draw_names(
	short line_height)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct observer_result const *camera = observer_get_camera(0);
	real_argb_color color = *global_real_argb_white;

	if (!camera)
		return;
	color.alpha = 0.8f;
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		long unit_index = player_unit(iterator.datum_index);
		real_point3d head;
		real_point2d screen;
		real dx, dy, dz;
		rectangle2d bounds;
		wchar_t name[16];

		if (unit_index == NONE ||
			(iterator.datum_index == spectate.player_index && spectate.mode == _spectate_mode_first_person))
			continue;
		object_get_origin(unit_index, &head);
		head.z += 0.85f;
		dx = head.x - camera->position.x;
		dy = head.y - camera->position.y;
		dz = head.z - camera->position.z;
		if (dx * dx + dy * dy + dz * dz > NAME_DISTANCE * NAME_DISTANCE ||
			dx * camera->forward.i + dy * camera->forward.j + dz * camera->forward.k <= 0.0f ||
			!render_camera_world_to_screen(&render.camera, &render.frustum, &head, &screen))
		{
			continue;
		}
		ustrncpy(name, player->name, NUMBEROF(player->name));
		name[NUMBEROF(player->name)] = 0;
		bounds.x0 = (short)(screen.x - 120.0f);
		bounds.x1 = (short)(screen.x + 120.0f);
		bounds.y0 = (short)(screen.y - line_height);
		bounds.y1 = (short)(screen.y + 4);
		draw_text(&bounds, 2, &color, name);
	}
}

void spectate_draw(
	void)
{
	long font_index = hud_get_font_index();
	struct font_header *font;
	rectangle2d window = render.camera.window_bounds;
	rectangle2d bounds;
	real_argb_color color = *global_real_argb_white;
	wchar_t text[160];
	short line_height;
	long unit_index = player_unit(spectate.player_index);
	struct gamepad_state const *gamepad = input_get_gamepad_state(0);

	if (font_index == NONE)
		return;
	font = font_definition_get(font_index);
	line_height = (short)(font->ascending_height + font->descending_height + font->leading_height);
	offset_rectangle2d(&window, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);

	if (!spectate.names_hidden)
		draw_names(line_height);

	/* the top line: SPECTATING and the camera */
	bounds = window;
	bounds.y0 = (short)(window.y0 + 8);
	bounds.y1 = (short)(bounds.y0 + line_height);
	if (spectate.mode == _spectate_mode_free)
	{
		usnprintf(text, NUMBEROF(text) - 1, L"SPECTATING   FREE CAMERA   SPEED %d", (int)(spectate.speed + 0.5f));
	}
	else if (spectate.player_index != NONE)
	{
		struct player_datum *player = player_get(spectate.player_index);
		wchar_t name[16];

		ustrncpy(name, player->name, NUMBEROF(player->name));
		name[NUMBEROF(player->name)] = 0;
		if (unit_index != NONE)
		{
			struct object_datum *unit = object_get(unit_index);

			usnprintf(text, NUMBEROF(text) - 1, L"%s %s   HEALTH %d%%   SHIELDS %d%%",
				spectate.mode == _spectate_mode_director ? L"DIRECTOR:" : L"SPECTATING", name,
				(int)(PIN(unit->object.body_vitality, 0.0f, 1.0f) * 100.0f + 0.5f),
				(int)(PIN(unit->object.shield_vitality, 0.0f, 9.0f) * 100.0f + 0.5f));
		}
		else
		{
			usnprintf(text, NUMBEROF(text) - 1, L"SPECTATING %s   (RESPAWNING)", name);
		}
	}
	else
	{
		usnprintf(text, NUMBEROF(text) - 1, L"SPECTATING   NOBODY TO WATCH");
	}
	text[NUMBEROF(text) - 1] = 0;
	draw_text(&bounds, 2, &color, text);

	/* the controls, small, at the bottom */
	color.alpha = 0.6f;
	bounds = window;
	bounds.y1 = (short)(window.y1 - 8);
	bounds.y0 = (short)(bounds.y1 - line_height);
	draw_text(&bounds, 2, &color,
		L"FIRE: NEXT    ACTION: CAMERA    BACK: SCORES");

	/* the scores, while the scoreboard's key is held */
	if (spectate.scores_shown || (gamepad && gamepad->buttons[_gamepad_binary_button_back]))
		game_engine_port_draw_spectator_scoreboard();
}

boolean spectate_console_command(
	char const *text)
{
	char word[32];
	char argument[64];
	int count;

	word[0] = argument[0] = 0;
	count = sscanf(text, " %31s %63[^\n]", word, argument);
	if (count <= 0)
	{
		console_printf(FALSE, "spectate: %s, %s camera, watching player %ld",
			spectate_enabled() ? (spectate_watching() ? "watching" : "on") : "off",
			spectate_mode_names[spectate.mode],
			spectate.player_index == NONE ? -1L : (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(spectate.player_index));
		return TRUE;
	}
	if (!strcmp(word, "on") || !strcmp(word, "off"))
	{
		spectate.console_set = TRUE;
		spectate.console_enabled = !strcmp(word, "on");
		console_printf(FALSE, "spectate: %s (for the next game joined)", word);
		return TRUE;
	}
	if (!spectate_watching())
	{
		console_warning("spectate: not watching a game");
		return FALSE;
	}
	if (!strcmp(word, "next"))
		watch(next_player(spectate.player_index, 1));
	else if (!strcmp(word, "previous"))
		watch(next_player(spectate.player_index, -1));
	else if (!strcmp(word, "free"))
		set_mode(_spectate_mode_free);
	else if (!strcmp(word, "follow"))
		set_mode(_spectate_mode_follow);
	else if (!strcmp(word, "first"))
		set_mode(_spectate_mode_first_person);
	else if (!strcmp(word, "director"))
		set_mode(_spectate_mode_director);
	else if (!strcmp(word, "scores"))
		spectate.scores_shown = !spectate.scores_shown;
	else if (!strcmp(word, "speed") && argument[0])
		spectate.speed = PIN((real)atof(argument), FREE_SPEED_MINIMUM, FREE_SPEED_MAXIMUM);
	else if (!strcmp(word, "player") && argument[0])
	{
		struct data_iterator iterator;
		struct player_datum *player;
		long found = NONE;

		data_iterator_new(&iterator, player_data);
		while ((player = data_iterator_next(&iterator)) != NULL && found == NONE)
		{
			char name[16];

			wide_to_ascii(player->name, name, sizeof(name));
			if (!strncmp(name, argument, strlen(argument)))
				found = iterator.datum_index;
		}
		if (found == NONE)
		{
			console_warning("spectate: no player named %s", argument);
			return FALSE;
		}
		watch(found);
	}
	else
	{
		console_warning("spectate: on, off, next, previous, free, follow, first, director, player <name>, speed <n>, scores");
		return FALSE;
	}
	return TRUE;
}

short spectate_watching_machines(
	struct network_game const *game)
{
	short count = 0;
	short machine;

	for (machine = 1; game && machine < HALO_PORT_MAXIMUM_NETWORK_MACHINES; machine++)
	{
		boolean has_player = FALSE;
		short player;

		/* (a slot in use: its machine index; the host's own is 0) */
		if (game->machines[machine].machine_index <= 0)
			continue;
		for (player = 0; player < HALO_PORT_MAXIMUM_NETWORK_PLAYERS && !has_player; player++)
		{
			has_player = network_player_is_valid((struct network_player *)&game->players[player]) &&
				game->players[player].machine_index == game->machines[machine].machine_index;
		}
		if (!has_player)
			count++;
	}
	return count;
}

void spectate_status(
	short *mode,
	long *player_index)
{
	*mode = spectate.mode;
	*player_index = spectate.player_index;
}
