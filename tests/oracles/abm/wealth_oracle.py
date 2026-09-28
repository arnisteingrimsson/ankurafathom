"""Pinned Mesa wealth workload, independent exact draws, and offline model gates."""
import argparse
import bisect
import copy
import hashlib
import importlib.metadata
import json
import math
from pathlib import Path
import statistics
import sys

from wealth_plan import HERE, encoded as encoded_plan, make_plan

REFERENCE = HERE / 'wealth-reference.json'


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata():
    return dict(plan_sha256=digest(HERE/'wealth-plan.json'),
                adapter_sha256=digest(Path(__file__)),
                requirements_sha256=digest(HERE/'requirements.txt'),
                engines=dict(Mesa='3.5.1', networkx='3.7'))


def encode(value):
    return json.dumps(value, separators=(',', ':'), sort_keys=True, allow_nan=False) + '\n'


def generate():
    for line in (HERE/'requirements.txt').read_text().splitlines():
        package, version = line.split('==')
        require(importlib.metadata.version(package) == version, 'unpinned '+package)
    import mesa
    from mesa.space import NetworkGrid
    import networkx as nx

    class Person(mesa.Agent):
        def __init__(self, model, index, wealth):
            super().__init__(model)
            self.index, self.wealth = index, wealth

        def give(self):
            if not self.wealth:
                return
            recipients = (self.model.grid.get_neighbors(self.pos, include_center=False)
                          if self.model.grid is not None else self.model.people)
            if recipients:
                recipient = self.random.choice(recipients)
                self.wealth -= 1
                recipient.wealth += 1

    class Economy(mesa.Model):
        def __init__(self, spec, seed):
            super().__init__(seed=seed)
            self.people = [Person(self, i, spec['initial']) for i in range(spec['n'])]
            self.grid = None
            if spec['topology'] != 'mixed':
                graph = nx.cycle_graph(spec['n']) if spec['topology'] == 'ring' else nx.star_graph(spec['n']-1)
                self.grid = NetworkGrid(graph)
                for person in self.people:
                    self.grid.place_agent(person, person.index)

        def step(self):
            self.agents.shuffle_do('give')

    plan = make_plan()
    rows = []
    for index, case in enumerate(plan['cases']):
        for replication in range(plan['replications']):
            model = Economy(case, plan['reference_seed'] + 1000003*index + replication)
            tick = 0
            for observation in plan['ticks']:
                while tick < observation:
                    model.step()
                    tick += 1
                    state = [p.wealth for p in model.people]
                    require(sum(state) == case['n']*case['initial'] and min(state) >= 0,
                            'Mesa sweep violated wealth conservation')
                rows.append(dict(case=case['id'], replication=replication, tick=tick,
                                 wealth=[p.wealth for p in model.people]))
    return dict(metadata=metadata(), rows=rows)


def validate_rows(rows):
    plan = make_plan()
    expected = [(c, r, t) for c in plan['cases'] for r in range(plan['replications']) for t in plan['ticks']]
    require(len(rows) == len(expected), 'wealth observation count')
    for row, (case, replication, tick) in zip(rows, expected):
        require(set(row) == {'case', 'replication', 'tick', 'wealth'}, 'wealth row fields')
        require(row['case'] == case['id'] and type(row['replication']) is int and row['replication'] == replication
                and type(row['tick']) is int and row['tick'] == tick, 'wealth observation identity/order')
        values = row['wealth']
        require(isinstance(values, list) and len(values) == case['n']
                and all(type(v) is int and v >= 0 for v in values), 'wealth population/record invariant')
        require(sum(values) == case['n']*case['initial'], 'wealth not conserved')
        if tick == 0:
            require(values == [case['initial']]*case['n'], 'initial wealth differs')


def read_reference():
    reference = json.loads(REFERENCE.read_text())
    require(set(reference) == {'metadata', 'rows'} and reference['metadata'] == metadata(), 'stale wealth reference')
    validate_rows(reference['rows'])
    return reference


def metrics(values):
    total, n = sum(values), len(values)
    # Pairwise absolute differences avoid relying on the sorted-rank Gini formula.
    return dict(gini=sum(abs(a-b) for a in values for b in values)/(2*n*total),
                zero_fraction=values.count(0)/n, maximum_share=max(values)/total,
                concentration=sum(v*v for v in values)/(total*total))


def ks(a, b):
    a, b = sorted(a), sorted(b)
    return max(abs(bisect.bisect_right(a, x)/len(a)-bisect.bisect_right(b, x)/len(b)) for x in set(a+b))


def compare(native, reference):
    plan = make_plan()
    def samples(rows):
        result = {}
        for row in rows:
            result.setdefault((row['case'], row['tick']), []).append(metrics(row['wealth']))
        return result
    a, b = samples(native), samples(reference)
    policy, n = plan['gates'], plan['replications']
    limit = math.sqrt(math.log(2*policy['comparisons']/policy['family_alpha'])/n)
    gates = []
    for case in plan['cases']:
        for tick in plan['ticks'][1:]:
            for metric in plan['metrics']:
                x = [m[metric] for m in a[case['id'], tick]]
                y = [m[metric] for m in b[case['id'], tick]]
                mx, my = statistics.mean(x), statistics.mean(y)
                se = math.sqrt((statistics.variance(x)+statistics.variance(y))/n)
                mean_limit = max(policy['mean_floor'][metric], policy['mean_sigma']*se)
                distance = ks(x, y)
                gates.append(dict(case=case['id'], tick=tick, metric=metric,
                                  native_mean=mx, reference_mean=my, mean_gap=abs(mx-my),
                                  difference_ci95=[mx-my-1.96*se, mx-my+1.96*se],
                                  mean_limit=mean_limit, ks=distance, ks_limit=limit,
                                  passed=abs(mx-my) <= mean_limit and distance <= limit))
    require(len(gates) == policy['comparisons'], 'gate count drift')
    return gates


def exact(rows):
    # Shared independent Python Philox primitive; the activation/transfer model below
    # uses plain lists and a separate shuffle/choice implementation.
    sys.path.insert(0, str(HERE.parents[1]))
    from abm_ir_contract import word
    plan = make_plan()
    actual = {(r['case'], r['replication'], r['tick']): r['wealth'] for r in rows}
    observations = 0
    for case in plan['cases']:
        n = case['n']
        for replication in [0, 1, plan['replications']-1]:
            values = [case['initial']]*n
            for tick in range(plan['ticks'][-1]+1):
                if tick:
                    def choose(bound, entity, stream):
                        retry = 0
                        while True:
                            raw = word(plan['seed'], plan['scenario'], replication, entity,
                                       tick-1 | (retry << 16), stream)
                            retry += 1
                            if raw >= 2**32 % bound:
                                return raw % bound
                    order = list(range(n))
                    for i in range(n-1, 0, -1):
                        j = choose(i+1, i, plan['order_stream'])
                        order[i], order[j] = order[j], order[i]
                    for donor in order:
                        if not values[donor]:
                            continue
                        if case['topology'] == 'mixed':
                            eligible = list(range(n))
                        elif case['topology'] == 'ring':
                            eligible = sorted([(donor-1) % n, (donor+1) % n])
                        else:
                            eligible = list(range(1, n)) if donor == 0 else [0]
                        recipient = eligible[choose(len(eligible), donor, plan['recipient_stream'])]
                        values[donor] -= 1
                        values[recipient] += 1
                if tick in plan['ticks']:
                    require(actual[case['id'], replication, tick] == values, 'addressed wealth history differs')
                    observations += n
    return observations


def contract():
    reference = read_reference()
    for mutation in [lambda r:r['rows'].pop(), lambda r:r['metadata'].__setitem__('plan_sha256','stale'),
                     lambda r:r['rows'][1].__setitem__('tick',99),
                     lambda r:r['rows'][1]['wealth'].__setitem__(0,-1),
                     lambda r:r['rows'][1]['wealth'].__setitem__(0,True)]:
        bad = copy.deepcopy(reference)
        mutation(bad)
        try:
            require(bad['metadata'] == metadata(), 'stale metadata')
            validate_rows(bad['rows'])
        except ValueError:
            continue
        raise ValueError('corrupt reference accepted')
    require(metrics([1,1]) == dict(gini=0, zero_fraction=0, maximum_share=.5, concentration=.5), 'equal wealth metrics')
    require(metrics([0,2]) == dict(gini=.5, zero_fraction=.5, maximum_share=1, concentration=1), 'concentrated wealth metrics')
    require(ks([0,0],[1,1]) == 1 and ks([0,1],[0,1]) == 0, 'KS contract')
    # Valid conserved populations with deliberately wrong dynamics must fail gates.
    frozen = copy.deepcopy(reference['rows'])
    for row in frozen:
        case = next(c for c in make_plan()['cases'] if c['id'] == row['case'])
        row['wealth'] = [case['initial']]*case['n']
    validate_rows(frozen)
    require(any(not g['passed'] for g in compare(frozen, reference['rows'])), 'frozen dynamics passed model gates')
    print('wealth reference, metrics, corruption and wrong-dynamics contracts passed')


def score(path, report):
    with Path(path).open() as file:
        require(json.loads(next(file)) == dict(plan=make_plan()), 'native wealth plan differs')
        rows = [json.loads(line) for line in file]
    validate_rows(rows)
    observations = exact(rows)
    gates = compare(rows, read_reference()['rows'])
    result = dict(metadata=metadata(), replications_per_engine=2048, exact_observations=observations,
                  gates=gates, passed=all(g['passed'] for g in gates))
    if report:
        Path(report).write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    require(result['passed'], 'wealth gates failed: '+str([g for g in gates if not g['passed']]))
    print(f'wealth: {len(gates)} distribution gates, 2048 runs per engine, {observations} exact addressed observations passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for flag in ['write','verify','contract']:
        parser.add_argument('--'+flag, action='store_true')
    parser.add_argument('--native')
    parser.add_argument('--report')
    args = parser.parse_args()
    require((HERE/'wealth-plan.json').read_text() == encoded_plan(), 'stale wealth plan')
    if args.write:
        REFERENCE.write_text(encode(generate()))
    if args.verify:
        require(encode(generate()) == REFERENCE.read_text(), 'Mesa wealth reference does not reproduce')
    if args.contract:
        contract()
    if args.native:
        score(args.native, args.report)
