"""Freeze wealth-exchange cases and gates before sampling either implementation."""
import argparse
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent


def make_plan():
    return dict(
        version=1, replications=512, seed=630127, reference_seed=982451653,
        scenario=23, order_stream=1001, recipient_stream=1002,
        ticks=[0, 8, 32],
        cases=[dict(id=name, n=24, initial=initial, topology=topology)
               for name, initial, topology in [('mixed-1', 1, 'mixed'),
                                                ('mixed-4', 4, 'mixed'),
                                                ('ring-1', 1, 'ring'),
                                                ('star-1', 1, 'star')]],
        metrics=['gini', 'zero_fraction', 'maximum_share', 'concentration'],
        gates=dict(family_alpha=.001, comparisons=32, mean_sigma=6,
                   mean_floor=dict(gini=.025, zero_fraction=.04,
                                   maximum_share=.025, concentration=.015)),
        power=dict(population_ks_alternative=.30,
                   miss_probability_bound=4*math.exp(-512*(.30-math.sqrt(math.log(64000)/512))**2/2)),
        # DKW + triangle inequality gives this conservative per-comparison miss
        # bound under a .30 population-CDF displacement. It is not a claim of
        # sensitivity to every small implementation error or an equilibrium test.
        scope='finite-time distributions; no equilibrium or universal parameter claim')


def encoded():
    return json.dumps(make_plan(), indent=2, allow_nan=False) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    path = HERE / 'wealth-plan.json'
    if args.verify:
        assert path.read_text() == encoded(), 'wealth plan changed'
    else:
        path.write_text(encoded())
