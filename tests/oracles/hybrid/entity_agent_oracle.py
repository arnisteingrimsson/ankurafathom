"""Independent FIFO multi-server/process + statechart owner scheduler."""
import argparse
import copy
from fractions import Fraction as F
import hashlib
import itertools
import json
import math
from pathlib import Path

PLAN = Path(__file__).with_name('entity-agent-plan.json')


def require(ok, why):
    if not ok:
        raise ValueError(why)


def make_plan():
    cases = []
    for c in range(6):
        initial = [[10*i, [1, 2, 4][(i+c) % 3]/8] for i in range(3+c % 3)]
        births = [dict(time=t, work=50+10*i+c, duration=[.125, .375][(c+i) % 2])
                  for i, t in enumerate([.5, 1.125])]
        cases.append(dict(id=f'case_{c}', initial=initial, births=births, capacity=1+c % 3,
                          ready_delay=[.125, .25, .375][c % 3], retire=bool(c % 2)))
    return dict(version=1, cases=cases, orders=list(map(list, itertools.permutations(range(5)))),
                horizons=[k/8 for k in range(25)], tolerance=1e-13)


def reference(case, horizon):
    horizon, lag = F(horizon), F(case['ready_delay'])
    rows, timers, active, waiting, snapshots = [], {}, {}, [], {}
    def spawn(work, duration, t):
        agent = len(rows)
        rows.append(dict(id=agent, state=0, entered=t, generation=0, work=F(work), duration=F(duration), alive=True, process=0))
        timers.setdefault(t+lag, []).append((agent, 'ready'))
    for work, duration in case['initial']:
        spawn(work, duration, F(0))
    births = {F(e['time']): e for e in case['births'] if e['time'] <= horizon}
    clock = F(0)
    while True:
        due = [t for t in list(timers)+list(births)+list(active.values()) if t <= horizon]
        if not due:
            break
        t = min(due)
        ready = []
        timer_commit = False
        for agent, kind in sorted(timers.pop(t, [])):
            row = rows[agent]
            if not row['alive']:
                continue
            timer_commit = True  # Stale chart timers still commit a population timestamp.
            if kind == 'ready' and row['state'] == 0:
                row.update(state=1, entered=t, generation=1, work=row['work']+1)
                timers.setdefault(t+F(1, 4), []).append((agent, 'progress'))
                ready.append(agent)
            elif kind == 'progress' and row['state'] == 1:
                row.update(state=2, entered=t, generation=2, work=row['work']+10)
        if timer_commit:
            clock = t
        if t in births:
            e = births.pop(t); spawn(e['work'], e['duration'], t); clock = t
        for agent in sorted(a for a, finish in active.items() if finish == t):
            row = rows[agent]
            require(row['alive'] and row['process'] == 1, 'invalid reference ownership')
            row['process'] = 2
            if case['retire'] and agent % 3 == 1:
                row['alive'] = False
            else:
                row.update(state=3, entered=t, generation=row['generation']+1, work=row['work']+100)
            del active[agent]; clock = t
        for agent in ready:
            row = rows[agent]
            row['process'] = 1
            snapshots[agent] = dict(time=t, work=row['work'], duration=row['duration'])
            waiting.append(agent)
        while waiting and len(active) < case['capacity']:
            agent = waiting.pop(0)
            active[agent] = t+snapshots[agent]['duration']
    data = []
    for row in rows:
        snapshot = snapshots.get(row['id'])
        data.append([row['id'], row['state'], float(row['entered']), row['generation'], float(row['work']),
                     float(row['duration']), row['alive'], row['process'],
                     None if snapshot is None else [float(snapshot[x]) for x in ('time', 'work', 'duration')]])
    return dict(time=float(clock), rows=data, allocated=len(active), waiting=len(waiting),
                launched=len(snapshots), returned=sum(r['process'] == 2 for r in rows))


def score(rows, plan):
    refs = {(c['id'], h): reference(c, h) for c in plan['cases'] for h in plan['horizons']}
    expected = {(c, tuple(o), h): ref for (c, h), ref in refs.items() for o in plan['orders']}
    require(len(rows) == len(expected), 'row count')
    seen, comparisons = set(), 0
    for row in rows:
        require(set(row) == {'case', 'order', 'horizon', 'time', 'rows', 'allocated', 'waiting', 'launched', 'returned'}, 'row schema')
        key = (row['case'], tuple(row['order']), row['horizon'])
        require(key in expected and key not in seen, 'unknown/duplicate row')
        seen.add(key); ref = expected[key]
        for agent in row['rows']:
            require(isinstance(agent, list) and len(agent) == 9, 'agent row width')
            require(all(type(agent[i]) is int for i in (0, 1, 3, 7)) and type(agent[6]) is bool, 'identity/state/liveness types')
            require(all(type(agent[i]) in (int, float) and math.isfinite(agent[i]) for i in (2, 4, 5)), 'agent numeric types')
            if agent[8] is not None:
                require(isinstance(agent[8], list) and len(agent[8]) == 3 and
                        all(type(x) in (int, float) and math.isfinite(x) for x in agent[8]), 'dispatch snapshot shape')
        for name in ('time', 'rows', 'allocated', 'waiting', 'launched', 'returned'):
            require(row[name] == ref[name], f'{name} mismatch at {key}: actual={row[name]} expected={ref[name]}')
        comparisons += len(row['rows'])*8+sum(3 for r in row['rows'] if r[8] is not None)
    return dict(cases=len(plan['cases']), configurations=len(plan['cases'])*len(plan['orders']),
                snapshots=len(rows), agent_comparisons=comparisons, max_absolute_gap=0)


def contract(plan):
    rows = [dict(case=c['id'], order=o, horizon=h, **reference(c, h)) for c in plan['cases']
            for o in plan['orders'] for h in plan['horizons']]
    score(rows, plan)
    def reject(sample):
        try:
            score(sample, plan)
        except ValueError:
            return
        raise ValueError('corrupt entity-agent evidence accepted')
    reject(rows[:-1]); reject(rows[:-1]+[rows[0]])
    controls = 2
    for name in ('time', 'allocated', 'waiting', 'launched', 'returned'):
        sample = copy.deepcopy(rows); sample[10][name] += 1; reject(sample); controls += 1
    for i in range(8):
        sample = copy.deepcopy(rows)
        sample[10]['rows'][0][i] = not sample[10]['rows'][0][i] if i == 6 else sample[10]['rows'][0][i]+1
        reject(sample); controls += 1
    for i in range(3):
        sample = copy.deepcopy(rows); sample[10]['rows'][0][8][i] += 1; reject(sample); controls += 1
    sample = copy.deepcopy(rows); sample[10]['rows'].pop(); reject(sample); controls += 1
    sample = copy.deepcopy(rows); sample[10]['rows'][0][6] = int(sample[10]['rows'][0][6]); reject(sample); controls += 1
    sample = copy.deepcopy(rows); sample[10]['rows'][0][0] = False; reject(sample); controls += 1
    print(f'Entity-agent oracle: {controls} negative controls rejected')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--write-plan', action='store_true'); p.add_argument('--verify', action='store_true')
    p.add_argument('--contract', action='store_true'); p.add_argument('--native', type=Path); p.add_argument('--report', type=Path)
    args = p.parse_args(); canonical = json.dumps(make_plan(), indent=2)+'\n'
    if args.write_plan:
        PLAN.write_text(canonical)
    require(PLAN.read_text() == canonical, 'frozen entity-agent plan changed')
    plan = json.loads(canonical)
    if args.verify:
        print('6-case entity-agent plan verified')
    if args.contract:
        contract(plan)
    if args.native:
        require(args.report is not None, '--report required')
        report = score([json.loads(line) for line in args.native.read_text().splitlines()], plan)
        report.update(plan_sha256=hashlib.sha256(PLAN.read_bytes()).hexdigest(), native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest())
        args.report.write_text(json.dumps(report, indent=2)+'\n'); print(json.dumps(report))


if __name__ == '__main__':
    main()
