"""Sugarscape-lite cases and gates declared before generating either ensemble."""
import argparse
import json
import math
from pathlib import Path
HERE = Path(__file__).resolve().parent


def make_plan():
    agents = [[4+(x+3*y)%5, 1+(x+y)%3, 1+(x+2*y)%3, x, y]
              for y in range(6) for x in range(6) if (x+y)%2 == 0]
    capacity = [1+max(0, 4-abs(x-1)-abs(y-1), 4-abs(x-4)-abs(y-4))
                for y in range(6) for x in range(6)]
    cases = [dict(id=name, width=6, height=6, wrap=wrap, regrowth=rate,
                  capacity=capacity, sugar=capacity, agents=agents)
             for name, wrap, rate in [('torus-renewable', True, 1),
                                     ('bounded-renewable', False, 1),
                                     ('torus-depletion', True, 0),
                                     ('bounded-fast-regrowth', False, 2)]]
    return dict(version=1, replications=512, seed=15485863, reference_seed=49979687,
                scenario=31, order_stream=1201, movement_stream=1202,
                ticks=[0, 4, 16], cases=cases,
                metrics=['survival_fraction', 'reserve_per_initial_agent',
                         'land_fraction', 'consumed_per_initial_agent'],
                gates=dict(family_alpha=.001, comparisons=32, mean_sigma=6,
                           mean_floor=dict(survival_fraction=.04, reserve_per_initial_agent=.35,
                                           land_fraction=.03, consumed_per_initial_agent=.35)),
                power=dict(population_ks_alternative=.30,
                           miss_probability_bound=4*math.exp(-512*(.30-math.sqrt(math.log(64000)/512))**2/2)),
                scope='sequential axial-vision harvest/metabolism/death and end-sweep regrowth; finite-time fixed landscapes; no reproduction, trade or universal equilibrium claim')


def encoded():
    return json.dumps(make_plan(), indent=2, allow_nan=False)+'\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    path = HERE/'sugarscape-plan.json'
    if args.verify:
        assert path.read_text() == encoded(), 'Sugarscape plan changed'
    else:
        path.write_text(encoded())
