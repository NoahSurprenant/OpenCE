/*
SKATE_SOUND.C (test)

The real skate sounds' entry of port/linux/game/skate_network.c, as Skate 3
mode sends a skater's sounds in a network game for the other machines to
hear: the loops' state and the last one-shots written to their bytes and read
back as they were (to the wire's steps), what is no entry refused, and the
one-shots' sequence numbers, which play each once, through their wrap.
test_skate_sound.py takes the sizes and structures (config.inc) and the code
(under_test.inc).
*/

#include "harness.h"

#include <math.h>

#include "config.inc"
#include "under_test.inc"

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	byte bytes[SKATE_SOUND_SIZE];
	struct skate_sound_state state, read_back;
	byte player_index = 0;
	short index;

	memset(&state, 0, sizeof(state));
	state.loops.wheels = 3;
	state.loops.grind = 2;
	state.loops.powerslide = TRUE;
	state.loops.surface = 1;
	state.loops.speed = 7.3f;
	state.newest = 201;
	state.count = SKATE_SOUND_EVENTS;
	for (index = 0; index < SKATE_SOUND_EVENTS; index++)
	{
		state.events[index].sound = (short)(4 + index);
		state.events[index].strength = 0.2f * (real)(index + 1);
		state.events[index].speed = 1.5f + (real)index;
		state.ages[index] = (byte)(12 - 3 * index);
	}

	if (!strcmp(case_name, "round-trip"))
	{
		skate_sound_write(9, &state, bytes);
		CHECK(skate_sound_read(bytes, &player_index, &read_back), "the entry was not read back");
		CHECK(player_index == 9, "player %d read back", player_index);
		CHECK(read_back.loops.wheels == 3 && read_back.loops.grind == 2 && read_back.loops.powerslide &&
			read_back.loops.surface == 1, "the loops read back as %d %d %d %d", read_back.loops.wheels,
			read_back.loops.grind, read_back.loops.powerslide, read_back.loops.surface);
		CHECK(fabs(read_back.loops.speed - 7.25f) < 0.001f, "the speed read back as %f", read_back.loops.speed);
		CHECK(read_back.newest == 201 && read_back.count == SKATE_SOUND_EVENTS, "newest %d, %d one-shots",
			read_back.newest, read_back.count);
		for (index = 0; index < SKATE_SOUND_EVENTS; index++)
		{
			CHECK(read_back.events[index].sound == state.events[index].sound, "one-shot %d is sound %d", index,
				read_back.events[index].sound);
			CHECK(fabs(read_back.events[index].strength - state.events[index].strength) <= 0.5f / 255.f + 1e-6f,
				"one-shot %d's strength %f", index, read_back.events[index].strength);
			CHECK(fabs(read_back.events[index].speed - state.events[index].speed) <= 0.125f + 1e-6f,
				"one-shot %d's speed %f", index, read_back.events[index].speed);
			CHECK(read_back.ages[index] == state.ages[index], "one-shot %d's age %d", index, read_back.ages[index]);
		}
		/* what the wire cannot hold is held to it: too fast, too hard, too
		many, nothing below nothing */
		state.loops.speed = 500.f;
		state.loops.wheels = 9;
		state.events[0].strength = 3.f;
		state.events[1].strength = -1.f;
		state.events[2].speed = -4.f;
		state.count = 40;
		skate_sound_write(9, &state, bytes);
		CHECK(skate_sound_read(bytes, &player_index, &read_back), "a held entry was not read back");
		CHECK(read_back.loops.speed == 255.f / SKATE_SOUND_SPEED_SCALE && read_back.loops.wheels == 4,
			"held to %f m/s and %d wheels", read_back.loops.speed, read_back.loops.wheels);
		CHECK(read_back.count == SKATE_SOUND_EVENTS, "%d one-shots", read_back.count);
		CHECK(read_back.events[0].strength == 1.f && read_back.events[1].strength == 0.f &&
			read_back.events[2].speed == 0.f, "strengths %f %f, speed %f", read_back.events[0].strength,
			read_back.events[1].strength, read_back.events[2].speed);
		/* none: the loops alone */
		state.count = 0;
		skate_sound_write(2, &state, bytes);
		CHECK(skate_sound_read(bytes, &player_index, &read_back) && read_back.count == 0, "no one-shots");
		return 0;
	}
	if (!strcmp(case_name, "malformed"))
	{
		skate_sound_write(1, &state, bytes);
		bytes[1] = SKATE_SOUND_FORMAT + 1;
		CHECK(!skate_sound_read(bytes, &player_index, &read_back), "another format was read");
		skate_sound_write(1, &state, bytes);
		bytes[5] = SKATE_SOUND_EVENTS + 1;
		CHECK(!skate_sound_read(bytes, &player_index, &read_back), "too many one-shots read");
		skate_sound_write(1, &state, bytes);
		bytes[2] = (byte)((bytes[2] & ~7) | 5);
		CHECK(!skate_sound_read(bytes, &player_index, &read_back), "five wheels read");
		skate_sound_write(1, &state, bytes);
		bytes[2] = (byte)(bytes[2] | 3 << 3);
		CHECK(!skate_sound_read(bytes, &player_index, &read_back), "a grind there is not read");
		/* and anything at all: read or refused, never more than it holds */
		for (index = 0; index < 20000; index++)
		{
			short at;

			for (at = 0; at < SKATE_SOUND_SIZE; at++)
				bytes[at] = (byte)(rand() & 0xFF);
			if (index & 1)
				bytes[1] = SKATE_SOUND_FORMAT;
			if (skate_sound_read(bytes, &player_index, &read_back))
			{
				CHECK(read_back.count >= 0 && read_back.count <= SKATE_SOUND_EVENTS, "%d one-shots", read_back.count);
				CHECK(read_back.loops.wheels <= 4 && read_back.loops.grind <= 2, "the loops %d %d",
					read_back.loops.wheels, read_back.loops.grind);
				CHECK(read_back.loops.speed >= 0.f && read_back.loops.speed <= 64.f, "%f m/s", read_back.loops.speed);
			}
		}
		return 0;
	}
	if (!strcmp(case_name, "sequence"))
	{
		CHECK(skate_sound_sequence_new(5, 4), "the next is new");
		CHECK(!skate_sound_sequence_new(4, 4), "the one played is not new");
		CHECK(!skate_sound_sequence_new(3, 4), "an older one is not new");
		CHECK(skate_sound_sequence_new(0, 255) && skate_sound_sequence_new(3, 250), "new through the wrap");
		CHECK(!skate_sound_sequence_new(250, 3), "old through the wrap");
		CHECK(skate_sound_sequence_new((byte)(4 + 127), 4) && !skate_sound_sequence_new((byte)(4 + 128), 4),
			"half the range ahead is the most that is new");
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", case_name);
	return 2;
}
