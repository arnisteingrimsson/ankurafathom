"""Independent integer workforce/queue scheduler with rational time accounting."""
import argparse
import copy
from fractions import Fraction as F
import hashlib
import itertools
import json
import math
from pathlib import Path

PLAN = Path(__file__).with_name('typed-pool-plan.json')


def require(ok, why):
    if not ok:
        raise ValueError(why)


def make_plan():
    cases = []
    for discipline in ('fifo', 'lifo', 'priority'):
        for variant in range(3):
            initial = [[2, 1], [0, 0], [1, 2, 0]][variant]
            events = [
                dict(time=0, requests=[[10*i, 1+(i+variant)%3, (i%3)-1] for i in range(1, 7)]),
                dict(time=.125, phase=True),
                dict(time=.375, births=[[2+variant, True]]),
                dict(time=.5, release_all=True, updates=[[0, 0, False]], retire=[1]),
                dict(time=.625, requests=[[70+10*i, 1+i, -i] for i in range(3)]),
                dict(time=.875, phase=True),
                dict(time=1, release_all=True, retire_all=True, births=[] if variant==0 else [[0, False]]),
                dict(time=1.125, requests=[[100, 2, 0], [110, 1, -1]]),
                dict(time=1.25, births=[[1, True], [2, True]], phase=variant==2),
                dict(time=1.5, release_all=True),
                dict(time=1.75, births=[[3, True]]),
                dict(time=2, release_all=True, retire_all=True),
                dict(time=2.125, births=[[3, True]]),
                dict(time=2.375, release_all=True),
                dict(time=2.75, release_all=True),
            ]
            cases.append(dict(id=f'{discipline}_{variant}', discipline=discipline,
                              first=2**48-16 if variant==2 else 0, initial=initial, events=events))
    return dict(version=1, cases=cases, orders=list(map(list, itertools.permutations(range(4)))),
                bag_orders=['forward', 'reverse'], horizons=[i/16 for i in range(49)], tolerance=0.)


def reference(case, horizon):
    first = case['first']
    rows, assignments, waiting, notes, grants = [], {}, [], [], []
    areas = [F(0)]*4
    clock, commits = F(0), 0
    def birth(cap, enabled):
        rows.append([first+len(rows), cap, 0, enabled, 0, 0, True])
    for cap in case['initial']:
        birth(cap, True)
    def levels():
        return [sum(r[1] for r in rows if r[6] and r[3]), sum(r[2] for r in rows if r[6]),
                len(waiting), sum(r[6] for r in rows)]
    def allocation_fields():
        for r in rows:
            if r[6]:
                r[2] = 0
        for shares in assignments.values():
            for agent, units in shares:
                rows[agent-first][2] += units
    for e in case['events']:
        t = F(e['time'])
        if t > horizon:
            break
        for j, level in enumerate(levels()):
            areas[j] += (t-clock)*level
        clock = t; commits += 1; notes = []; grants = []
        if e.get('release_all'):
            for request, shares in sorted(assignments.items()):
                for agent, units in shares:
                    notes.append([request, agent, units, False])
                    rows[agent-first][5] += units
            assignments.clear(); allocation_fields()
        for offset, cap, enabled in e.get('updates', []):
            row = rows[offset]; require(row[6] and row[2] <= (cap if enabled else 0), 'invalid reference update')
            row[1], row[3] = cap, enabled
        retire = [i for i, r in enumerate(rows) if r[6]] if e.get('retire_all') else e.get('retire', [])
        for i in retire:
            require(rows[i][6] and rows[i][2]==0, 'busy reference retirement')
            rows[i][6] = False
        for cap, enabled in e.get('births', []):
            birth(cap, enabled)
        if e.get('phase'):
            enabled = sum(r[3] for r in rows if r[6])
            for r in rows:
                if r[6]:
                    r[1] += enabled-int(r[3])
        incoming = sorted(e.get('requests', []))
        if case['discipline']=='priority':
            incoming.sort(key=lambda r: (r[2], r[0]))
        waiting.extend(incoming)
        if case['discipline']=='priority':
            waiting.sort(key=lambda r: r[2])  # Stable existing-waiter priority order.
        available = levels()[0]-levels()[1]
        assigned = []
        while waiting:
            index = -1 if case['discipline']=='lifo' else 0
            request, units, _ = waiting[index]
            if units > available:
                break
            waiting.pop(index); available -= units; grants.append([request, units])
            shares, remaining = [], units
            for row in rows:
                if not row[6] or not row[3]:
                    continue
                take = min(remaining, row[1]-row[2])
                if take:
                    shares.append([row[0], take]); assigned.append([request, row[0], take, True])
                    row[2] += take; remaining -= take
                if not remaining:
                    break
            require(remaining==0, 'reference grant lacks agents'); assignments[request] = shares
        for note in sorted(assigned):
            rows[note[1]-first][4] += note[2]; notes.append(note)
    current = levels()
    projected = [float(a+(F(horizon)-clock)*n) for a, n in zip(areas, current)]
    sd_time = max(clock, F(math.floor(horizon*4), 4))
    sd = [float(areas[j]+(sd_time-clock)*current[j]) for j in (0, 1, 3)]
    return dict(time=float(clock), rows=rows, assignments=[[key, val] for key, val in sorted(assignments.items())],
                capacity=current[0], allocated=current[1], waiting=current[2], live=current[3],
                statistics=projected, sd_time=float(sd_time), sd=sd, signals=list(map(float, (current[0], current[1], current[3]))),
                revision=commits, notifications=notes, grants=grants, next_id=first+len(rows))


def compare(actual, expected, tolerance, path='value'):
    if isinstance(expected, dict):
        require(isinstance(actual, dict) and actual.keys()==expected.keys(), path+' keys')
        return sum(compare(actual[k], v, tolerance, path+'.'+k) for k, v in expected.items())
    if isinstance(expected, list):
        require(isinstance(actual, list) and len(actual)==len(expected), path+' length')
        return sum(compare(a, e, tolerance, path) for a, e in zip(actual, expected))
    if isinstance(expected, bool):
        require(type(actual) is bool and actual==expected, path+' bool')
    elif isinstance(expected, int):
        require(type(actual) is int and actual==expected, path+' integer')
    elif isinstance(expected, float):
        require(type(actual) in (int, float) and math.isfinite(actual) and abs(actual-expected)<=tolerance, path+' numeric')
    else:
        require(type(actual) is type(expected) and actual==expected, path+' exact')
    return 1


def score(rows, plan):
    expected = {(c['id'], tuple(o), b, h): reference(c, h)
                for c in plan['cases'] for o in plan['orders'] for b in plan['bag_orders'] for h in plan['horizons']}
    require(len(rows)==len(expected), 'row count')
    seen, checks = set(), 0
    for row in rows:
        require(set(row)=={'case', 'order', 'bag_order', 'horizon', 'state'}, 'row fields')
        key = row['case'], tuple(row['order']), row['bag_order'], row['horizon']
        require(key in expected and key not in seen, 'unknown or repeated case/order/horizon')
        seen.add(key); checks += compare(row['state'], expected[key], plan['tolerance'])
    return dict(passed=True, configurations=len(plan['cases'])*len(plan['orders'])*len(plan['bag_orders']),
                snapshots=len(rows), scalar_comparisons=checks, max_absolute_gap=0)


def contract(plan):
    small = dict(plan, cases=plan['cases'][:1], orders=plan['orders'][:1], bag_orders=['forward'], horizons=[0, .5, 1.25, 3])
    good = [dict(case=c['id'], order=o, bag_order=b, horizon=h, state=reference(c, h))
            for c in small['cases'] for o in small['orders'] for b in small['bag_orders'] for h in small['horizons']]
    score(good, small)
    mutations = []
    def corrupt(fn):
        bad = copy.deepcopy(good);fn(bad);mutations.append(bad)
    corrupt(lambda a: a.pop())
    corrupt(lambda a: a.append(copy.deepcopy(a[0])))
    corrupt(lambda a: a[0].update(case='unknown'))
    corrupt(lambda a: a[0]['state'].update(capacity=999))
    corrupt(lambda a: a[0]['state'].update(allocated=0))
    corrupt(lambda a: a[0]['state'].update(waiting=99))
    corrupt(lambda a: a[1]['state']['rows'][0].__setitem__(2, 1))
    corrupt(lambda a: a[1]['state']['rows'][0].__setitem__(6, 1))
    corrupt(lambda a: a[0]['state']['assignments'][0][1][0].__setitem__(1, 99))
    corrupt(lambda a: a[0]['state']['notifications'][0].__setitem__(0, 99))
    corrupt(lambda a: a[2]['state']['statistics'].__setitem__(0, 99.))
    corrupt(lambda a: a[2]['state']['sd'].__setitem__(1, 99.))
    corrupt(lambda a: a[2]['state'].update(next_id=True))
    corrupt(lambda a: a[3]['state'].update(revision=0))
    corrupt(lambda a: a[0]['state']['grants'][0].__setitem__(1, 0))
    corrupt(lambda a: a[0]['state'].update(extra=0))
    for bad in mutations:
        try:
            score(bad, small)
        except (ValueError, TypeError):
            continue
        raise ValueError('corrupted typed pool report accepted')
    return len(mutations)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write-plan', action='store_true');parser.add_argument('--verify', action='store_true')
    parser.add_argument('--contract', action='store_true');parser.add_argument('--native', type=Path);parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.write_plan:
        PLAN.write_text(json.dumps(make_plan(), indent=2)+'\n')
    plan = json.loads(PLAN.read_text());require(plan==make_plan(), 'frozen typed pool plan changed')
    if args.verify:
        for c in plan['cases']:
            for h in plan['horizons']:
                reference(c, h)
        print('typed pool frozen plan verified')
    if args.contract:
        print(f'{contract(plan)} typed pool negative controls passed')
    if args.native:
        data = args.native.read_bytes();rows = [json.loads(line) for line in data.splitlines()]
        report = score(rows, plan);report['trajectory_sha256'] = hashlib.sha256(data).hexdigest()
        report['negative_controls'] = contract(plan)
        if args.report:
            args.report.write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report))

if __name__=='__main__':
    main()
