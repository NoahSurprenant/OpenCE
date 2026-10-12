"""The real skate state of skate_network.c (Skate 3 mode in a network game): a skater's pose sent as each node's turn
from its bind pose and rebuilt on another machine as the skate rig poses it, the offsets that cannot be sent left out,
and what is no state refused."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run  # noqa: E402

CASES = ["round-trip", "offsets", "malformed"]
FUNCTIONS = ("skate_round", "skate_quaternion_pack", "skate_quaternion_unpack", "skate_put_short", "skate_get_short",
             "skate_put_long", "skate_get_long", "skate_offset_valid", "skate_state_write", "skate_state_read",
             "skate_quaternion_from_matrix", "skate_quaternion_multiply", "skate_quaternion_rotate",
             "skate_skeleton_valid", "skate_pose_turns", "skate_pose_rebuild")

# a fault in skate_network.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "turns-not-from-the-bind-pose": (("bind[0] = -bind[0]; bind[1] = -bind[1]; bind[2] = -bind[2];", ""),
                                     "skate_pose_turns", "round-trip"),
    "children-at-their-own-turn": (("skate_quaternion_rotate(state->turns[parent], &offset, &turned);",
                                    "skate_quaternion_rotate(turn, &offset, &turned);"), "skate_pose_rebuild",
                                   "round-trip"),
    "format-unchecked": (("|| data[3] != SKATE_STATE_FORMAT ", ""), "skate_state_read", "malformed"),
    "no-turn-accepted": (("if (sum > 0.76f)\n\t\treturn FALSE;", ""), "skate_quaternion_unpack", "malformed"),
    "far-root-sent": (("if (!skate_offset_valid(&state->root_offset))\n\t\tnode_count = 0;", ""),
                      "skate_state_write", "offsets"),
}


def generated(fault=None, faulty_function=None):
    source = read("port/linux/game/skate_network.c")
    header = read("port/linux/game/skate.h")
    config = ""
    for text in (header, source):
        for match in re.finditer(r"^#define (SKATE_(?:STATE|QUATERNION)_\w+) (.+)$", text, re.M):
            config += f"#define {match.group(1)} {match.group(2)}\n"
    config += enum_with(source, "_skate_state_skating_bit") + "\n"
    match = re.search(r"^struct skate_state\n\{.*?^\};\n", source, re.M | re.S)
    assert match, "struct skate_state not found in skate_network.c"
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
    status, output = run(build("skate_state", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("name", sorted(NEGATIVE_CONTROLS))
def test_negative_control(name):
    fault, faulty_function, case = NEGATIVE_CONTROLS[name]
    status, output = run(build("skate_state", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"{name}: the fault was not caught by {case}:\n{output}"


if __name__ == "__main__":
    for case in CASES:
        print(case, run(build("skate_state", generated()), case))
