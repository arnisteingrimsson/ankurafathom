"""Independent membership/timer scheduler for DES-to-population lifecycle coupling."""
import argparse
import copy
from fractions import Fraction as F
import hashlib
import itertools
import json
from pathlib import Path

PLAN = Path(__file__).with_name('lifecycle-bridge-plan.json')


def require(ok, why):
    if not ok:
        raise ValueError(why)


def make_plan():
    cases = []
    for c in range(8):
        initial = [[1+c, .25, -1], [3+c, .5, -2], [5+c, .75, -3]]
        events = []
        for i, t in enumerate([.125, .25, .5, .75, 1, 1.25, 1.5, 2]):
            retirements = {2: [0], 3: [1], 4: [2], 6: [3]}.get(i, [])
            births = [[2+c+j+i, (1+(c+j+i) % 4)/4, i*10+j] for j in range(1+(c+i) % 2)]
            if i in (3, 6):
                births = []
            events.append(dict(time=t, key=100+i, retirements=retirements, births=births))
        cases.append(dict(id=f'case_{c:02d}', dt=[.25, .5, 1][c % 3], initial=initial, events=events))
    return dict(version=1, cases=cases, modes=['sync', 'async'],
                orders=list(map(list, itertools.permutations(range(4)))),
                horizons=[k/8 for k in range(21)], tolerance=1e-13)


def reference(case, mode, horizon):
    horizon, dt = F(horizon), F(case['dt'])
    records = [[i, F(r[0]), F(r[1]), r[2], True] for i, r in enumerate(case['initial'])]
    events = {F(e['time']): e for e in case['events'] if e['time'] <= horizon}
    timers = {0: [F(1, 2)], 1: [F(7, 8)], 2: [F(5, 8)]} if mode == 'async' else {}
    times = set(events)
    if mode == 'sync':
        times.update(k*dt for k in range(1, int(horizon/dt)+1))
    else:
        times.update(t for due in timers.values() for t in due if t <= horizon)
    clock, completed, revision = F(0), 0, 0
    while times:
        t = min(times); times.remove(t)
        changed = False
        if mode == 'sync' and t % dt == 0:
            for row in records:
                if row[4]:
                    row[1] += row[2]
            changed = True
        if mode == 'async':
            for row in records:
                if row[4] and t in timers.get(row[0], []):
                    row[1] += 2*row[2]
                    changed = True
        if changed:
            clock = t
            revision += 1
        if t in events:
            e = events[t]
            for agent in e['retirements']:
                require(records[agent][4], 'plan retires inactive agent')
                records[agent][4] = False
                timers.pop(agent, None)
            for work, rate, origin in e['births']:
                agent = len(records)
                records.append([agent, F(work), F(rate), origin, True])
                if mode == 'async':
                    timers[agent] = [t+F(1, 4), t+F(7, 4)]
                    times.update(due for due in timers[agent] if due <= horizon)
            completed += 1
            clock = t
            revision += 1
    edges = [[a, b] for a, b in [(0, 1), (0, 2), (1, 2)] if records[a][4] and records[b][4]]
    return dict(time=float(clock), revision=revision, completed=completed,
                rows=[[i, float(w), float(r), origin, alive] for i, w, r, origin, alive in records], edges=edges)


def score(rows, plan):
    references = {(c['id'], mode, h): reference(c, mode, h) for c in plan['cases']
                  for mode in plan['modes'] for h in plan['horizons']}
    expected = {(c, m, tuple(o), h): ref for (c, m, h), ref in references.items() for o in plan['orders']}
    require(len(rows) == len(expected), 'row count')
    seen, values, maximum = set(), 0, 0.
    for row in rows:
        require(set(row) == {'case', 'mode', 'order', 'horizon', 'time', 'revision', 'completed', 'rows', 'edges'}, 'row schema')
        key = (row['case'], row['mode'], tuple(row['order']), row['horizon'])
        require(key in expected and key not in seen, 'unknown/duplicate row')
        seen.add(key); ref = expected[key]
        for name in ('time', 'revision', 'completed', 'edges'):
            require(row[name] == ref[name], f'{name} mismatch at {key}')
        require(len(row['rows']) == len(ref['rows']), 'allocation mismatch')
        for actual, target in zip(row['rows'], ref['rows']):
            require(len(actual) == 5 and actual[0] == target[0] and actual[3:] == target[3:], 'identity/liveness mismatch')
            for a, b in zip(actual[1:3], target[1:3]):
                require(type(a) in (int, float) and abs(a-b) <= plan['tolerance']*max(1., abs(b)), 'agent value mismatch')
                maximum = max(maximum, abs(a-b)); values += 1
    return dict(cases=len(plan['cases']), configurations=len(plan['cases'])*2*len(plan['orders']),
                snapshots=len(rows), agent_scalar_comparisons=values, max_absolute_gap=maximum)


def contract(plan):
    rows = [dict(case=c['id'], mode=m, order=o, horizon=h, **reference(c, m, h))
            for c in plan['cases'] for m in plan['modes'] for o in plan['orders'] for h in plan['horizons']]
    score(rows, plan)
    bad = [rows[:-1], rows[:-1]+[rows[0]]]
    for name in ('time', 'revision', 'completed'):
        sample = copy.deepcopy(rows); sample[10][name] += 1; bad.append(sample)
    for i in range(4):
        sample = copy.deepcopy(rows); sample[10]['rows'][0][i] += 1; bad.append(sample)
    sample = copy.deepcopy(rows); sample[10]['rows'][0][4] = True; bad.append(sample)
    sample = copy.deepcopy(rows); sample[10]['rows'][1][1] = float('nan'); bad.append(sample)
    sample = copy.deepcopy(rows); sample[10]['rows'].pop(); bad.append(sample)
    sample = copy.deepcopy(rows); sample[10]['edges'].append([0, 1]); bad.append(sample)
    for sample in bad:
        try:
            score(sample, plan)
        except ValueError:
            continue
        raise ValueError('corrupt lifecycle evidence accepted')
    print(f'Lifecycle oracle: {len(bad)} negative controls rejected')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--write-plan', action='store_true'); p.add_argument('--verify', action='store_true')
    p.add_argument('--contract', action='store_true'); p.add_argument('--native', type=Path); p.add_argument('--report', type=Path)
    args = p.parse_args(); canonical = json.dumps(make_plan(), indent=2)+'\n'
    if args.write_plan:
        PLAN.write_text(canonical)
    require(PLAN.read_text() == canonical, 'frozen lifecycle plan changed')
    plan = json.loads(canonical)
    if args.verify:
        print('8-case lifecycle plan verified')
    if args.contract:
        contract(plan)
    if args.native:
        require(args.report is not None, '--report required')
        report = score([json.loads(line) for line in args.native.read_text().splitlines()], plan)
        report.update(plan_sha256=hashlib.sha256(PLAN.read_bytes()).hexdigest(), native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest())
        args.report.write_text(json.dumps(report, indent=2)+'\n'); print(json.dumps(report))


if __name__ == '__main__':
    main()
