"""Cumulative-hazard inversion and analytic Poisson checks for scalar-driven DES."""
import argparse
import copy
from fractions import Fraction as F
import hashlib
import itertools
import json
import math
from pathlib import Path
import statistics
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from des_typed_contract import philox_word

PLAN = Path(__file__).with_name('signal-rate-plan.json')


def require(ok, why):
    if not ok:
        raise ValueError(why)


def make_plan():
    cases = []
    definitions = [
        ('constant', 2, 0, .5, [], 256),
        ('ramp', .5, .5, .25, [], 256),
        ('pause_resume', 2, 0, .5, [[1, -2], [2, 1]], 256),
        ('off_grid', 1, .25, .5, [[.375, 1], [1.125, -1]], 256),
        ('zero', 0, 0, .5, [], 256),
        ('bounded', 4, 0, .5, [], 2),
        ('high_address', 1, 0, .5, [], 8),
    ]
    for i, (name, initial, slope, dt, pulses, limit) in enumerate(definitions):
        high = name == 'high_address'
        cases.append(dict(id=name, initial=initial, slope=slope, dt=dt, pulses=pulses, limit=limit,
                          first_id=(1 << 48)-8 if high else 0, seed=(1 << 64)-1 if high else 123,
                          scenario=65535 if high else i, replication_start=65504 if high else 0,
                          replications=32 if high else 256, source_stream=65534 if high else 7,
                          routing_stream=65535 if high else 8))
    return dict(version=1, cases=cases, orders=list(map(list, itertools.permutations(range(3)))),
                horizon=4, tolerance=2e-11, mean_standard_errors=6, variance_standard_errors=7)


def segments(case, horizon):
    dt, horizon = F(case['dt']), F(horizon)
    pulses = {F(t): F(amount) for t, amount in case['pulses']}
    times = sorted({F(0), horizon} | {k*dt for k in range(1, int(horizon/dt)+1)} | set(pulses))
    stock, previous = F(case['initial']), F(0)
    result = []
    for i, t in enumerate(times[:-1]):
        stock += F(case['slope'])*(t-previous)+pulses.get(t, 0)
        require(stock >= 0, 'plan violates nonnegative rate stock')
        result.append((float(t), float(times[i+1]), float(stock)))
        previous = t
    return result


def reference(case, replication, horizon):
    intervals = segments(case, horizon)
    target, arrivals = 0., []
    for identity in range(case['first_id'], case['first_id']+case['limit']):
        word = philox_word(case['seed'], case['scenario'], replication, identity, case['source_stream'])
        target += -math.log((word+.5)/(1 << 32))
        cumulative = 0.
        for start, end, rate in intervals:
            next_cumulative = cumulative+(end-start)*rate
            if rate and target <= next_cumulative:
                t = start+(target-cumulative)/rate
                probability = rate/(rate+1)
                u = (philox_word(case['seed'], case['scenario'], replication, identity, case['routing_stream'])+.5)/(1 << 32)
                branch = 1 if u < probability else 2 if u < probability+(1-probability)/4 else 3
                arrivals.append(dict(id=identity, time=t, branch=branch, rate=rate, probability=probability,
                                     group=bool(identity % 2), label=case['id']))
                break
            cumulative = next_cumulative
        else:
            break
    return arrivals


def moments(lam, limit):
    if lam == 0 or limit == 0:
        return 0., 0., 0.
    terms, p = [], math.exp(-lam)
    k = 0
    while k < limit or p > 1e-20:
        terms.append((min(k, limit), p))
        k += 1; p *= lam/k
    mean = math.fsum(x*p for x, p in terms)
    variance = math.fsum((x-mean)**2*p for x, p in terms)
    fourth = math.fsum((x-mean)**4*p for x, p in terms)
    return mean, variance, fourth


def score(rows, plan):
    expected = {(c['id'], r, tuple(o)): reference(c, r, plan['horizon']) for c in plan['cases']
                for r in range(c['replication_start'], c['replication_start']+c['replications']) for o in plan['orders']}
    require(len(rows) == len(expected), 'row count')
    seen, maximum, events = set(), 0., 0
    counts = {c['id']: [] for c in plan['cases']}
    for row in rows:
        require(set(row) == {'case', 'replication', 'order', 'arrivals', 'generated', 'routed'}, 'row schema')
        key = (row['case'], row['replication'], tuple(row['order']))
        require(key in expected and key not in seen, 'unknown/duplicate row')
        seen.add(key); ref = expected[key]
        require(type(row['generated']) is int and row['generated'] == len(ref) == row['routed'] == len(row['arrivals']), 'arrival accounting')
        for actual, target in zip(row['arrivals'], ref):
            require(set(actual) == set(target), 'event schema')
            for name in ('id', 'branch', 'group', 'label'):
                require(type(actual[name]) is type(target[name]) and actual[name] == target[name], f'{name} mismatch at {key}')
            for name in ('time', 'rate', 'probability'):
                require(type(actual[name]) in (int, float) and math.isfinite(actual[name]) and
                        abs(actual[name]-target[name]) <= plan['tolerance']*max(1., abs(target[name])), f'{name} mismatch at {key}')
                maximum = max(maximum, abs(actual[name]-target[name]))
            events += 1
        if row['order'] == plan['orders'][0]:
            counts[row['case']].append(row['generated'])
    gates = []
    for case in plan['cases']:
        values = counts[case['id']]; n = len(values)
        lam = math.fsum((b-a)*rate for a, b, rate in segments(case, plan['horizon']))
        mean, variance, fourth = moments(lam, case['limit'])
        observed_mean, observed_variance = statistics.mean(values), statistics.variance(values)
        mean_bound = plan['mean_standard_errors']*math.sqrt(variance/n)+plan['tolerance']
        variance_bound = plan['variance_standard_errors']*math.sqrt(max(0., (fourth-(n-3)/(n-1)*variance**2)/n))+plan['tolerance']
        require(abs(observed_mean-mean) <= mean_bound, f'Poisson mean gate: {case["id"]}')
        require(abs(observed_variance-variance) <= variance_bound, f'Poisson variance gate: {case["id"]}')
        gates.append(dict(case=case['id'], replications=n, integrated_rate=lam, expected_mean=mean,
                          observed_mean=observed_mean, mean_bound=mean_bound, expected_variance=variance,
                          observed_variance=observed_variance, variance_bound=variance_bound))
    return dict(configurations=len(rows), exact_event_comparisons=events, max_absolute_gap=maximum,
                poisson_gates=len(gates)*2, count_checks=gates)


def contract(plan):
    rows = [dict(case=c['id'], replication=r, order=o, arrivals=reference(c, r, plan['horizon']),
                 generated=len(reference(c, r, plan['horizon'])), routed=len(reference(c, r, plan['horizon'])))
            for c in plan['cases'] for r in range(c['replication_start'], c['replication_start']+c['replications']) for o in plan['orders']]
    score(rows, plan)
    def reject(sample):
        try:
            score(sample, plan)
        except ValueError:
            return
        raise ValueError('corrupt rate evidence accepted')
    reject(rows[:-1]); reject(rows[:-1]+[rows[0]]); controls = 2
    for name in ('generated', 'routed'):
        sample = copy.deepcopy(rows); sample[0][name] += 1; reject(sample); controls += 1
    for name in ('id', 'time', 'branch', 'rate', 'probability'):
        sample = copy.deepcopy(rows); sample[0]['arrivals'][0][name] += 1; reject(sample); controls += 1
    sample = copy.deepcopy(rows); sample[0]['arrivals'][0]['group'] = 0; reject(sample); controls += 1
    sample = copy.deepcopy(rows); sample[0]['arrivals'][0]['label'] = 'wrong'; reject(sample); controls += 1
    sample = copy.deepcopy(rows); sample[0]['arrivals'][0]['time'] = float('nan'); reject(sample); controls += 1
    sample = copy.deepcopy(rows); sample[0]['arrivals'].pop(); reject(sample); controls += 1
    print(f'Signal rate oracle: {controls} negative controls rejected')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--write-plan', action='store_true'); p.add_argument('--verify', action='store_true')
    p.add_argument('--contract', action='store_true'); p.add_argument('--native', type=Path); p.add_argument('--report', type=Path)
    args = p.parse_args(); canonical = json.dumps(make_plan(), indent=2)+'\n'
    if args.write_plan:
        PLAN.write_text(canonical)
    require(PLAN.read_text() == canonical, 'frozen rate plan changed')
    plan = json.loads(canonical)
    if args.verify:
        print('7-case scalar-driven rate plan verified')
    if args.contract:
        contract(plan)
    if args.native:
        require(args.report is not None, '--report required')
        report = score([json.loads(line) for line in args.native.read_text().splitlines()], plan)
        report.update(plan_sha256=hashlib.sha256(PLAN.read_bytes()).hexdigest(), native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest())
        args.report.write_text(json.dumps(report, indent=2)+'\n'); print(json.dumps(report))


if __name__ == '__main__':
    main()
