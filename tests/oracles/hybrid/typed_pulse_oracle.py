"""Independent rational event-quantity and sampled-flow oracle for typed pulses."""
import argparse
import copy
from fractions import Fraction as F
import hashlib
import itertools
import json
import math
from pathlib import Path

PLAN = Path(__file__).with_name('typed-pulse-plan.json')


def require(ok, why):
    if not ok:
        raise ValueError(why)


def make_plan():
    cases = []
    for c in range(12):
        events = []
        for i, t in enumerate([0, .25, .375, .5, 1.125, 1.5, 2.25]):
            for channel in range(2):
                if (c+i+channel) % 4 == 0:
                    continue
                for j in range(1+(i+c+channel) % 3):
                    events.append(dict(time=t, channel=channel, key=(1 << 63)+i*16+j,
                                       quantity=1+(c+j+i) % 3, price=1+(c+2*j) % 4))
        signals = [dict(time=t, revision=3*i, value=(1+(c+i) % 4)/32)
                   for i, t in enumerate([0, .375, .5, 1.125, 1.5, 2.25])]
        cases.append(dict(id=f'case_{c:02d}', dt=[.25, .5, 1][c % 3],
                          initial=[100+2*c, 200+3*c, 0], events=events, signals=signals))
    return dict(version=1, cases=cases, orders=list(map(list, itertools.permutations(range(3)))),
                input_orders=[0, 1], horizons=[k/8 for k in range(21)], tolerance=2e-12)


def reference(case, horizon):
    horizon, dt = F(horizon), F(case['dt'])
    by_time = {}
    for event in case['events']:
        if event['time'] <= horizon:
            by_time.setdefault(F(event['time']), []).append(event)
    signals = {F(s['time']): s for s in case['signals'] if s['time'] <= horizon}
    times = sorted(set(by_time) | set(signals) | {k*dt for k in range(1, int(horizon/dt)+1)})
    state, held, time = list(map(F, case['initial'])), F(1, 8), F(0)
    counts, revisions, scalar_revision = [0, 0], [None, None], None
    for t in times:
        elapsed = t-time
        transfer, fees = state[0]*held*elapsed, state[1]*F(1, 32)*elapsed
        state = [state[0]-transfer, state[1]+transfer-fees, state[2]+fees]
        time = t
        if t in signals:
            held = F(signals[t]['value'])
            scalar_revision = signals[t]['revision']
        batches = {0: [], 1: []}
        for event in by_time.get(t, []):
            batches[event['channel']].append(event)
        for channel, events in batches.items():
            if events:
                revisions[channel] = 0 if revisions[channel] is None else revisions[channel]+1
            for event in events:
                counts[channel] += 1
                q, p = F(event['quantity']), F(event['price'])
                sign = 1 if channel == 1 else -1
                state[0] += sign*q
                state[1] -= sign*q*p
        require(all(x >= 0 for x in state), 'plan violates stock domain')
    return dict(time=float(time), stocks=list(map(float, state)), signal=float(held),
                scalar_revision=scalar_revision, revisions=revisions, counts=counts)


def score(rows, plan):
    expected = {(c['id'], tuple(o), io, h): reference(c, h) for c in plan['cases']
                for o in plan['orders'] for io in plan['input_orders'] for h in plan['horizons']}
    require(len(rows) == len(expected), 'row count')
    seen, maximum = set(), 0.
    for row in rows:
        require(set(row) == {'case', 'order', 'input_order', 'horizon', 'time', 'stocks', 'signal',
                             'scalar_revision', 'revisions', 'counts'}, 'row schema')
        key = (row['case'], tuple(row['order']), row['input_order'], row['horizon'])
        require(key in expected and key not in seen, 'duplicate or unknown row')
        seen.add(key)
        ref = expected[key]
        for name in ('time', 'scalar_revision', 'revisions', 'counts'):
            require(row[name] == ref[name], f'{name} mismatch')
        require(len(row['stocks']) == 3, 'stock width')
        for actual, target in zip(row['stocks']+[row['signal']], ref['stocks']+[ref['signal']]):
            require(type(actual) in (int, float) and math.isfinite(actual) and
                    abs(actual-target) <= plan['tolerance']*max(1., abs(target)), 'stock/signal mismatch')
            maximum = max(maximum, abs(actual-target))
    return dict(cases=len(plan['cases']), configurations=len(plan['cases'])*len(plan['orders'])*2,
                snapshots=len(rows), scalar_comparisons=4*len(rows), max_absolute_gap=maximum)


def contract(plan):
    rows = [dict(case=c['id'], order=o, input_order=io, horizon=h, **reference(c, h))
            for c in plan['cases'] for o in plan['orders'] for io in plan['input_orders'] for h in plan['horizons']]
    score(rows, plan)
    bad = [rows[:-1], rows[:-1]+[rows[0]]]
    for name in ('stocks', 'revisions', 'counts'):
        for i in range(len(rows[10][name])):
            sample = copy.deepcopy(rows)
            sample[10][name][i] += 1
            bad.append(sample)
    for name in ('time', 'signal', 'scalar_revision'):
        sample = copy.deepcopy(rows); sample[10][name] += 1; bad.append(sample)
    sample = copy.deepcopy(rows); sample[10]['stocks'][0] = float('nan'); bad.append(sample)
    sample = copy.deepcopy(rows); sample[10]['stocks'].append(0); bad.append(sample)
    for sample in bad:
        try:
            score(sample, plan)
        except ValueError:
            continue
        raise ValueError('corrupt pulse evidence accepted')
    print(f'Typed pulse oracle: {len(bad)} negative controls rejected')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write-plan', action='store_true')
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--contract', action='store_true')
    parser.add_argument('--native', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    canonical = json.dumps(make_plan(), indent=2)+'\n'
    if args.write_plan:
        PLAN.write_text(canonical)
    require(PLAN.read_text() == canonical, 'frozen pulse plan changed')
    plan = json.loads(canonical)
    if args.verify:
        print('12-case typed pulse plan verified')
    if args.contract:
        contract(plan)
    if args.native:
        require(args.report is not None, '--report required')
        report = score([json.loads(line) for line in args.native.read_text().splitlines()], plan)
        report.update(plan_sha256=hashlib.sha256(PLAN.read_bytes()).hexdigest(),
                      native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest())
        args.report.write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report))


if __name__ == '__main__':
    main()
