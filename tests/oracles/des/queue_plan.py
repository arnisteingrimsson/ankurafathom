"""Freeze analytical targets and replication power planning before native execution.

Independent pilot uses Python Random and an absolute-deadline heap, never Fathom.
Only standard-library dependencies; --verify regenerates and checks the artifact.
"""
import argparse
from collections import deque
from fractions import Fraction as F
import heapq
import json
import math
from pathlib import Path
import random
import statistics

WARMUP, WINDOW, PILOTS = 200, 1000, 64
CASES = [
    ('md1', 1, None, '0.5', 'deterministic', F(1)),
    ('me2_1', 1, None, '0.5', 'erlang2', F(3, 2)),
    ('mixed_mg1', 1, None, '0.5', 'two_point', F(13, 4)),
    ('mm1_k1', 1, 0, '0.7', 'exponential', F(2)),
    ('mm1_k4_balanced', 1, 3, '1', 'exponential', F(2)),
    ('mm1_k3_overloaded', 1, 2, '2', 'exponential', F(2)),
    ('mm2_k2_loss', 2, 0, '3', 'exponential', F(2)),
    ('mm2_k5_overloaded', 2, 3, '2.4', 'exponential', F(2)),
]


def targets(c, q, rate, second):
    if q is None:
        wait = rate * second / (2 * (1 - rate))  # E[S] = 1
        return dict(queue=rate*wait, wait=wait, utilization=rate, throughput=rate, blocking=F(0))
    k = c + q
    weights = [F(1)]
    for n in range(1, k+1):
        weights.append(weights[-1] * rate / min(n, c))  # mu = 1
    probabilities = [w/sum(weights) for w in weights]
    queue = sum(max(0, n-c)*p for n, p in enumerate(probabilities))
    busy = sum(min(n, c)*p for n, p in enumerate(probabilities))
    admitted = rate*(1-probabilities[-1])
    assert admitted == busy  # Exact rate balance, including rho == 1.
    result = dict(queue=queue, wait=queue/admitted, utilization=busy/c,
                  throughput=admitted, blocking=probabilities[-1])
    result.update({f'p{n}': p for n, p in enumerate(probabilities)})
    return result


def tolerance(metric, expected):
    if metric == 'utilization': return 0.035
    if metric == 'throughput': return 0.015 + 0.05*expected
    if metric == 'blocking': return 0.02 if expected else 0.0
    if metric.startswith('p'): return 0.035
    if expected == 0: return 0.0
    return (0.04 if metric == 'queue' else 0.06) + 0.20*expected


def pilot(c, q, rate, service, seed):
    arrivals_rng = random.Random(seed)
    service_rng = random.Random(seed+9000000)
    def exponential(rng, rate): return -math.log1p(-rng.random())/rate
    def duration():
        if service == 'deterministic': return 1.0
        if service == 'erlang2': return exponential(service_rng, 2)+exponential(service_rng, 2)
        if service == 'two_point': return 0.25 if service_rng.random() < 0.8 else 4.0
        return exponential(service_rng, 1)
    end = WARMUP + WINDOW
    arrival = exponential(arrivals_rng, rate)
    active, queue = [], deque()
    areas = [0.0]*(c+q+1) if q is not None else []
    t = queue_area = busy_area = wait = 0.0
    offers = blocked = departures = cohort = 0
    while active or arrival <= end:
        event = min(arrival, active[0][0] if active else math.inf)
        elapsed = max(0, min(event, end)-max(t, WARMUP))
        queue_area += elapsed*len(queue)
        busy_area += elapsed*len(active)
        if areas: areas[len(queue)+len(active)] += elapsed
        t = event
        if active and active[0][0] == event:
            heapq.heappop(active)
            if WARMUP < t <= end: departures += 1
        else:
            work = duration()
            measured = WARMUP < t <= end
            if measured: offers += 1
            if q is not None and len(active)+len(queue) == c+q:
                if measured: blocked += 1
            else:
                queue.append((t, work, measured))
            arrival += exponential(arrivals_rng, rate)
            if arrival > end: arrival = math.inf
        while queue and len(active) < c:
            born, work, measured = queue.popleft()
            if measured:
                wait += t-born
                cohort += 1
            heapq.heappush(active, (t+work, born))
    if areas: areas[0] += max(0, end-max(t, WARMUP))
    result = dict(queue=queue_area/WINDOW, utilization=busy_area/(c*WINDOW),
                  wait=wait/cohort, throughput=departures/WINDOW, blocking=blocked/offers)
    result.update({f'p{n}': area/WINDOW for n, area in enumerate(areas)})
    return result


def generate():
    result = dict(version=1, warmup=WARMUP, window=WINDOW, native_seed=2026092401,
                  numerical_epsilon=2e-9,
                  pilot_replications=PILOTS, pilot_seed_base=701001,
                  sigma_inflation=1.25, false_alarm_z=4.0, power_z=1.645, cases=[])
    for index, (name, c, q, rate, service, second) in enumerate(CASES):
        exact = targets(c, q, F(rate), second)
        samples = [pilot(c, q, float(rate), service, 701001+1000*index+r) for r in range(PILOTS)]
        metrics = {}
        required = 16
        for metric, target in exact.items():
            sigma = statistics.stdev(s[metric] for s in samples)
            gate = tolerance(metric, float(target))
            n = math.ceil(((4+1.645)*1.25*sigma/gate)**2) if gate else 0
            required = max(required, n)
            metrics[metric] = dict(target=float(target), exact=str(target), tolerance=gate,
                                   pilot_sd=float(f'{sigma:.12g}'), required_replications=n)
        # Whole groups of eight; no native outcomes enter this calculation.
        replications = 8*math.ceil(required/8)
        result['cases'].append(dict(name=name, scenario=index+20, servers=c, queue_capacity=q,
                                   arrival_rate=float(rate), service=service,
                                   service_mean=1, service_second_moment=float(second),
                                   replications=replications, metrics=metrics))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    path = Path(__file__).with_name('queue-plan.json')
    plan = generate()
    if args.verify:
        stored = json.loads(path.read_text())
        # Pilots are diagnostics; tolerate platform libm differences in stored SDs.
        for current, prior in zip(plan['cases'], stored['cases'], strict=True):
            for key, metric in current['metrics'].items():
                assert math.isclose(metric['pilot_sd'], prior['metrics'][key]['pilot_sd'], rel_tol=1e-10, abs_tol=1e-12)
                metric['pilot_sd'] = prior['metrics'][key]['pilot_sd']
        assert plan == stored, 'queue plan changed; investigate before updating acceptance gates'
    else:
        path.write_text(json.dumps(plan, indent=2)+'\n')
    print('Queue plan:', ', '.join(f"{c['name']}={c['replications']} replications" for c in plan['cases']))


if __name__ == '__main__':
    main()
