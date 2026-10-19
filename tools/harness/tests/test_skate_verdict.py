"""The real verdict of skate_authority.c (Skate 3 mode in a network game): the host decides a skater's hit on another
player, their vehicle or another skater once, from its own view (who bails, the push on a person on foot, a person
on foot run over and dealt Halo's vehicle collision damage by the speed), taking no speed its copies did not close
at and no hit of two it had far apart; and the report and the verdict on the wire."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run  # noqa: E402

CASES = ["skaters", "cheats", "push", "run_over", "wire", "malformed"]
FUNCTIONS = ("skate_verdict_real_valid", "skate_run_over_share", "skate_verdict_decide", "skate_authority_round",
             "skate_authority_put_short", "skate_authority_get_short", "skate_authority_put_long",
             "skate_authority_get_long", "skate_report_write", "skate_report_read", "skate_verdict_write",
             "skate_verdict_read", "skate_verdict_text")
STRUCTS = ("skate_verdict_party", "skate_verdict_input", "skate_verdict_outcome", "skate_report", "skate_verdict")

# a fault in skate_authority.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "told-speed-trusted": (("if (out->closing > input->bound + SKATE_VERDICT_SLACK)",
                            "if (0)"), "skate_verdict_decide", "cheats"),
    "far-hit-believed": ((" &&\n\t\tinput->distance <= input->allowed", ""), "skate_verdict_decide", "cheats"),
    "wish-alone-bails": (("(party->wants_bail && out->closing >= SKATE_VERDICT_LEAST_BAIL)", "party->wants_bail"),
                         "skate_verdict_decide", "skaters"),
    "pushed-toward-the-skater": (("out->push.i = -input->normal.i", "out->push.i = input->normal.i"),
                                 "skate_verdict_decide", "push"),
    "run-over-bails": (("out->bails[0] = FALSE;", ""), "skate_verdict_decide", "run_over"),
    "skaters-run-over": (("if (input->parties[1].pushable && (input->run_over_told", "if ((input->run_over_told"),
                         "skate_verdict_decide", "run_over"),
    "run-over-unscaled": (("return SKATE_RUN_OVER_HURT_MOST * (closing", "return 1.f + 0.f * (closing"),
                          "skate_run_over_share", "run_over"),
    "format-unchecked": (("if (data[0] != SKATE_VERDICT_FORMAT)\n\t\treturn FALSE;", ""), "skate_verdict_read",
                         "malformed"),
}


def generated(fault=None, faulty_function=None):
    source = read("port/linux/game/skate_authority.c")
    header = read("port/linux/game/skate.h")
    config = ""
    for text in (header, source):
        pattern = r"^#define ((?:SKATE_(?:REPORT|VERDICT|PUSH|AUTHORITY|RUN_OVER)_)\w+) (.+)$"
        for match in re.finditer(pattern, text, re.M):
            config += f"#define {match.group(1)} {match.group(2)}\n"
    config += enum_with(source, "_skate_report_bail_held_bit") + "\n"
    config += enum_with(source, "_skate_verdict_first_bails_bit") + "\n"
    for name in STRUCTS:
        match = re.search(r"^struct " + name + r"\n\{.*?^\};\n", source, re.M | re.S)
        assert match, f"struct {name} not found in skate_authority.c"
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
    status, output = run(build("skate_verdict", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("name", sorted(NEGATIVE_CONTROLS))
def test_negative_control(name):
    fault, faulty_function, case = NEGATIVE_CONTROLS[name]
    status, output = run(build("skate_verdict", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"{name}: the fault was not caught by {case}:\n{output}"


if __name__ == "__main__":
    for case in CASES:
        print(case, run(build("skate_verdict", generated()), case))
