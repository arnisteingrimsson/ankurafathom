"""Pre-native Jackson acceptance plan; independent Python max-plus pilot.

No native simulator or native RNG is used. All times are in arbitrary model units.
"""
import argparse
from fractions import Fraction as F
import itertools
import json
import math
from pathlib import Path
import random
import statistics

WARMUP, WINDOW, PILOTS = 200, 1000, 64
CASES = [('downstream_bottleneck', ['1', '1.25', '0.8']),
         ('upstream_bottleneck', ['0.8', '1.25', '1'])]
CELLS = list(itertools.product(range(3), repeat=3))  # 0, 1, >=2 at each station


def targets(rates):
    arrival = F(1, 2)
    result = {}
    probabilities = []
    for i, rate in enumerate(rates):
        rho = arrival/rate
        wait = rho/(rate-arrival)
        result.update({f's{i}_queue': arrival*wait, f's{i}_wait': wait,
                       f's{i}_utilization': rho, f's{i}_throughput': arrival})
        probabilities.append([1-rho, (1-rho)*rho, rho*rho])
    result['cycle'] = sum(1/(rate-arrival) for rate in rates)
    for cell in CELLS:
        result['p'+''.join(map(str, cell))] = math.prod(probabilities[i][n] for i, n in enumerate(cell))
    assert sum(value for key, value in result.items() if key.startswith('p')) == 1
    return result


def tolerance(key, expected):
    if key.startswith('p'): return .025
    if key.endswith('_utilization'): return .035
    if key.endswith('_throughput'): return .04
    if key.endswith('_queue'): return .04+.20*expected
    if key.endswith('_wait'): return .06+.20*expected
    return .15+.15*expected


def pilot(rates, seed):
    arrival_rng = random.Random(seed)
    service_rng = [random.Random(seed+9000000+100000*i) for i in range(3)]
    def exponential(rng, rate): return -math.log1p(-rng.random())/rate
    end = WARMUP+WINDOW
    finish = [0.0]*3
    result = {key: 0.0 for key in targets(list(map(lambda x: F(str(x)), rates)))}
    cohort = [0]*3
    cycles = 0
    events = [(0.0, 0, 0), (float(end), 0, 0)]
    born = exponential(arrival_rng, .5)
    while born <= end:
        entry = born
        for i in range(3):
            start = max(entry, finish[i])
            finish[i] = start+exponential(service_rng[i], rates[i])
            events.extend([(entry, i, 1), (finish[i], i, -1)])
            if WARMUP < entry <= end:
                result[f's{i}_wait'] += start-entry
                cohort[i] += 1
            if WARMUP < finish[i] <= end: result[f's{i}_throughput'] += 1/WINDOW
            entry = finish[i]
        if WARMUP < born <= end:
            result['cycle'] += entry-born
            cycles += 1
        born += exponential(arrival_rng, .5)
    state = [0]*3
    previous = 0
    for time, stage, change in sorted(events):
        elapsed = max(0.0, min(time, end)-max(previous, WARMUP))
        result['p'+''.join(str(min(n, 2)) for n in state)] += elapsed/WINDOW
        for i, n in enumerate(state):
            result[f's{i}_queue'] += max(0, n-1)*elapsed/WINDOW
            result[f's{i}_utilization'] += (n > 0)*elapsed/WINDOW
        state[stage] += change
        previous = time
    assert state == [0]*3
    for i in range(3): result[f's{i}_wait'] /= cohort[i]
    result['cycle'] /= cycles
    return result


def generate():
    plan = dict(version=1, warmup=WARMUP, window=WINDOW, native_seed=2026092402,
                arrival_rate=.5, arrival_stream=200, service_streams=[201, 202, 203],
                pilot_replications=PILOTS, pilot_seed_base=811001,
                sigma_inflation=1.25, false_alarm_z=4.0, power_z=1.645, cases=[])
    for index, (name, rates) in enumerate(CASES):
        exact = targets(list(map(F, rates)))
        samples = [pilot(list(map(float, rates)), 811001+1000*index+r) for r in range(PILOTS)]
        required, metrics = 16, {}
        for key, target in exact.items():
            sigma = statistics.stdev(s[key] for s in samples)
            gate = tolerance(key, float(target))
            count = math.ceil(((4+1.645)*1.25*sigma/gate)**2)
            required = max(required, count)
            metrics[key] = dict(target=float(target), exact=str(target), tolerance=gate,
                                pilot_sd=float(f'{sigma:.12g}'), required_replications=count)
        plan['cases'].append(dict(name=name, scenario=30+index, service_rates=list(map(float, rates)),
                                  replications=8*math.ceil(required/8), metrics=metrics))
    return plan


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    path = Path(__file__).with_name('jackson-plan.json')
    plan = generate()
    if args.verify:
        stored = json.loads(path.read_text())
        for current, prior in zip(plan['cases'], stored['cases'], strict=True):
            for key, metric in current['metrics'].items():
                assert math.isclose(metric['pilot_sd'], prior['metrics'][key]['pilot_sd'], rel_tol=1e-10, abs_tol=1e-12)
                metric['pilot_sd'] = prior['metrics'][key]['pilot_sd']
        assert plan == stored, 'Jackson plan changed; investigate before updating gates'
    else:
        path.write_text(json.dumps(plan, indent=2)+'\n')
    print('Jackson plan:', ', '.join(f"{c['name']}={c['replications']} replications" for c in plan['cases']))


if __name__ == '__main__':
    main()
