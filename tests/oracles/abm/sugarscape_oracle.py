"""Independent Mesa Sugarscape-lite implementation and offline native gates."""
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
from sugarscape_plan import HERE, encoded as encoded_plan, make_plan
REFERENCE = HERE/'sugarscape-reference.json'


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata():
    return dict(plan_sha256=digest(HERE/'sugarscape-plan.json'),
                adapter_sha256=digest(Path(__file__)),
                requirements_sha256=digest(HERE/'requirements.txt'), engines=dict(Mesa='3.5.1'))


def encode(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False)+'\n'


def metrics(case, row):
    n = len(case['agents'])
    return dict(survival_fraction=len(row['agents'])/n,
                reserve_per_initial_agent=sum(a[1] for a in row['agents'])/n,
                land_fraction=sum(row['sugar'])/sum(case['capacity']),
                consumed_per_initial_agent=row['ledger']['consumed']/n)


def initial_row(case, first=0):
    return dict(agents=[[first+i, *a] for i, a in enumerate(case['agents'])],
                sugar=list(case['sugar']), ledger=dict(initial=sum(case['sugar'])+sum(a[0] for a in case['agents']),
                                                      regrown=0, consumed=0, deaths=0))


def validate_state(case, row):
    agents, sugar, ledger = row['agents'], row['sugar'], row['ledger']
    require(isinstance(agents, list) and len(agents) <= len(case['agents']), 'Sugarscape population count')
    require(isinstance(sugar, list) and len(sugar) == len(case['capacity']), 'Sugarscape landscape shape')
    require(all(type(s) is int and 0 <= s <= cap for s, cap in zip(sugar, case['capacity'])), 'Sugarscape land bounds/type')
    require(isinstance(ledger, dict) and set(ledger) == {'initial', 'regrown', 'consumed', 'deaths'}, 'Sugarscape ledger fields')
    require(all(type(v) is int and 0 <= v <= 2**63-1 for v in ledger.values()), 'Sugarscape ledger type/range')
    cells, previous = set(), -1
    for a in agents:
        require(isinstance(a, list) and len(a) == 6 and all(type(v) is int for v in a), 'Sugarscape record type')
        i, reserve, metabolism, vision, x, y = a
        require(previous < i < len(case['agents']), 'Sugarscape identity/order')
        previous = i
        require(0 < reserve <= 2**63-1 and [metabolism, vision] == case['agents'][i][1:3], 'Sugarscape immutable traits/reserve')
        require(0 <= x < case['width'] and 0 <= y < case['height'] and (x,y) not in cells, 'Sugarscape occupancy')
        cells.add((x,y))
    require(ledger['initial'] == initial_row(case)['ledger']['initial'], 'Sugarscape initial budget')
    require(len(agents)+ledger['deaths'] == len(case['agents']), 'Sugarscape population conservation')
    require(sum(a[1] for a in agents)+sum(sugar)+ledger['consumed'] == ledger['initial']+ledger['regrown'] <= 2**63-1,
            'Sugarscape resource conservation')


def generate():
    for line in (HERE/'requirements.txt').read_text().splitlines():
        package, version = line.split('==')
        require(importlib.metadata.version(package) == version, 'unpinned '+package)
    import mesa
    from mesa.space import SingleGrid

    class Forager(mesa.Agent):
        def __init__(self, model, index, reserve, metabolism, vision):
            super().__init__(model)
            self.index, self.reserve, self.metabolism, self.vision = index, reserve, metabolism, vision

        def forage(self):
            m = self.model
            # Mesa supplies canonical, deduplicated von Neumann cells. Filter to
            # axial vision: diagonal cells in the Manhattan ball are not visible.
            cells = m.grid.get_neighborhood(self.pos, moore=False, include_center=True, radius=self.vision)
            x, y = self.pos
            candidates = []
            for xx, yy in cells:
                if xx != x and yy != y:
                    continue
                if (xx, yy) != self.pos and not m.grid.is_cell_empty((xx, yy)):
                    continue
                dx, dy = abs(xx-x), abs(yy-y)
                if m.case['wrap']:
                    dx, dy = min(dx, m.case['width']-dx), min(dy, m.case['height']-dy)
                candidates.append((m.sugar[yy*m.case['width']+xx], -(dx+dy), xx, yy))
            best = max((s, d) for s, d, _, _ in candidates)
            ties = sorted([(xx, yy) for s, d, xx, yy in candidates if (s,d) == best], key=lambda p:(p[1],p[0]))
            to = self.random.choice(ties)
            m.grid.move_agent(self, to)
            cell = to[1]*m.case['width']+to[0]
            self.reserve += m.sugar[cell]
            m.sugar[cell] = 0
            used = min(self.reserve, self.metabolism)
            self.reserve -= used
            m.consumed += used
            if self.reserve == 0:
                m.grid.remove_agent(self)
                self.remove()
                m.deaths += 1

    class Landscape(mesa.Model):
        def __init__(self, case, seed):
            super().__init__(seed=seed)
            self.case = case
            self.grid = SingleGrid(case['width'], case['height'], case['wrap'])
            self.sugar = list(case['sugar'])
            self.regrown = self.consumed = self.deaths = 0
            self.initial = initial_row(case)['ledger']['initial']
            for i, (reserve, metabolism, vision, x, y) in enumerate(case['agents']):
                self.grid.place_agent(Forager(self, i, reserve, metabolism, vision), (x,y))

        def state(self):
            agents = sorted(self.agents, key=lambda a:a.index)
            return dict(agents=[[a.index, a.reserve, a.metabolism, a.vision, *a.pos] for a in agents],
                        sugar=list(self.sugar), ledger=dict(initial=self.initial, regrown=self.regrown,
                                                           consumed=self.consumed, deaths=self.deaths))

        def step(self):
            self.agents.shuffle_do('forage')
            for i, cap in enumerate(self.case['capacity']):
                grown = min(self.case['regrowth'], cap-self.sugar[i])
                self.sugar[i] += grown
                self.regrown += grown
            validate_state(self.case, self.state())
            require(len(self.grid.empties)+len(self.agents) == self.case['width']*self.case['height'], 'Mesa occupancy budget')

        def summaries(self):
            # Inspect Mesa objects/grid directly, independently of exported rows.
            count = len(self.case['agents'])
            land = sum(self.sugar[y*self.case['width']+x] for _, (x,y) in self.grid.coord_iter())
            return dict(survival_fraction=len(self.agents)/count,
                        reserve_per_initial_agent=sum(a.reserve for a in self.agents)/count,
                        land_fraction=land/sum(self.case['capacity']), consumed_per_initial_agent=self.consumed/count)

    plan, rows = make_plan(), []
    for index, case in enumerate(plan['cases']):
        for replication in range(plan['replications']):
            model = Landscape(case, plan['reference_seed']+1000003*index+replication)
            tick = 0
            for observation in plan['ticks']:
                while tick < observation:
                    model.step()
                    tick += 1
                rows.append(dict(case=case['id'], replication=replication, tick=tick,
                                 **model.state(), metrics=model.summaries()))
    return dict(metadata=metadata(), rows=rows)


def validate_rows(rows, reference=False):
    plan = make_plan()
    expected = [(c,r,t) for c in plan['cases'] for r in range(plan['replications']) for t in plan['ticks']]
    require(isinstance(rows, list) and len(rows) == len(expected), 'Sugarscape observation count')
    previous = None
    for row, (case, replication, tick) in zip(rows, expected):
        require(set(row) == {'case', 'replication', 'tick', 'agents', 'sugar', 'ledger'} | ({'metrics'} if reference else set()), 'Sugarscape row fields')
        require(row['case'] == case['id'] and type(row['replication']) is int and row['replication'] == replication
                and type(row['tick']) is int and row['tick'] == tick, 'Sugarscape observation identity/order')
        validate_state(case, row)
        if tick == 0:
            require(all(row[k] == v for k, v in initial_row(case).items()), 'Sugarscape initial state differs')
        else:
            require(set(a[0] for a in row['agents']) <= set(a[0] for a in previous['agents']), 'Sugarscape retired ID resurrected')
            for name in ['regrown', 'consumed', 'deaths']:
                require(row['ledger'][name] >= previous['ledger'][name], 'Sugarscape cumulative ledger decreased')
            require(row['ledger']['regrown']-previous['ledger']['regrown'] <= (tick-previous['tick'])*len(case['capacity'])*case['regrowth'],
                    'Sugarscape regrowth exceeds rate')
        if reference:
            require(set(row['metrics']) == set(plan['metrics']), 'Sugarscape metric names')
            derived = metrics(case, row)
            for name, number in row['metrics'].items():
                require(type(number) in (int,float) and math.isfinite(number) and abs(number-derived[name]) < 1e-14, 'Mesa/scorer summary mismatch')
        previous = row


def read_reference():
    value = json.loads(REFERENCE.read_text())
    require(set(value) == {'metadata', 'rows'} and value['metadata'] == metadata(), 'stale Sugarscape reference')
    validate_rows(value['rows'], reference=True)
    return value


def ks(a, b):
    a, b = sorted(a), sorted(b)
    return max(abs(bisect.bisect_right(a,x)/len(a)-bisect.bisect_right(b,x)/len(b)) for x in set(a+b))


def compare(rows, reference):
    plan = make_plan()
    cases = {c['id']:c for c in plan['cases']}
    samples, ref = {}, {}
    for row in rows:
        samples.setdefault((row['case'], row['tick']), []).append(metrics(cases[row['case']],row))
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
                distance = ks(a,b)
                gates.append(dict(case=case['id'], tick=tick, metric=metric, native_mean=ma, reference_mean=mb,
                                  mean_gap=abs(ma-mb), difference_ci95=[ma-mb-1.96*se,ma-mb+1.96*se],
                                  mean_limit=mean_limit, ks=distance, ks_limit=limit,
                                  passed=abs(ma-mb) <= mean_limit and distance <= limit))
    require(len(gates) == policy['comparisons'], 'Sugarscape gate count drift')
    return gates


def addressed(case, draws, ticks, first=0):
    """List/dict recurrence with ray enumeration, independent of C++ and Mesa indexes."""
    sys.path.insert(0, str(HERE.parents[1]))
    from abm_ir_contract import word
    row = initial_row(case, first)
    agents = {a[0]:a[1:] for a in row['agents']}
    sugar, ledger, result = row['sugar'], row['ledger'], {}
    for tick in range(max(ticks)+1):
        if tick:
            def choose(bound, entity, stream):
                retry = 0
                while True:
                    raw = word(draws['seed'], draws['scenario'], draws['replication'], entity, (tick-1)|(retry<<16), stream)
                    retry += 1
                    if raw >= 2**32 % bound:
                        return raw % bound
            order = sorted(agents)
            for i in range(len(order)-1,0,-1):
                j = choose(i+1, first+i, draws['order_stream'])
                order[i], order[j] = order[j], order[i]
            for i in order:
                reserve, metabolism, vision, x, y = agents[i]
                visible = {(x,y)}
                for distance in range(1,vision+1):
                    for xx, yy in [(x-distance,y),(x+distance,y),(x,y-distance),(x,y+distance)]:
                        if case['wrap']:
                            xx, yy = xx%case['width'], yy%case['height']
                        if 0 <= xx < case['width'] and 0 <= yy < case['height']:
                            visible.add((xx,yy))
                occupied = {(a[3],a[4]) for j,a in agents.items() if j != i}
                def ranking(p):
                    dx, dy = abs(x-p[0]), abs(y-p[1])
                    if case['wrap']:
                        dx, dy = min(dx,case['width']-dx), min(dy,case['height']-dy)
                    return sugar[p[1]*case['width']+p[0]], -(dx+dy)
                available = sorted(visible-occupied, key=lambda p:(p[1],p[0]))
                best = max(map(ranking, available))
                tied = [p for p in available if ranking(p) == best]
                xx, yy = tied[choose(len(tied),i,draws['movement_stream'])]
                cell = yy*case['width']+xx
                available_reserve = reserve+sugar[cell]
                sugar[cell] = 0
                consumed = min(available_reserve, metabolism)
                ledger['consumed'] += consumed
                reserve = available_reserve-consumed
                if reserve:
                    agents[i] = [reserve,metabolism,vision,xx,yy]
                else:
                    del agents[i]
                    ledger['deaths'] += 1
            for i, cap in enumerate(case['capacity']):
                delta = min(cap-sugar[i],case['regrowth'])
                sugar[i] += delta
                ledger['regrown'] += delta
        if tick in ticks:
            result[tick] = copy.deepcopy(dict(agents=[[i,*a] for i,a in sorted(agents.items())],sugar=sugar,ledger=ledger))
    return result


def exact(rows):
    plan = make_plan()
    actual = {(r['case'],r['replication'],r['tick']):r for r in rows}
    agent_states = cell_states = ledgers = 0
    for case in plan['cases']:
        for replication in [0,1,plan['replications']-1]:
            draws = dict(plan, replication=replication)
            for tick, expected in addressed(case, draws, plan['ticks']).items():
                row = actual[case['id'],replication,tick]
                require(all(row[k] == v for k,v in expected.items()), 'addressed Sugarscape trajectory differs')
                agent_states += len(row['agents'])
                cell_states += len(row['sugar'])
                ledgers += 1
    return dict(agent_states=agent_states, cell_states=cell_states, ledgers=ledgers)


def contract():
    reference = read_reference()
    for mutation in [lambda r:r['rows'].pop(), lambda r:r['metadata'].__setitem__('plan_sha256','stale'),
                     lambda r:r['rows'][1].__setitem__('tick',True),
                     lambda r:r['rows'][1]['ledger'].__setitem__('consumed',True),
                     lambda r:r['rows'][1]['ledger'].__setitem__('regrown',999999),
                     lambda r:r['rows'][1]['ledger'].__setitem__('deaths',99),
                     lambda r:r['rows'][1]['sugar'].__setitem__(0,-1),
                     lambda r:r['rows'][1]['agents'][0].__setitem__(1,0),
                     lambda r:r['rows'][1]['agents'][0].__setitem__(2,99),
                     lambda r:r['rows'][1]['agents'][0].__setitem__(4,-1),
                     lambda r:r['rows'][1]['agents'][0].__setitem__(slice(4,6),r['rows'][1]['agents'][1][4:]),
                     lambda r:r['rows'][1]['metrics'].__setitem__('land_fraction',99)]:
        bad = copy.deepcopy(reference)
        mutation(bad)
        try:
            require(bad['metadata'] == metadata(), 'stale metadata')
            validate_rows(bad['rows'],reference=True)
        except ValueError:
            continue
        raise ValueError('corrupt Sugarscape reference accepted')
    tiny = dict(width=2,height=1,wrap=False,regrowth=3,capacity=[2,5],sugar=[0,4],agents=[[1,1,0,0,0]])
    draws = dict(seed=1,scenario=0,replication=0,order_stream=1,movement_stream=2)
    result = addressed(tiny,draws,[1])[1]
    require(result == dict(agents=[],sugar=[2,5],ledger=dict(initial=5,regrown=3,consumed=1,deaths=1)), 'hand starvation/regrowth recurrence')
    require(metrics(tiny,result) == dict(survival_fraction=0,reserve_per_initial_agent=0,land_fraction=1,consumed_per_initial_agent=1), 'hand metrics')
    require(ks([0,0],[1,1]) == 1 and ks([0,1],[0,1]) == 0, 'KS contract')
    frozen, cases = [], {c['id']:c for c in make_plan()['cases']}
    for row in reference['rows']:
        frozen.append(dict(case=row['case'],replication=row['replication'],tick=row['tick'],**initial_row(cases[row['case']])))
    validate_rows(frozen)
    require(any(not g['passed'] for g in compare(frozen,reference['rows'])), 'frozen dynamics passed Sugarscape gates')
    print('Sugarscape reference, budget/metric, corruption and wrong-dynamics contracts passed')


def score(path, report):
    with Path(path).open() as file:
        require(json.loads(next(file)) == dict(plan=make_plan()), 'native Sugarscape plan differs')
        rows = [json.loads(line) for line in file]
    validate_rows(rows)
    observations = exact(rows)
    gates = compare(rows,read_reference()['rows'])
    result = dict(metadata=metadata(),runs_per_engine=len(make_plan()['cases'])*make_plan()['replications'],
                  exact=observations,gates=gates,passed=all(g['passed'] for g in gates))
    if report:
        Path(report).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    require(result['passed'], 'Sugarscape gates failed: '+str([g for g in gates if not g['passed']]))
    print(f'Sugarscape: {len(gates)} distribution gates, {result["runs_per_engine"]} runs per engine, {observations} exact observations passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for flag in ['write','verify','contract']:
        parser.add_argument('--'+flag,action='store_true')
    parser.add_argument('--native')
    parser.add_argument('--report')
    args = parser.parse_args()
    require((HERE/'sugarscape-plan.json').read_text() == encoded_plan(), 'stale Sugarscape plan')
    if args.write:
        REFERENCE.write_text(encode(generate()))
    if args.verify:
        require(encode(generate()) == REFERENCE.read_text(), 'Mesa Sugarscape reference does not reproduce')
    if args.contract:
        contract()
    if args.native:
        score(args.native,args.report)
