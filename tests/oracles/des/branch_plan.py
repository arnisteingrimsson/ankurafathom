"""Independent max-plus pilot for a three-station Bernoulli branching network."""
import argparse
from fractions import Fraction as F
import itertools
import json
import math
from pathlib import Path
import random
import statistics

WARMUP, WINDOW, PILOTS = 200, 1000, 64
ARRIVAL = F(3, 5)
RATES = [F(1), F(4, 5), F(6, 5)]
CASES = [('branch_35', F(7, 20)), ('branch_65', F(13, 20))]


def targets(probability):
    traffic = [ARRIVAL, ARRIVAL*probability, ARRIVAL*(1-probability)]
    result, probabilities = {}, []
    for i, (arrival, service) in enumerate(zip(traffic, RATES, strict=True)):
        rho = arrival/service
        wait = rho/(service-arrival)
        result.update({f's{i}_queue': arrival*wait, f's{i}_wait': wait,
                       f's{i}_utilization': rho, f's{i}_throughput': arrival})
        probabilities.append([1-rho, (1-rho)*rho, rho*rho])
    result['cycle'] = 1/(RATES[0]-ARRIVAL)+probability/(RATES[1]-traffic[1])+(1-probability)/(RATES[2]-traffic[2])
    result['match_fraction'] = probability
    for cell in itertools.product(range(3), repeat=3):
        result['p'+''.join(map(str, cell))] = math.prod(probabilities[i][n] for i, n in enumerate(cell))
    assert sum(v for k, v in result.items() if k.startswith('p')) == 1
    return result


def tolerance(key, expected):
    if key.startswith('p') or key == 'match_fraction': return .025
    if key.endswith('_utilization'): return .035
    if key.endswith('_throughput'): return .015+.05*expected
    if key.endswith('_queue'): return .04+.20*expected
    if key.endswith('_wait'): return .06+.20*expected
    return .15+.15*expected


def pilot(probability, seed):
    arrival_rng, route_rng = random.Random(seed), random.Random(seed+8000000)
    service_rng = [random.Random(seed+9000000+100000*i) for i in range(3)]
    def exponential(rng, rate): return -math.log1p(-rng.random())/float(rate)
    end, finish = WARMUP+WINDOW, [0.0]*3
    result = {key: 0.0 for key in targets(probability)}
    cohort, cycles, routing, matched = [0]*3, 0, 0, 0
    events = [(0.0, 0, 0), (float(end), 0, 0)]
    born = exponential(arrival_rng, ARRIVAL)
    while born <= end:
        chosen = 1 if route_rng.random() < float(probability) else 2
        entry = born
        for i in [0, chosen]:
            start = max(entry, finish[i])
            finish[i] = start+exponential(service_rng[i], RATES[i])
            events.extend([(entry, i, 1), (finish[i], i, -1)])
            if WARMUP < entry <= end:
                result[f's{i}_wait'] += start-entry
                cohort[i] += 1
            if WARMUP < finish[i] <= end:
                result[f's{i}_throughput'] += 1/WINDOW
                if i == 0:
                    routing += 1
                    matched += chosen == 1
            entry = finish[i]
        if WARMUP < born <= end:
            result['cycle'] += entry-born
            cycles += 1
        born += exponential(arrival_rng, ARRIVAL)
    state, previous = [0]*3, 0
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
    result['match_fraction'] = matched/routing
    return result


def generate():
    plan = dict(version=1, warmup=WARMUP, window=WINDOW, native_seed=2026092501,
                arrival_rate=float(ARRIVAL), arrival_stream=400, service_streams=[401, 402, 403], routing_stream=404,
                pilot_replications=PILOTS, pilot_seed_base=1031001,
                sigma_inflation=1.25, false_alarm_z=4.0, power_z=1.645, cases=[])
    for index, (name, probability) in enumerate(CASES):
        samples = [pilot(probability, 1031001+1000*index+r) for r in range(PILOTS)]
        required, metrics = 16, {}
        for key, target in targets(probability).items():
            sigma = statistics.stdev(sample[key] for sample in samples)
            gate = tolerance(key, float(target))
            count = math.ceil(((4+1.645)*1.25*sigma/gate)**2)
            required = max(required, count)
            metrics[key] = dict(target=float(target), exact=str(target), tolerance=gate,
                                pilot_sd=float(f'{sigma:.12g}'), required_replications=count)
        plan['cases'].append(dict(name=name, scenario=40+index, service_rates=list(map(float, RATES)),
                                  branch_probability=float(probability), replications=8*math.ceil(required/8), metrics=metrics))
    return plan


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    path = Path(__file__).with_name('branch-plan.json')
    plan = generate()
    if args.verify:
        stored = json.loads(path.read_text())
        for current, prior in zip(plan['cases'], stored['cases'], strict=True):
            for key, metric in current['metrics'].items():
                assert math.isclose(metric['pilot_sd'], prior['metrics'][key]['pilot_sd'], rel_tol=1e-10, abs_tol=1e-12)
                metric['pilot_sd'] = prior['metrics'][key]['pilot_sd']
        assert plan == stored, 'branch plan changed; investigate before updating gates'
    else:
        path.write_text(json.dumps(plan, indent=2)+'\n')
    print('Branch plan:', ', '.join(f"{c['name']}={c['replications']} replications" for c in plan['cases']))
