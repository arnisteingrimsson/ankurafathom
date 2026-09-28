"""Boids force, geometry and statistical cases frozen before ensemble generation."""
import argparse
import json
import math
from pathlib import Path
HERE = Path(__file__).resolve().parent


def parameters(**changes):
    result = dict(width=10., height=8., wrap=True, dt=.125, vision=3., separation_radius=1.,
                  alignment=1., cohesion=.3, separation=.5, max_acceleration=2.,
                  max_speed=2., softening=.1, bin_width=1.)
    result.update(changes)
    return result


def make_plan():
    cases = [dict(id=name, n=20, parameters=parameters(**options)) for name, options in [
        ('periodic-balanced', {}), ('periodic-alignment', dict(alignment=3., cohesion=.1, separation=.2)),
        ('periodic-separation', dict(alignment=.3, cohesion=.1, separation=2., separation_radius=2.)),
        ('reflecting-balanced', dict(wrap=False))]]
    return dict(version=1, replications=512, seed=15485863, reference_seed=49979687,
                scenario=31, position_stream=1201, velocity_stream=1202,
                ticks=[0, 8, 24], cases=cases,
                metrics=['polarization', 'mean_speed', 'neighbor_fraction', 'mean_pair_distance'],
                gates=dict(family_alpha=.001, comparisons=32, mean_sigma=6,
                           mean_floor=dict(polarization=.04, mean_speed=.025,
                                           neighbor_fraction=.025, mean_pair_distance=.025)),
                power=dict(population_ks_alternative=.30,
                           miss_probability_bound=4*math.exp(-512*(.30-math.sqrt(math.log(64000)/512))**2/2)),
                path_tolerance=dict(relative=2e-10, absolute=2e-10),
                scope='declared synchronous finite-time variant, randomized initial states; no long-time chaos/equilibrium claim')


def encoded():
    return json.dumps(make_plan(), indent=2, allow_nan=False)+'\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    path = HERE/'boids-plan.json'
    if args.verify:
        assert path.read_text() == encoded(), 'Boids plan changed'
    else:
        path.write_text(encoded())
