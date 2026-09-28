"""Schelling cases and tolerances, frozen before either sample ensemble."""
import argparse
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent


def make_plan():
    initial = [[int((x*7+y*11+x*y) % 5 < 2), x, y]
               for y in range(6) for x in range(6) if (x+2*y) % 3 != 0]
    cases = [dict(id=name, width=6, height=6, wrap=wrap, moore=moore,
                  numerator=num, denominator=den, agents=initial)
             for name, num, den, wrap, moore in [
                 ('torus-low', 1, 3, True, True),
                 ('torus-medium', 1, 2, True, True),
                 ('torus-high', 2, 3, True, True),
                 ('bounded-von-neumann', 1, 2, False, False)]]
    return dict(version=1, replications=512, seed=873109, reference_seed=32452843,
                scenario=29, order_stream=1101, relocation_stream=1102,
                ticks=[0, 4, 16], cases=cases,
                metrics=['unhappy_fraction', 'neighbor_similarity', 'largest_cluster', 'moved_fraction'],
                gates=dict(family_alpha=.001, comparisons=32, mean_sigma=6,
                           mean_floor=dict(unhappy_fraction=.04, neighbor_similarity=.025,
                                           largest_cluster=.04, moved_fraction=.04)),
                power=dict(population_ks_alternative=.30,
                           miss_probability_bound=4*math.exp(-512*(.30-math.sqrt(math.log(64000)/512))**2/2)),
                scope='finite-time distributions from fixed mixed initial states; no universal equilibrium claim')


def encoded():
    return json.dumps(make_plan(), indent=2, allow_nan=False)+'\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    path = HERE/'schelling-plan.json'
    if args.verify:
        assert path.read_text() == encoded(), 'Schelling plan changed'
    else:
        path.write_text(encoded())
