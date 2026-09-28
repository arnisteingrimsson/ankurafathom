"""Pinned SimPy/Ciw models. Generation never reads native outputs or code."""
import argparse
from collections import defaultdict
from importlib.metadata import version
import itertools
import json
import math
import random
import statistics

from engine_plan import ROOT, PLAN_NAMES, digest, load


def require(condition, message):
    if not condition:
        raise ValueError(message)


def close(a, b):
    return math.isclose(a, b, rel_tol=2e-9, abs_tol=2e-9)


def workload(spec, case, end, plan):
    arrival_rng = random.Random(case['seed'])
    rates = spec.get('service_rates', [1.0])
    services = [random.Random(case['seed']+plan['service_seed_offset']+i*plan['station_seed_stride']) for i in range(len(rates))]
    def exponential(rng, rate):
        return -math.log1p(-rng.random())/rate
    arrival_rate = spec.get('arrival_rate', .5)
    arrivals, durations = [], [[] for _ in rates]
    time = exponential(arrival_rng, arrival_rate)
    while time <= end:
        arrivals.append(time)
        for i, rng in enumerate(services):
            law = spec.get('service', 'exponential')
            if law == 'deterministic': duration = 1.0
            elif law == 'erlang2': duration = exponential(rng, 2)+exponential(rng, 2)
            elif law == 'two_point': duration = .25 if rng.random() < .8 else 4.0
            else:
                require(law == 'exponential', 'unsupported service law')
                duration = exponential(rng, rates[i])
            require(math.isfinite(duration) and duration > 0, 'invalid service draw')
            durations[i].append(duration)
        time += exponential(arrival_rng, arrival_rate)
    return arrivals, durations


def simpy_run(arrivals, durations, servers, limit, routes=None, discipline='fifo', priorities=None):
    import simpy
    env = simpy.Environment()
    require(discipline in ('fifo', 'lifo', 'priority'), 'unknown queue discipline')
    require(discipline == 'fifo' or (len(durations) == 1 and routes is None), 'discipline oracle is single-station only')
    resources = [(simpy.Resource if discipline == 'fifo' else simpy.PriorityResource)(env, capacity=servers)
                 for _ in durations]
    records, rejected = [], []
    def job(identity):
        for node in routes[identity] if routes is not None else range(len(resources)):
            resource = resources[node]
            entry = env.now
            # SimPy Resource itself has an unlimited queue; explicitly model
            # external loss admission before requesting its FIFO service slot.
            if limit is not None and resource.count+len(resource.queue) >= servers+limit:
                require(node == 0 and len(resources) == 1, 'finite downstream routing is outside this oracle')
                rejected.append((identity, entry))
                return
            rank = {} if discipline == 'fifo' else dict(priority=-identity if discipline == 'lifo' else priorities[identity])
            with resource.request(**rank) as request:
                yield request
                start = env.now
                yield env.timeout(durations[node][identity])
                records.append((identity, node, entry, start, env.now))
    def source():
        for identity, arrival in enumerate(arrivals):
            yield env.timeout(arrival-env.now)
            env.process(job(identity))
    env.process(source())
    env.run()  # Empty event queue: all admitted jobs finish.
    return sorted(records), sorted(rejected)


def ciw_run(arrivals, durations, servers, limit, seed, routes=None, discipline='fifo', priorities=None):
    import ciw
    class ArrivalSchedule(ciw.dists.Distribution):
        def __init__(self): self.index = 0
        def sample(self, t=None, ind=None):
            if self.index == len(arrivals): return math.inf
            target = arrivals[self.index]
            self.index += 1
            return target-t
    class EntityService(ciw.dists.Distribution):
        def __init__(self, node): self.node = node
        def sample(self, t=None, ind=None):
            return durations[self.node][ind.id_number-1]
    nodes = len(durations)
    require(discipline in ('fifo', 'lifo', 'priority'), 'unknown queue discipline')
    require(discipline == 'fifo' or (nodes == 1 and routes is None), 'discipline oracle is single-station only')
    service_discipline = ciw.disciplines.LIFO if discipline == 'lifo' else ciw.disciplines.FIFO
    if discipline == 'priority':
        service_discipline = lambda individuals, time: min(individuals,
            key=lambda ind: (priorities[ind.id_number-1], ind.id_number))
    routing = [[float(j == i+1) for j in range(nodes)] for i in range(nodes)]
    if routes is not None:
        # Built-in process routing consumes the remaining one-based node IDs.
        routing = ciw.routing.ProcessBased(lambda ind, simulation: [i+1 for i in routes[ind.id_number-1][1:]])
    network = ciw.create_network(
        arrival_distributions=[ArrivalSchedule()]+[None]*(nodes-1),
        service_distributions=[EntityService(i) for i in range(nodes)],
        number_of_servers=[servers]*nodes,
        service_disciplines=[service_discipline]*nodes,
        queue_capacities=[math.inf if limit is None else limit]*nodes,
        routing=routing)
    ciw.seed(seed)  # Pins Ciw's tie-breaking choices; workload draws are separate.
    sim = ciw.Simulation(network)
    # Even serial execution of every offered service fits within this bound.
    bound = (arrivals[-1] if arrivals else 0)+sum(map(sum, durations))+1
    sim.simulate_until_max_time(bound)
    records, rejected = [], []
    for record in sim.get_all_records():
        identity = record.id_number-1
        if record.record_type == 'service':
            require(record.time_blocked == 0, 'unexpected downstream blocking')
            require(close(record.exit_date, record.service_end_date), 'exit does not coincide with completion')
            records.append((identity, record.node-1, record.arrival_date, record.service_start_date, record.service_end_date))
        elif record.record_type == 'rejection':
            require(record.node == 1 and nodes == 1, 'unexpected internal rejection')
            rejected.append((identity, record.arrival_date))
        else:
            raise ValueError(f'unexpected Ciw record: {record.record_type}')
    require(all(node.number_of_individuals == 0 for node in sim.transitive_nodes), 'Ciw failed to drain')
    return sorted(records), sorted(rejected)


def compare_traces(left, right):
    for records_a, records_b in zip(left, right, strict=True):
        require(len(records_a) == len(records_b), 'engine record counts differ')
        for a, b in zip(records_a, records_b, strict=True):
            ids = 2 if len(a) == 5 else 1
            require(a[:ids] == b[:ids], f'engine identity/outcome differs: {a}, {b}')
            require(all(close(x, y) for x, y in zip(a[ids:], b[ids:], strict=True)), f'engine timestamps differ: {a}, {b}')


def summarize(arrivals, durations, servers, limit, trace, warm, window, routes=None):
    records, rejected = trace
    end, nodes = warm+window, len(durations)
    grouped = [[] for _ in range(nodes)]
    by_identity = {}
    visited = defaultdict(set)
    for identity, node, entry, start, finish in records:
        require(0 <= identity < len(arrivals) and 0 <= node < nodes, 'unknown entity or node')
        require((identity, node) not in by_identity, 'duplicate service record')
        require(all(math.isfinite(x) for x in (entry, start, finish)) and entry <= start < finish, 'invalid service times')
        require(close(finish-start, durations[node][identity]), 'wrong job service draw')
        by_identity[identity, node] = (entry, start, finish)
        visited[identity].add(node)
        grouped[node].append((identity, entry, start, finish))
    lost = dict(rejected)
    require(len(lost) == len(rejected), 'duplicate rejection')
    for identity, born in enumerate(arrivals):
        if identity in lost:
            require(close(lost[identity], born) and all((identity, i) not in by_identity for i in range(nodes)), 'lost job also served')
        else:
            previous = born
            route = routes[identity] if routes is not None else range(nodes)
            require(visited[identity] == set(route), 'incorrect visited node set')
            for i in route:
                entry, start, finish = by_identity[identity, i]
                require(close(entry, previous), 'inter-stage flow or source identity mismatch')
                previous = finish
    require(set(lost) <= set(range(len(arrivals))), 'unknown rejection identity')
    values, events = {}, [(warm, 0, 0), (end, 0, 0)]
    queue_integrals, busy_integrals = [], []
    overlap = lambda a, b: max(0.0, min(b, end)-max(a, warm))
    for i, jobs in enumerate(grouped):
        prefix = '' if nodes == 1 else f's{i}_'
        queue = sum(overlap(entry, start) for _, entry, start, _ in jobs)
        busy = sum(overlap(start, finish) for _, _, start, finish in jobs)
        cohort = [start-entry for _, entry, start, _ in jobs if warm < entry <= end]
        require(cohort, 'empty measurement cohort')
        values.update({prefix+'queue': queue/window, prefix+'utilization': busy/(servers*window),
                       prefix+'wait': statistics.mean(cohort),
                       prefix+'throughput': sum(warm < finish <= end for _, _, _, finish in jobs)/window})
        queue_integrals.append(queue); busy_integrals.append(busy)
        for _, entry, _, finish in jobs: events.extend([(entry, i, 1), (finish, i, -1)])
    if nodes == 1:
        offers = sum(warm < t <= end for t in arrivals)
        require(offers > 0, 'empty offer cohort')
        values['blocking'] = sum(warm < t <= end for t in lost.values())/offers
    else:
        cycles = [by_identity[identity, routes[identity][-1] if routes is not None else nodes-1][2]-born
                  for identity, born in enumerate(arrivals) if warm < born <= end]
        values['cycle'] = statistics.mean(cycles)
        if routes is not None:
            routed = [identity for identity, _, _, finish in grouped[0] if warm < finish <= end]
            require(routed, 'empty routing cohort')
            values['match_fraction'] = sum(routes[identity][1] == 1 for identity in routed)/len(routed)
    state, q_area, b_area = [0]*nodes, [0.0]*nodes, [0.0]*nodes
    probabilities = defaultdict(float)
    previous = 0
    for time, node, change in sorted(events):
        elapsed = max(0.0, min(time, end)-max(previous, warm))
        if nodes == 1 and limit is not None: probabilities[f'p{state[0]}'] += elapsed/window
        elif nodes > 1: probabilities['p'+''.join(str(min(n, 2)) for n in state)] += elapsed/window
        for i, n in enumerate(state):
            q_area[i] += max(0, n-servers)*elapsed
            b_area[i] += min(n, servers)*elapsed
        state[node] += change
        require(state[node] >= 0 and (limit is None or state[node] <= servers+limit), 'occupancy violates capacity')
        previous = time
    require(state == [0]*nodes, 'event histories failed to drain')
    for i in range(nodes):
        require(close(q_area[i], queue_integrals[i]) and close(b_area[i], busy_integrals[i]), 'state/individual integrals differ')
    keys = [f'p{n}' for n in range(servers+limit+1)] if limit is not None else (
        ['p'+''.join(map(str, cell)) for cell in itertools.product(range(3), repeat=nodes)] if nodes > 1 else [])
    for key in keys: values[key] = probabilities[key]
    if keys: require(close(sum(values[k] for k in keys), 1), 'occupancy probability mass differs from one')
    return values


def aggregate(samples):
    result = {}
    for key in samples[0]:
        values = [s[key] for s in samples]
        mean = statistics.mean(values)
        se = statistics.stdev(values)/math.sqrt(len(values))
        result[key] = dict(mean=mean, standard_error=se, ci95=[mean-1.959963984540054*se, mean+1.959963984540054*se])
    return result


def generate(plan):
    packages = dict(line.strip().split('==') for line in (ROOT/'requirements.txt').read_text().splitlines() if line.strip())
    require(all(version(name) == expected for name, expected in packages.items()), 'oracle dependencies differ from pins')
    originals = {group: json.loads((ROOT/name).read_text()) for group, name in PLAN_NAMES.items()}
    report = dict(version=1, plan_sha256=digest(ROOT/'engine-plan.json'), generator_sha256=digest(ROOT/'engine_oracles.py'),
                  packages=packages, engines={name: [] for name in plan['engines']}, matched_service_records=0, matched_rejections=0)
    for case in plan['cases']:
        original = originals[case['group']]
        spec = next(s for s in original['cases'] if s['name'] == case['name'])
        spec = dict(spec, arrival_rate=spec.get('arrival_rate', original.get('arrival_rate', .5)))
        warm, window = original['warmup'], original['window']
        samples = {name: [] for name in plan['engines']}
        for rep in range(case['replications']):
            try:
                seed = case['seed_base']+rep
                arrivals, durations = workload(spec, {'seed': seed}, warm+window, plan)
                routes = None
                if 'branch_probability' in spec:
                    routing_rng = random.Random(seed+plan['routing_seed_offset'])
                    routes = [[0, 1 if routing_rng.random() < spec['branch_probability'] else 2] for _ in arrivals]
                servers, limit = spec.get('servers', 1), spec.get('queue_capacity')
                traces = dict(simpy=simpy_run(arrivals, durations, servers, limit, routes),
                              ciw=ciw_run(arrivals, durations, servers, limit, seed, routes))
                compare_traces(traces['simpy'], traces['ciw'])
                report['matched_service_records'] += len(traces['simpy'][0])
                report['matched_rejections'] += len(traces['simpy'][1])
                for name, trace in traces.items():
                    values = summarize(arrivals, durations, servers, limit, trace, warm, window, routes)
                    require(values.keys() == case['metrics'].keys(), 'metric set differs from plan')
                    samples[name].append(values)
            except Exception as error:
                raise ValueError(f"{case['name']} replication {rep}: {error}") from error
        for name, values in samples.items():
            metrics = aggregate(values)
            for key, metric in metrics.items():
                gate = case['metrics'][key]
                tolerance = gate['analytic_tolerance'] or plan['numerical_tolerance']
                require(abs(metric['mean']-gate['target']) <= tolerance, f"{name}/{case['name']}/{key} fails analytical gate")
            report['engines'][name].append(dict(name=case['name'], group=case['group'], replications=case['replications'], metrics=metrics))
        print(f"{case['name']}: {case['replications']} replications agree in SimPy/Ciw", flush=True)
    return report


def equivalent(current, stored, path=''):
    if isinstance(current, dict):
        require(isinstance(stored, dict) and current.keys() == stored.keys(), f'keys differ at {path}')
        for key, value in current.items(): equivalent(value, stored[key], path+'/'+key)
    elif isinstance(current, list):
        require(isinstance(stored, list) and len(current) == len(stored), f'length differs at {path}')
        for i, (a, b) in enumerate(zip(current, stored, strict=True)): equivalent(a, b, path+f'/{i}')
    elif isinstance(current, float):
        require(math.isfinite(current) and math.isclose(current, stored, rel_tol=1e-10, abs_tol=1e-12), f'regeneration drift at {path}')
    else: require(current == stored, f'value differs at {path}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    report = generate(load())
    path = ROOT/'engine-reference.json'
    if args.verify: equivalent(report, json.loads(path.read_text()))
    else: path.write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
    print(f"Matched {report['matched_service_records']} service records and {report['matched_rejections']} rejection records")
