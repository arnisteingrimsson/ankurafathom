"""Independent Mesa Schelling implementation and offline native model gates."""
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
from schelling_plan import HERE, encoded as encoded_plan, make_plan

REFERENCE = HERE/'schelling-reference.json'


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata():
    return dict(plan_sha256=digest(HERE/'schelling-plan.json'),
                adapter_sha256=digest(Path(__file__)),
                requirements_sha256=digest(HERE/'requirements.txt'),
                engines=dict(Mesa='3.5.1', networkx='3.7'))


def encode(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False)+'\n'


def neighbors(case, agents, i):
    # Independent pairwise geometry: no native spatial index or Mesa calls.
    _, x, y = agents[i]
    result = []
    for j, (_, xx, yy) in enumerate(agents):
        if j == i:
            continue
        dx, dy = abs(x-xx), abs(y-yy)
        if case['wrap']:
            dx, dy = min(dx, case['width']-dx), min(dy, case['height']-dy)
        if (max(dx, dy) if case['moore'] else dx+dy) <= 1:
            result.append(j)
    return result


def happy(case, agents, i, adjacent):
    same = sum(agents[j][0] == agents[i][0] for j in adjacent)
    return same*case['denominator'] >= len(adjacent)*case['numerator']


def metrics(case, agents, moves):
    n = len(agents)
    adjacent = [neighbors(case, agents, i) for i in range(n)]
    same = [[j for j in adjacent[i] if agents[i][0] == agents[j][0]] for i in range(n)]
    unseen = set(range(n))
    largest = 0
    while unseen:
        pending = [unseen.pop()]
        size = 0
        while pending:
            i = pending.pop()
            size += 1
            new = unseen.intersection(same[i])
            unseen -= new
            pending.extend(new)
        largest = max(largest, size)
    return dict(unhappy_fraction=sum(not happy(case, agents, i, adjacent[i]) for i in range(n))/n,
                neighbor_similarity=sum(len(same[i])/len(adjacent[i]) if adjacent[i] else 1 for i in range(n))/n,
                largest_cluster=largest/n, moved_fraction=moves/n)


def validate_state(case, agents):
    require(isinstance(agents, list) and len(agents) == len(case['agents']), 'Schelling population count')
    cells = set()
    for actual, initial in zip(agents, case['agents']):
        require(isinstance(actual, list) and len(actual) == 3 and all(type(v) is int for v in actual), 'Schelling record type')
        group, x, y = actual
        require(group == initial[0], 'Schelling group/identity changed')
        require(0 <= x < case['width'] and 0 <= y < case['height'], 'Schelling coordinate outside grid')
        require((x, y) not in cells, 'Schelling cell collision')
        cells.add((x, y))


def generate():
    for line in (HERE/'requirements.txt').read_text().splitlines():
        package, version = line.split('==')
        require(importlib.metadata.version(package) == version, 'unpinned '+package)
    import mesa
    from mesa.space import SingleGrid
    import networkx as nx

    class Person(mesa.Agent):
        def __init__(self, model, index, group):
            super().__init__(model)
            self.index, self.group = index, group

        def adjacent(self):
            return self.model.grid.get_neighbors(self.pos, moore=self.model.case['moore'], include_center=False, radius=1)

        def satisfied(self):
            adjacent = self.adjacent()
            same = sum(other.group == self.group for other in adjacent)
            return same*self.model.case['denominator'] >= len(adjacent)*self.model.case['numerator']

        def relocate(self):
            if not self.satisfied() and self.model.grid.empties:
                vacancies = sorted(self.model.grid.empties, key=lambda p:(p[1], p[0]))
                self.model.grid.move_agent(self, self.random.choice(vacancies))
                self.model.moves += 1

    class Neighborhood(mesa.Model):
        def __init__(self, case, seed):
            super().__init__(seed=seed)
            self.case = case
            self.grid = SingleGrid(case['width'], case['height'], case['wrap'])
            self.people = []
            self.moves = 0
            for i, (group, x, y) in enumerate(case['agents']):
                agent = Person(self, i, group)
                self.people.append(agent)
                self.grid.place_agent(agent, (x, y))

        def state(self):
            return [[a.group, *a.pos] for a in self.people]

        def step(self):
            self.moves = 0
            self.agents.shuffle_do('relocate')
            validate_state(self.case, self.state())
            require(len(self.grid.empties)+len(self.people) == self.case['width']*self.case['height'], 'Mesa occupancy conservation')

        def summaries(self):
            graph = nx.Graph()
            graph.add_nodes_from(range(len(self.people)))
            similarity = []
            for a in self.people:
                adjacent = a.adjacent()
                similar = [b for b in adjacent if b.group == a.group]
                graph.add_edges_from((a.index, b.index) for b in similar)
                similarity.append(len(similar)/len(adjacent) if adjacent else 1)
            return dict(unhappy_fraction=sum(not a.satisfied() for a in self.people)/len(self.people),
                        neighbor_similarity=sum(similarity)/len(self.people),
                        largest_cluster=max(map(len, nx.connected_components(graph)))/len(self.people),
                        moved_fraction=self.moves/len(self.people))

    plan = make_plan()
    rows = []
    for index, case in enumerate(plan['cases']):
        for replication in range(plan['replications']):
            model = Neighborhood(case, plan['reference_seed']+1000003*index+replication)
            tick = 0
            for observation in plan['ticks']:
                while tick < observation:
                    model.step()
                    tick += 1
                rows.append(dict(case=case['id'], replication=replication, tick=tick,
                                 agents=model.state(), moves=model.moves, metrics=model.summaries()))
    return dict(metadata=metadata(), rows=rows)


def validate_rows(rows, reference=False):
    plan = make_plan()
    expected = [(c, r, t) for c in plan['cases'] for r in range(plan['replications']) for t in plan['ticks']]
    require(len(rows) == len(expected), 'Schelling observation count')
    for row, (case, replication, tick) in zip(rows, expected):
        require(set(row) == {'case', 'replication', 'tick', 'agents', 'moves'} | ({'metrics'} if reference else set()), 'Schelling row fields')
        require(row['case'] == case['id'] and type(row['replication']) is int and row['replication'] == replication
                and type(row['tick']) is int and row['tick'] == tick, 'Schelling observation identity/order')
        validate_state(case, row['agents'])
        require(type(row['moves']) is int and 0 <= row['moves'] <= len(case['agents']), 'Schelling move count')
        if tick == 0:
            require(row['agents'] == case['agents'] and row['moves'] == 0, 'Schelling initial state differs')
        if reference:
            require(set(row['metrics']) == set(plan['metrics']), 'Schelling metric names')
            derived = metrics(case, row['agents'], row['moves'])
            for name, number in row['metrics'].items():
                require(type(number) in (int, float) and math.isfinite(number)
                        and abs(number-derived[name]) < 1e-14, 'Mesa/pairwise summary mismatch')


def read_reference():
    value = json.loads(REFERENCE.read_text())
    require(set(value) == {'metadata', 'rows'} and value['metadata'] == metadata(), 'stale Schelling reference')
    validate_rows(value['rows'], reference=True)
    return value


def ks(a, b):
    a, b = sorted(a), sorted(b)
    return max(abs(bisect.bisect_right(a, x)/len(a)-bisect.bisect_right(b, x)/len(b)) for x in set(a+b))


def compare(rows, reference):
    plan = make_plan()
    cases = {c['id']:c for c in plan['cases']}
    samples = {}
    for row in rows:
        samples.setdefault((row['case'], row['tick']), []).append(metrics(cases[row['case']], row['agents'], row['moves']))
    ref = {}
    for row in reference:
        ref.setdefault((row['case'], row['tick']), []).append(row['metrics'])
    n, policy = plan['replications'], plan['gates']
    limit = math.sqrt(math.log(2*policy['comparisons']/policy['family_alpha'])/n)
    gates = []
    for case in plan['cases']:
        for tick in plan['ticks'][1:]:
            for metric in plan['metrics']:
                a = [v[metric] for v in samples[case['id'], tick]]
                b = [v[metric] for v in ref[case['id'], tick]]
                ma, mb = statistics.mean(a), statistics.mean(b)
                se = math.sqrt((statistics.variance(a)+statistics.variance(b))/n)
                mean_limit = max(policy['mean_floor'][metric], policy['mean_sigma']*se)
                distance = ks(a, b)
                gates.append(dict(case=case['id'], tick=tick, metric=metric,
                                  native_mean=ma, reference_mean=mb, mean_gap=abs(ma-mb),
                                  difference_ci95=[ma-mb-1.96*se, ma-mb+1.96*se],
                                  mean_limit=mean_limit, ks=distance, ks_limit=limit,
                                  passed=abs(ma-mb) <= mean_limit and distance <= limit))
    require(len(gates) == policy['comparisons'], 'Schelling gate count drift')
    return gates


def exact(rows):
    sys.path.insert(0, str(HERE.parents[1]))
    from abm_ir_contract import word
    plan = make_plan()
    actual = {(r['case'], r['replication'], r['tick']):r for r in rows}
    observations = 0
    for case in plan['cases']:
        n = len(case['agents'])
        for replication in [0, 1, plan['replications']-1]:
            agents = copy.deepcopy(case['agents'])
            moves = 0
            for tick in range(plan['ticks'][-1]+1):
                if tick:
                    def choose(bound, entity, stream):
                        retry = 0
                        while True:
                            raw = word(plan['seed'], plan['scenario'], replication, entity,
                                       (tick-1) | (retry << 16), stream)
                            retry += 1
                            if raw >= 2**32 % bound:
                                return raw % bound
                    order = list(range(n))
                    for i in range(n-1, 0, -1):
                        j = choose(i+1, i, plan['order_stream'])
                        order[i], order[j] = order[j], order[i]
                    moves = 0
                    for i in order:
                        if happy(case, agents, i, neighbors(case, agents, i)):
                            continue
                        occupied = {(a[1], a[2]) for a in agents}
                        empty = [(x,y) for y in range(case['height']) for x in range(case['width']) if (x,y) not in occupied]
                        if empty:
                            x, y = empty[choose(len(empty), i, plan['relocation_stream'])]
                            agents[i][1:] = [x, y]
                            moves += 1
                if tick in plan['ticks']:
                    row = actual[case['id'], replication, tick]
                    require(row['agents'] == agents and row['moves'] == moves, 'addressed Schelling trajectory differs')
                    observations += n
    return observations


def contract():
    reference = read_reference()
    for mutation in [lambda r:r['rows'].pop(), lambda r:r['metadata'].__setitem__('plan_sha256', 'stale'),
                     lambda r:r['rows'][1].__setitem__('moves', True),
                     lambda r:r['rows'][1].__setitem__('tick', 99),
                     lambda r:r['rows'][1]['agents'][0].__setitem__(0, 9),
                     lambda r:r['rows'][1]['agents'][0].__setitem__(1, -1),
                     lambda r:r['rows'][1]['agents'][0].__setitem__(slice(1,3), r['rows'][1]['agents'][1][1:]),
                     lambda r:r['rows'][1]['metrics'].__setitem__('largest_cluster', 99)]:
        bad = copy.deepcopy(reference)
        mutation(bad)
        try:
            require(bad['metadata'] == metadata(), 'stale metadata')
            validate_rows(bad['rows'], reference=True)
        except ValueError:
            continue
        raise ValueError('corrupt Schelling reference accepted')
    line = dict(width=4, height=1, wrap=False, moore=False, numerator=1, denominator=2)
    require(metrics(line, [[0,0,0],[0,1,0],[1,2,0]], 1) == dict(unhappy_fraction=1/3, neighbor_similarity=.5, largest_cluster=2/3, moved_fraction=1/3), 'hand Schelling metrics')
    require(metrics(line, [[0,0,0],[1,3,0]], 0) == dict(unhappy_fraction=0, neighbor_similarity=1, largest_cluster=.5, moved_fraction=0), 'isolated Schelling metrics')
    require(ks([0,0],[1,1]) == 1 and ks([0,1],[0,1]) == 0, 'KS contract')
    frozen = []
    cases = {c['id']:c for c in make_plan()['cases']}
    for row in reference['rows']:
        frozen.append(dict(case=row['case'], replication=row['replication'], tick=row['tick'],
                           agents=copy.deepcopy(cases[row['case']]['agents']), moves=0))
    validate_rows(frozen)
    require(any(not g['passed'] for g in compare(frozen, reference['rows'])), 'frozen spatial dynamics passed model gates')
    print('Schelling reference, geometry/metric, corruption and wrong-dynamics contracts passed')


def score(path, report):
    with Path(path).open() as file:
        require(json.loads(next(file)) == dict(plan=make_plan()), 'native Schelling plan differs')
        rows = [json.loads(line) for line in file]
    validate_rows(rows)
    observations = exact(rows)
    gates = compare(rows, read_reference()['rows'])
    plan = make_plan()
    result = dict(metadata=metadata(), runs_per_engine=len(plan['cases'])*plan['replications'],
                  exact_agent_observations=observations, gates=gates, passed=all(g['passed'] for g in gates))
    if report:
        Path(report).write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    require(result['passed'], 'Schelling gates failed: '+str([g for g in gates if not g['passed']]))
    print(f'Schelling: {len(gates)} distribution gates, {result["runs_per_engine"]} runs per engine, {observations} exact agent observations passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for flag in ['write', 'verify', 'contract']:
        parser.add_argument('--'+flag, action='store_true')
    parser.add_argument('--native')
    parser.add_argument('--report')
    args = parser.parse_args()
    require((HERE/'schelling-plan.json').read_text() == encoded_plan(), 'stale Schelling plan')
    if args.write:
        REFERENCE.write_text(encode(generate()))
    if args.verify:
        require(encode(generate()) == REFERENCE.read_text(), 'Mesa Schelling reference does not reproduce')
    if args.contract:
        contract()
    if args.native:
        score(args.native, args.report)
