"""The real skate sounds' entry of skate_network.c (Skate 3 mode in a network game): a skater's loops and last one-shots
sent for the other machines to hear, read back as they were, what is no entry refused, and the one-shots' sequence
numbers, each played once through their wrap."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["round-trip", "malformed", "sequence"]
FUNCTIONS = ("skate_round", "skate_sound_byte", "skate_sound_write", "skate_sound_read", "skate_sound_sequence_new")

# a fault in skate_network.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "format-unchecked": (("data[1] != SKATE_SOUND_FORMAT || ", ""), "skate_sound_read", "malformed"),
    "count-unchecked": (("|| data[5] > SKATE_SOUND_EVENTS ", ""), "skate_sound_read", "malformed"),
    "wheels-unheld": (("state->loops.wheels > 4 ? 4 : state->loops.wheels", "state->loops.wheels"),
                      "skate_sound_write", "round-trip"),
    "speed-wraps": (("step > 255 ? 255 : step", "step"), "skate_sound_byte", "round-trip"),
    "played-again": (("ahead >= 1 && ahead < 128", "ahead < 128"), "skate_sound_sequence_new", "sequence"),
    "no-wrap": (("byte ahead = (byte)(sequence - last);", "int ahead = sequence - last;"),
                "skate_sound_sequence_new", "sequence"),
}


def generated(fault=None, faulty_function=None):
    source = read("port/linux/game/skate_network.c")
    header = read("port/linux/game/skate.h")
    sound_header = read("port/linux/game/skate_sound.h")
    config = ""
    for text in (header, source):
        for match in re.finditer(r"^#define (SKATE_SOUND_\w+) (.+)$", text, re.M):
            config += f"#define {match.group(1)} {match.group(2)}\n"
    for name, text in (("skate_sound_loops", sound_header), ("skate_sound_event", sound_header),
                       ("skate_sound_state", source)):
        match = re.search(rf"^struct {name}\n\{{.*?^\}};\n", text, re.M | re.S)
        assert match, f"struct {name} not found"
        config += match.group(0)
    text = ""
    for name in FUNCTIONS:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("skate_sound", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("name", sorted(NEGATIVE_CONTROLS))
def test_negative_control(name):
    fault, faulty_function, case = NEGATIVE_CONTROLS[name]
    status, output = run(build("skate_sound", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"{name}: the fault was not caught by {case}:\n{output}"


if __name__ == "__main__":
    for case in CASES:
        print(case, run(build("skate_sound", generated()), case))
