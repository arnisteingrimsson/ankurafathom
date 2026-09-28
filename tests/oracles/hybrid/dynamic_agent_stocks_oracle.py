"""Independent rational Euler/lifecycle recurrence, including bridge publication delay."""
import argparse
import copy
from fractions import Fraction as F
import hashlib
import itertools
import json
import math
from pathlib import Path

PLAN = Path(__file__).with_name('dynamic-agent-stocks-plan.json')


def require(ok, why):
    if not ok:
        raise ValueError(why)


def make_plan():
    cases = []
    for i, (agent_dt, sd_dt) in enumerate(((.25, .125), (.5, .25), (.125, .5), (.375, .25))):
        cases.append(dict(id=f'clocks_{i}', agent_dt=agent_dt, sd_dt=sd_dt, first=2**48-16 if i==3 else 0,
                          initial=[[20+i, 0, .5, True], [16+i, 1, .75, True]],
                          direct=[
                              dict(time=.125, channel='z', updates=[[1, 3, False]], pulses=[['agent', 0, 0, -1], ['global', 0, 0, 1]]),
                              dict(time=.375, channel='z', updates=[[1, 3, True]], births=[[14, 0, .5, True]]),
                              dict(time=.375, channel='a', births=[[12, 1, .25, True]]),
                              dict(time=.5, channel='a', pulses=[['agent', 0, 0, -2], ['global', 0, 0, 2]]),
                              dict(time=.75, channel='a', updates=[[2, 2, .75]], pulses=[['global', 0, 0, -.5], ['global', 0, 0, .5]]),
                              dict(time=1.125, channel='z', births=[[18, 2, .5, True]]),
                              dict(time=1.875, channel='a', pulses=[['agent', 3, 1, 1], ['agent', 3, 0, -1]]),
                          ],
                          lifecycle=[
                              dict(time=.5, key=40, retire=[1], births=[[11, 0, .25, True]]),
                              dict(time=.5, key=10, retire=[0], births=[[13, 1, .5, True]]),
                              dict(time=1.5, key=50, retire=[2], births=[]),
                              dict(time=2, key=70, retire=[], births=[[9, 0, .25, True]]),
                          ]))
    return dict(version=1, cases=cases, orders=list(map(list, itertools.permutations(range(4)))),
                bag_orders=['forward', 'reverse'], horizons=[i/16 for i in range(41)], tolerance=1e-11)


def reference(case, horizon):
    first = case['first']; rows = []; global_stock = F(0); core_time = F(0); revision = 0
    sd_time = F(0); sd = [F(0)]*3; receipts = []
    def spawn(record):
        rows.append([first+len(rows), F(record[0]), F(record[1]), F(record[2]), bool(record[3]), True])
    for r in case['initial']:
        spawn(r)
    def aggregates():
        return [sum((r[1] for r in rows if r[5]), F(0)), sum((r[2] for r in rows if r[5]), F(0)), F(sum(r[5] for r in rows))]
    signals = aggregates()
    def integrate(t):
        nonlocal core_time, global_stock
        dt = t-core_time; old_global = global_stock
        for r in rows:
            if not r[5] or not r[4]:
                continue
            transfer = r[3]*(1+old_global/64)*dt
            export = F(1, 8)*dt
            r[1] -= transfer+export; r[2] += transfer; global_stock += export
            require(r[1]>=0, 'reference stock exhausted')
        core_time = t
    def apply(parts, life, t):
        nonlocal global_stock, revision
        pulse = {}; global_pulse = F(0); retire = []; births = []
        for e in parts:
            for agent, field, value in e.get('updates', []):
                rows[agent][field+1] = bool(value) if field==3 else F(value)
            for kind, agent, field, amount in e.get('pulses', []):
                if kind=='global':
                    global_pulse += F(amount)
                else:
                    pulse[(agent, field)] = pulse.get((agent, field), F(0))+F(amount)
            retire.extend(e.get('retire', []));births.extend(e.get('births', []))
        for e in life:
            retire.extend(e.get('retire', []));births.extend(e.get('births', []))
        agent_pulse = [F(0)]*2
        for (agent, field), amount in sorted(pulse.items()):
            rows[agent][field+1] += amount;agent_pulse[field] += amount
        global_stock += global_pulse
        removed = [F(0)]*2
        for agent in retire:
            require(rows[agent][5], 'reference repeated retirement')
            for j in range(2):
                removed[j] += rows[agent][j+1]
            rows[agent][5] = False
        born = []; added = [F(0)]*2
        for record in births:
            born.append(first+len(rows));spawn(record)
            for j in range(2):
                added[j] += F(record[j])
        revision += 1
        receipts.append([float(t), revision, born, [first+i for i in retire], list(map(float, added)),
                         list(map(float, removed)), list(map(float, agent_pulse)), [float(global_pulse)]])
    agent_ticks = {F(case['agent_dt'])*i for i in range(1, 1+math.floor(horizon/case['agent_dt']))}
    sd_ticks = {F(case['sd_dt'])*i for i in range(1, 1+math.floor(horizon/case['sd_dt']))}
    direct = {}
    for e in case['direct']:
        if e['time']<=horizon:
            direct.setdefault(F(e['time']), []).append(e)
    lifecycle = {}
    for e in case['lifecycle']:
        if e['time']<=horizon:
            lifecycle.setdefault(F(e['time']), []).append(e)
    for t in sorted(agent_ticks | sd_ticks | direct.keys() | lifecycle.keys()):
        changed = False
        if t in agent_ticks or t in direct:
            integrate(t)
            if t in direct:
                apply(sorted(direct[t], key=lambda e: e['channel']), [], t)
            else:
                revision += 1
            changed = True
        # A bridge emits only after consuming its typed event: a second commit
        # follows the direct/tick commit at the same physical timestamp.
        if t in lifecycle:
            integrate(t);apply([], sorted(lifecycle[t], key=lambda e: e['key']), t);changed = True
        if changed or t in sd_ticks:
            for j in range(3):
                sd[j] += (t-sd_time)*signals[j]
            sd_time = t
        if changed:
            signals = aggregates()
    converted = [[r[0], float(r[1]), float(r[2]), float(r[3]), r[4], r[5]] for r in rows]
    initial_total = sum(F(r[0])+F(r[1]) for r in case['initial'])
    added = sum(F(x) for receipt in receipts for x in receipt[4])
    removed = sum(F(x) for receipt in receipts for x in receipt[5])
    # Pulse changes are conservative in every declared case. Receipt floats are
    # checked separately against the exact recurrence; inventory tolerance is explicit.
    balance_gap = abs(float(sum(aggregates()[:2])+global_stock-initial_total)-float(added-removed))
    require(balance_gap<1e-12, 'reference discrete conservation')
    return dict(time=float(core_time), revision=revision, rows=converted, globals=[float(global_stock)],
                sd_time=float(sd_time), sd=list(map(float, sd)), signals=list(map(float, signals)),
                receipts=receipts, next_id=first+len(rows))


def compare(actual, expected, tol, path='state'):
    if isinstance(expected, dict):
        require(isinstance(actual, dict) and actual.keys()==expected.keys(), path+' keys')
        pairs = [compare(actual[k], v, tol, path+'.'+k) for k, v in expected.items()]
    elif isinstance(expected, list):
        require(isinstance(actual, list) and len(actual)==len(expected), path+' length')
        pairs = [compare(a, b, tol, path) for a, b in zip(actual, expected)]
    else:
        if type(expected) in (bool, int):
            require(type(actual) is type(expected) and actual==expected, path+' discrete')
            return 1, 0.
        require(type(actual) in (int, float) and math.isfinite(actual), path+' finite')
        gap = abs(actual-expected);require(gap<=tol, path+f' gap {gap}')
        return 1, gap
    return sum(p[0] for p in pairs), max((p[1] for p in pairs), default=0.)


def score(rows, plan):
    refs = {(c['id'], h): reference(c, h) for c in plan['cases'] for h in plan['horizons']}
    expected = {(c, tuple(o), b, h): ref for (c, h), ref in refs.items() for o in plan['orders'] for b in plan['bag_orders']}
    require(len(rows)==len(expected), 'row count');seen = set();checks = 0;gap = 0.
    for row in rows:
        require(set(row)=={'case', 'order', 'bag_order', 'horizon', 'state'}, 'row fields')
        key = row['case'], tuple(row['order']), row['bag_order'], row['horizon']
        require(key in expected and key not in seen, 'unknown/duplicate observation');seen.add(key)
        count, error = compare(row['state'], expected[key], plan['tolerance']);checks += count;gap = max(gap, error)
    return dict(passed=True, configurations=len(plan['cases'])*len(plan['orders'])*len(plan['bag_orders']), snapshots=len(rows),
                scalar_comparisons=checks, max_absolute_gap=gap)


def contract(plan):
    small = dict(plan, cases=plan['cases'][:1], orders=plan['orders'][:1], bag_orders=['forward'], horizons=[0, .125, .5, 2.5])
    good = [dict(case=c['id'], order=o, bag_order=b, horizon=h, state=reference(c, h))
            for c in small['cases'] for o in small['orders'] for b in small['bag_orders'] for h in small['horizons']]
    score(good, small);mutations = []
    def corrupt(fn):
        bad = copy.deepcopy(good);fn(bad);mutations.append(bad)
    corrupt(lambda a: a.pop())
    corrupt(lambda a: a.append(copy.deepcopy(a[0])))
    corrupt(lambda a: a[0].update(case='unknown'))
    corrupt(lambda a: a[1]['state'].update(time=.25))
    corrupt(lambda a: a[2]['state'].update(revision=999))
    corrupt(lambda a: a[2]['state']['rows'][0].__setitem__(5, True))
    corrupt(lambda a: a[2]['state']['rows'][0].__setitem__(0, False))
    corrupt(lambda a: a[2]['state']['rows'][2].__setitem__(1, 99.))
    corrupt(lambda a: a[1]['state']['globals'].__setitem__(0, 99.))
    corrupt(lambda a: a[3]['state']['sd'].__setitem__(0, 99.))
    corrupt(lambda a: a[3]['state']['signals'].__setitem__(1, 99.))
    corrupt(lambda a: a[2]['state']['receipts'][-1][3].reverse())
    corrupt(lambda a: a[2]['state']['receipts'][-1][5].__setitem__(0, 99.))
    corrupt(lambda a: a[1]['state']['receipts'][0][6].__setitem__(0, 0.))
    corrupt(lambda a: a[0]['state'].update(next_id=True))
    corrupt(lambda a: a[0]['state'].update(extra=0))
    for bad in mutations:
        try:
            score(bad, small)
        except (ValueError, TypeError):
            continue
        raise ValueError('corrupted dynamic stock report accepted')
    return len(mutations)


def main():
    parser = argparse.ArgumentParser();parser.add_argument('--write-plan', action='store_true');parser.add_argument('--verify', action='store_true')
    parser.add_argument('--contract', action='store_true');parser.add_argument('--native', type=Path);parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.write_plan:
        PLAN.write_text(json.dumps(make_plan(), indent=2)+'\n')
    plan = json.loads(PLAN.read_text());require(plan==make_plan(), 'frozen dynamic stock plan changed')
    if args.verify:
        for c in plan['cases']:
            for h in plan['horizons']:
                reference(c, h)
        print('dynamic agent stock frozen plan verified')
    if args.contract:
        print(f'{contract(plan)} dynamic stock negative controls passed')
    if args.native:
        data = args.native.read_bytes();report = score([json.loads(line) for line in data.splitlines()], plan)
        report['trajectory_sha256'] = hashlib.sha256(data).hexdigest();report['negative_controls'] = contract(plan)
        if args.report:
            args.report.write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report))

if __name__=='__main__':
    main()
