/*
SKATE_VERDICT.C (test)

The real verdict of port/linux/game/skate_authority.c, as the host of a
network game decides a skater's hit on another player, their vehicle or
another skater: who bails, at what speed, and the push on a person on foot;
and the report and the verdict on the wire, written and read back, the
words every machine logs alike, and what is not an entry refused.
test_skate_verdict.py takes the sizes, flags and structures (config.inc)
and the code (under_test.inc).
*/

#include "harness.h"

#include <math.h>

typedef unsigned short word;
#define TICKS_PER_SECOND 30

#include "config.inc"
#include "under_test.inc"

/* world units a second, from metres a second */
#define MS(v) ((v) / 3.048f)

static struct skate_verdict_input hit(real claimed, real bound)
{
	struct skate_verdict_input input;

	memset(&input, 0, sizeof(input));
	input.parties[0].skater = TRUE;
	input.parties[0].riding = TRUE;
	input.claimed = claimed;
	input.bound = bound;
	input.distance = 0.5f;
	input.allowed = 1.5f;
	input.bail_speed = MS(6.f);
	input.normal.i = -1.f;
	return input;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	struct skate_verdict_input input;
	struct skate_verdict_outcome out;
	byte bytes[64];
	short trial;
	short index;

	/* two skaters: past the bail speed both who ride bail; slower, only one
	whose own engine wanted to, and not below the least; one who does not
	ride never */
	CASE("skaters")
	{
		input = hit(MS(7.f), MS(7.5f));
		input.parties[1].skater = TRUE;
		input.parties[1].riding = TRUE;
		skate_verdict_decide(&input, &out);
		CHECK(out.believed && out.bails[0] && out.bails[1], "a 7 m/s hit did not knock both over");
		CHECK(fabs(out.closing - MS(7.f)) < 0.001f, "the speed decided by is not the one told: %f", out.closing);
		CHECK(out.push.i == 0.f && out.push.k == 0.f, "a skater was pushed");
		input = hit(MS(4.f), MS(5.f));
		input.parties[1].skater = TRUE;
		input.parties[1].riding = TRUE;
		input.parties[0].wants_bail = TRUE;
		skate_verdict_decide(&input, &out);
		CHECK(out.bails[0] && !out.bails[1], "4 m/s: the wish of the one who wanted is not alone granted");
		input.claimed = MS(1.5f);
		skate_verdict_decide(&input, &out);
		CHECK(!out.bails[0], "a wish at 1.5 m/s was granted");
		input = hit(MS(8.f), MS(8.f));
		input.parties[1].skater = TRUE;
		input.parties[1].riding = FALSE;
		input.parties[0].riding = FALSE;
		skate_verdict_decide(&input, &out);
		CHECK(!out.bails[0] && !out.bails[1], "a skater not riding bailed");
		return 0;
	}

	/* the host takes no speed its copies did not close at (and the slack),
	nor a hit of two it has far apart, nor what is no number */
	CASE("cheats")
	{
		input = hit(MS(20.f), MS(3.f));
		input.parties[1].skater = TRUE;
		input.parties[1].riding = TRUE;
		skate_verdict_decide(&input, &out);
		CHECK(out.believed && !out.bails[0] && !out.bails[1], "a 20 m/s hit told of 3 m/s copies knocked over");
		CHECK(out.closing <= MS(3.f) + SKATE_VERDICT_SLACK + 0.001f, "the speed was not held to the bound");
		input = hit(MS(7.f), MS(7.f));
		input.distance = 3.f;
		skate_verdict_decide(&input, &out);
		CHECK(!out.believed && !out.bails[0], "a hit of two far apart was believed");
		input = hit(NAN, MS(7.f));
		skate_verdict_decide(&input, &out);
		CHECK(!out.believed, "a speed that is no number was believed");
		input = hit(-MS(7.f), MS(7.f));
		skate_verdict_decide(&input, &out);
		CHECK(!out.believed, "a speed below 0 was believed");
		/* a skater who says they never bail still does past the bail speed */
		input = hit(MS(8.f), MS(8.f));
		skate_verdict_decide(&input, &out);
		CHECK(out.bails[0], "a skater whose engine held no bail was spared at 8 m/s");
		return 0;
	}

	/* a person on foot is pushed away from the skater along the ground,
	lifted a little, by the closing speed up to the most; a slow hit, a
	vehicle or a skater are not */
	CASE("push")
	{
		input = hit(MS(5.f), MS(5.f));
		input.parties[1].pushable = TRUE;
		/* (the normal from the person toward the skater: the skater came from -X) */
		input.normal.i = -0.8f;
		input.normal.k = 0.6f;
		skate_verdict_decide(&input, &out);
		CHECK(!out.bails[1], "a person on foot bailed");
		CHECK(out.push.i > 0.f && fabs(out.push.j) < 0.001f, "not pushed away from the skater: %f %f", out.push.i,
			out.push.j);
		CHECK(fabs(out.push.i - MS(5.f) * SKATE_PUSH_SHARE) < 0.01f, "pushed %f, not the closing speed", out.push.i);
		CHECK(fabs(out.push.k - out.push.i * SKATE_PUSH_LIFT) < 0.001f, "not lifted by a quarter");
		input.claimed = MS(30.f);
		input.bound = MS(30.f);
		skate_verdict_decide(&input, &out);
		CHECK(out.push.i <= SKATE_PUSH_MOST + 0.001f, "pushed past the most: %f", out.push.i);
		input.claimed = MS(0.5f);
		skate_verdict_decide(&input, &out);
		CHECK(out.push.i == 0.f && out.push.k == 0.f, "a 0.5 m/s touch pushed");
		input = hit(MS(5.f), MS(5.f));
		skate_verdict_decide(&input, &out);
		CHECK(out.push.i == 0.f, "what is not to be pushed was pushed");
		return 0;
	}

	/* a person on foot hit at the run-over speed or faster is run over, as by
	a vehicle: the skater rides on, past the bail speed too, and the person
	is dealt Halo's vehicle collision damage by the speed (none below 3 m/s,
	a rising share to 6.1 m/s, Halo's lethal vehicle speed, the whole past
	it); slower a bump as before; one gone through on the skater's machine
	is run over whatever the host's speed; two skaters never */
	CASE("run_over")
	{
		CHECK(skate_run_over_share(MS(2.f)) == 0.f, "a 2 m/s hit dealt %f", skate_run_over_share(MS(2.f)));
		CHECK(skate_run_over_share(SKATE_RUN_OVER_SPEED) == 0.f, "the least run-over speed dealt damage");
		CHECK(skate_run_over_share(MS(4.5f)) > 0.05f && skate_run_over_share(MS(4.5f)) < SKATE_RUN_OVER_HURT_MOST,
			"a 4.5 m/s run-over dealt %f", skate_run_over_share(MS(4.5f)));
		CHECK(skate_run_over_share(MS(4.f)) < skate_run_over_share(MS(5.f)), "a faster run-over did not deal more");
		CHECK(skate_run_over_share(MS(6.f)) <= SKATE_RUN_OVER_HURT_MOST + 0.001f, "6 m/s, under Halo's 6.1, killed");
		CHECK(skate_run_over_share(SKATE_RUN_OVER_LETHAL) == 1.f && skate_run_over_share(MS(13.9f)) == 1.f,
			"a run-over past Halo's lethal vehicle speed did not deal it whole");
		CHECK(skate_run_over_share(NAN) == 0.f && skate_run_over_share(-1.f) == 0.f, "no speed dealt damage");
		/* (fast: run over, the skater rides on though past the bail speed and
		though their engine wanted to bail; the person pushed) */
		input = hit(MS(13.9f), MS(14.f));
		input.parties[1].pushable = TRUE;
		input.parties[0].wants_bail = TRUE;
		skate_verdict_decide(&input, &out);
		CHECK(out.believed && out.run_over && out.share == 1.f, "a 13.9 m/s hit on foot not run over whole: %d %f",
			out.run_over, out.share);
		CHECK(!out.bails[0] && !out.bails[1], "the skater bailed running a person over");
		CHECK(out.push.i > 0.f, "the person run over was not pushed");
		/* (slow: a bump, nobody hurt) */
		input = hit(MS(2.f), MS(2.f));
		input.parties[1].pushable = TRUE;
		skate_verdict_decide(&input, &out);
		CHECK(!out.run_over && out.share == 0.f, "a 2 m/s hit ran a person over");
		/* (gone through on the skater's machine: run over, by the host's speed) */
		input.run_over_told = TRUE;
		skate_verdict_decide(&input, &out);
		CHECK(out.run_over && out.share == 0.f && !out.bails[0], "a person gone through not run over, or hurt");
		/* (two skaters, or a vehicle: no run-over, bails as before) */
		input = hit(MS(13.9f), MS(14.f));
		input.parties[1].skater = TRUE;
		input.parties[1].riding = TRUE;
		input.run_over_told = TRUE;
		skate_verdict_decide(&input, &out);
		CHECK(!out.run_over && out.bails[0] && out.bails[1], "two skaters at 13.9 m/s did not bump and bail");
		input = hit(MS(13.9f), MS(14.f));
		skate_verdict_decide(&input, &out);
		CHECK(!out.run_over && out.bails[0], "a vehicle was run over");
		return 0;
	}

	/* reports and verdicts written and read back as they were (within the
	wire's steps), and the words of a verdict */
	CASE("wire")
	{
		struct skate_report report, report_back;
		struct skate_verdict verdict, verdict_back;
		char text[192];

		memset(&report, 0, sizeof(report));
		report.player_index = 3;
		report.flags = FLAG(_skate_report_bail_held_bit);
		report.number = 200;
		report.target_index = (long)0xE2A10007;
		report.closing = MS(7.3f);
		report.normal.i = 0.6f;
		report.normal.k = -0.8f;
		report.time = 123456;
		skate_report_write(&report, bytes);
		CHECK(skate_report_read(bytes, &report_back), "a report was not read back");
		CHECK(report_back.player_index == 3 && report_back.flags == report.flags && report_back.number == 200 &&
			report_back.target_index == report.target_index && report_back.time == 123456,
			"a report's player, flags, number, target or time changed");
		CHECK(fabs(report_back.closing - report.closing) < 0.001f && fabs(report_back.normal.k + 0.8f) < 0.0001f,
			"a report's speed or normal changed");
		memset(&verdict, 0, sizeof(verdict));
		verdict.flags = FLAG(_skate_verdict_first_bails_bit) | FLAG(_skate_verdict_second_pushed_bit) |
			FLAG(_skate_verdict_second_on_foot_bit);
		verdict.player_indices[0] = 1;
		verdict.player_indices[1] = 0;
		verdict.number = 65000;
		verdict.closing = MS(7.3f);
		verdict.object_indices[0] = (long)0xE2A10003;
		verdict.object_indices[1] = 77;
		verdict.push.i = MS(4.f) / TICKS_PER_SECOND;
		verdict.push.k = MS(1.f) / TICKS_PER_SECOND;
		verdict.report_number = 9;
		skate_verdict_write(&verdict, bytes);
		CHECK(skate_verdict_read(bytes, &verdict_back), "a verdict was not read back");
		CHECK(verdict_back.flags == verdict.flags && verdict_back.number == 65000 && verdict_back.player_indices[0] == 1 &&
			verdict_back.object_indices[0] == verdict.object_indices[0] && verdict_back.object_indices[1] == 77 &&
			verdict_back.report_number == 9, "a verdict's flags, number, players, objects or report changed");
		CHECK(fabs(verdict_back.push.i - verdict.push.i) < 0.001f, "a verdict's push changed");
		skate_verdict_text(&verdict_back, text, sizeof(text));
		CHECK(!strcmp(text, "player 1's skater bails; player 0 on foot pushed 2.1 m/s; at 7.3 m/s"), "the words: %s",
			text);
		/* (a run-over: its flag and its words, the share by the speed) */
		verdict.flags = FLAG(_skate_verdict_second_pushed_bit) | FLAG(_skate_verdict_second_on_foot_bit) |
			FLAG(_skate_verdict_second_run_over_bit);
		verdict.closing = MS(4.5f);
		skate_verdict_write(&verdict, bytes);
		CHECK(skate_verdict_read(bytes, &verdict_back) && verdict_back.flags == verdict.flags,
			"a run-over's flags changed");
		skate_verdict_text(&verdict_back, text, sizeof(text));
		CHECK(!strcmp(text, "player 1's skater rides on; player 0 on foot run over, 0.07 of a vehicle's hit, pushed "
			"2.1 m/s; at 4.5 m/s"), "the words: %s", text);
		report.flags = FLAG(_skate_report_run_over_bit);
		skate_report_write(&report, bytes);
		CHECK(skate_report_read(bytes, &report_back) && report_back.flags == report.flags, "a run-over report changed");
		return 0;
	}

	/* entries of another format, or with no speed, or a normal that is not
	one, are refused; bytes at random read as numbers or not at all */
	CASE("malformed")
	{
		struct skate_report report;
		struct skate_verdict verdict;

		memset(bytes, 0, sizeof(bytes));
		bytes[0] = SKATE_REPORT_FORMAT + 1;
		CHECK(!skate_report_read(bytes, &report), "a report of another format was read");
		bytes[0] = SKATE_REPORT_FORMAT;
		CHECK(!skate_report_read(bytes, &report), "a report with no normal was read");
		bytes[0] = SKATE_VERDICT_FORMAT + 1;
		CHECK(!skate_verdict_read(bytes, &verdict), "a verdict of another format was read");
		bytes[0] = SKATE_VERDICT_FORMAT;
		bytes[7] = 0x80;
		CHECK(!skate_verdict_read(bytes, &verdict), "a verdict with a speed below 0 was read");
		for (trial = 0; trial < 20000; trial++)
		{
			for (index = 0; index < SKATE_VERDICT_SIZE; index++)
				bytes[index] = (byte)rand();
			bytes[0] = (byte)(trial & 1);
			if (skate_report_read(bytes, &report))
				CHECK(report.closing >= 0.f && report.closing <= 32.f, "a report's speed read as %f", report.closing);
			if (skate_verdict_read(bytes, &verdict))
				CHECK(verdict.closing >= 0.f && fabs(verdict.push.i) <= 32.f, "a verdict read out of range");
		}
		return 0;
	}

	fprintf(stderr, "unknown case %s\n", case_name);
	return 2;
}
