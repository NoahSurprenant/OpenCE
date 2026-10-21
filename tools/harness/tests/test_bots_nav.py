"""The bots' walking graph and the graph vehicles drive (port/linux/game/bots_nav.c) on made-up levels of boxes."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

CASES = ['floor', 'drop', 'jump', 'wall', 'ceiling', 'slope', 'ramp', 'teleporter', 'ladder', 'prune', 'cache', 'drive']
CONTROLS = {
    # neighbours never found: nothing is walked
    'no-neighbours': ('if (step <= best_step)', 'if (step <= best_step && step < 0.0f)', 'floor'),
    # every drop and jump taken, walls or not
    'links-unchecked': ('	if (!input->clear)\n		return 1;', '	return 1;', 'wall'),
    # no room needed above a floor
    'no-headroom': ('room >= builder->parameters.agent_height', 'room >= 0.0f', 'ceiling'),
    # a vehicle as narrow as a biped
    'vehicle-narrow': ('parameters->agent_radius = 0.5f;', 'parameters->agent_radius = 0.25f;', 'drive'),
    # jumps as high as drops
    'jumps-unlimited': ('rise <= builder->parameters.jump_height', 'rise <= builder->parameters.drop_height', 'jump'),
}


def generated(control=None):
    code = read('port/linux/game/bots_nav.c')
    if control:
        before, after, _ = CONTROLS[control]
        code = mutated(code, before, after)
    return (('bots_nav.h', read('port/linux/game/bots_nav.h')), ('under_test.inc', code))


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('bots_nav', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('control', sorted(CONTROLS))
def test_negative_control(control):
    status, output = run(build('bots_nav', generated(control)), CONTROLS[control][2])
    assert status == CHECK_FAILED, f'{control} went unnoticed: {output}'
