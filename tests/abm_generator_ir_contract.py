"""Declarative graphs: independent addressed draws and lifecycle/override contracts."""
import csv
import io
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from abm_ir_contract import word

ROOT = Path(__file__).resolve().parents[1]


def graph(n, kind, p, degree, m, seed, scenario, replication, stream):
    adjacency = [set() for _ in range(n)]

    def add(a, b):
        adjacency[a].add(b)
        adjacency[b].add(a)

    ordinal = 0

    def draw():
        nonlocal ordinal
        # Network sequence occupies step and draw-index fields in Philox word 3.
        packed = (ordinal >> 16) | ((ordinal & 65535) << 16)
        ordinal += 1
        return word(seed, scenario, replication, 0, packed, stream)

    def index(bound):
        threshold = (1 << 32) % bound
        while True:
            value = draw()
            if value >= threshold:
                return value % bound

    if kind == 'erdos_renyi':
        pair = 0
        for a in range(n):
            for b in range(a + 1, n):
                if (word(seed, scenario, replication, pair, 0, stream) + .5) / 2**32 < p:
                    add(a, b)
                pair += 1
    elif kind == 'watts_strogatz':
        for distance in range(1, degree // 2 + 1):
            for a in range(n):
                add(a, (a + distance) % n)
        for distance in range(1, degree // 2 + 1):
            for a in range(n):
                if (draw() + .5) / 2**32 >= p:
                    continue
                candidates = [b for b in range(n) if b != a and b not in adjacency[a]]
                if candidates:
                    b = candidates[index(len(candidates))]
                    old = (a + distance) % n
                    adjacency[a].remove(old)
                    adjacency[old].remove(a)
                    add(a, b)
    else:
        for b in range(1, m + 1):
            add(0, b)
        for a in range(m + 1, n):
            weights = [len(adjacency[b]) for b in range(a)]
            chosen = []
            for _ in range(m):
                ticket = index(sum(weights))
                for b, weight in enumerate(weights):
                    if ticket < weight:
                        chosen.append(b)
                        weights[b] = 0
                        break
                    ticket -= weight
            for b in chosen:
                add(a, b)
    return adjacency


def model(kind, n=8, asynchronous=False):
    value = json.loads((ROOT / 'models/typed_abm_network_generator.ir.json').read_text())
    component = value['components'][0]
    component['agents'] = [{'value': 2**i} for i in range(n)]
    component['phases'] = [{'assign': []}]
    component['queries'][1] = dict(id='signature', source='network', op='sum', field='value')
    value['parameters'] = [dict(id=k, value=v, unit='1') for k, v in [('p', .35), ('k', 2), ('m', 2)]]
    generator = dict(kind=kind, stream=901)
    if kind != 'barabasi_albert':
        generator['probability'] = 'p'
    if kind == 'watts_strogatz':
        generator['degree'] = 'k'
    if kind == 'barabasi_albert':
        generator['m'] = 'm'
    component['network'] = dict(generator=generator)
    value['outputs'] = [dict(id=f'{q}{i}', agent=i, query=q) for i in range(n) for q in ['degree', 'signature']]
    if not n:
        value['outputs'] = [dict(id='active', metric='active')]
    if asynchronous:
        rate = json.loads((ROOT / 'models/typed_abm_rates.ir.json').read_text())['components'][0]
        component.pop('phases')
        component['execution'] = 'async'
        component['fields'] += rate['fields']
        for agent in component['agents']:
            agent.update(rate['agents'][0])
        component['chart'] = rate['chart']
        value['parameters'].append(dict(id='hazard', value=0, unit='1/day'))
    return value


def main():
    exe = str(Path(sys.argv[1]).resolve())
    observations = 0
    with tempfile.TemporaryDirectory() as directory:
        path, experiment_path = [Path(directory) / s for s in ['model.json', 'experiment.json']]

        def call(value, experiment=None, valid=True, mode='run'):
            path.write_text(json.dumps(value))
            command = [exe, mode, str(path)]
            if experiment is not None:
                experiment_path.write_text(json.dumps(experiment))
                command += ['--experiment', str(experiment_path)]
            result = subprocess.run(command, capture_output=True, text=True)
            if not valid:
                assert result.returncode != 0, result.stdout
                diagnostic = json.loads(result.stderr)['diagnostics'][0]
                assert diagnostic['code'].startswith('IR_'), diagnostic
                return diagnostic
            assert result.returncode == 0, result.stderr
            if mode == 'lint':
                return
            return {(int(r.get('scenario', 0)), int(r.get('replication', 0)), float(r['time']), r['output_id']): float(r['value'])
                    for r in csv.DictReader(io.StringIO(result.stdout))}

        for kind in ['erdos_renyi', 'watts_strogatz', 'barabasi_albert']:
            experiment = dict(seed=7319, replications=3, scenarios=[
                dict(id=0, parameters={}), dict(id=4, parameters=dict(p=0, k=0, m=1)),
                dict(id=9, parameters=dict(p=1, k=6, m=7))])
            for asynchronous in [False, True]:
                value = model(kind, asynchronous=asynchronous)
                rows = call(value, experiment)
                for scenario, p, k, m in [(0, .35, 2, 2), (4, 0, 0, 1), (9, 1, 6, 7)]:
                    for replication in range(3):
                        expected = graph(8, kind, p, k, m, 7319, scenario, replication, 901)
                        for time in [0, 1, 2]:
                            for agent, neighbors in enumerate(expected):
                                for q, number in [('degree', len(neighbors)), ('signature', sum(2**b for b in neighbors))]:
                                    assert rows[scenario, replication, time, f'{q}{agent}'] == number, (kind, asynchronous, scenario, replication, time, agent, q)
                                    observations += 1
                assert call(value, experiment) == rows
                value['outputs'].reverse()
                value['components'][0]['queries'].reverse()
                value['time']['dt'] = .5
                dense = call(value, experiment)
                assert all(dense[key] == number for key, number in rows.items())
                # Scenario values are validated again after a successful default lint.
                call(value, mode='lint')
                invalid_options = [('m', 0), ('m', 1.5), ('m', 8)] if kind == 'barabasi_albert' else [('p', -1), ('p', 1.1)]
                if kind == 'watts_strogatz':
                    invalid_options += [('k', 3), ('k', 8), ('k', 2.5)]
                for key, number in invalid_options:
                    bad = dict(seed=7319, replications=1, scenarios=[dict(id=0, parameters={key: number})])
                    assert call(value, bad, valid=False)['code'] == 'IR_ABM_RUNTIME'
            # A second seed and stream independently exercise the remaining RNG keys.
            value = model(kind, 12)
            value['components'][0]['network']['generator']['stream'] = 65535
            experiment = dict(seed=982451653, replications=1, scenarios=[dict(id=13, parameters={})])
            rows = call(value, experiment)
            expected = graph(12, kind, .35, 2, 2, 982451653, 13, 0, 65535)
            for time in [0, 1, 2]:
                for agent, neighbors in enumerate(expected):
                    assert rows[13, 0, time, f'degree{agent}'] == len(neighbors)
                    assert rows[13, 0, time, f'signature{agent}'] == sum(2**b for b in neighbors)
                    observations += 2
        empty = model('erdos_renyi', 0)
        assert all(v == 0 for v in call(empty).values())
        for kind, n, options in [('watts_strogatz', 1, dict(k=0)), ('watts_strogatz', 5, dict(k=4, p=1)), ('barabasi_albert', 2, dict(m=1))]:
            value = model(kind, n)
            for parameter in value['parameters']:
                parameter['value'] = options.get(parameter['id'], parameter['value'])
            rows = call(value)
            assert all(number == (n-1 if key[-1].startswith('degree') else sum(2**b for b in range(n) if b != int(key[-1][9:]))) for key, number in rows.items())
        # Disabled charts still reserve their rate stream.
        collision = model('erdos_renyi', asynchronous=True)
        collision['components'][0]['chart']['transitions'][0]['stream'] = 901
        call(collision, valid=False, mode='lint')
        # Initial graph precedes time-zero retirement, isolated birth, then edge edit.
        value = model('erdos_renyi', 3)
        value['parameters'][0]['value'] = 1
        component = value['components'][0]
        component['lifecycle'] = [dict(time=0, sequence=0, retire=[1], births=[dict(value=8)])]
        value['outputs'] = [dict(id=f'degree{i}', agent=i, query='degree', inactive_value=-1) for i in range(4)]
        isolated = call(value)
        assert all(number == [1, -1, 1, 0][int(key[-1][-1])] for key, number in isolated.items())
        component['network_updates'] = [dict(time=0, sequence=0, add=[[0, 3]], remove=[])]
        edited = call(value)
        assert all(number == [2, -1, 1, 1][int(key[-1][-1])] for key, number in edited.items())
        component['network_updates'][0]['add'] = [[0, 2]]
        call(value, valid=False)
    print(f'Declarative graph generators: {observations} exact addressed observations; replay, ordering, density, overrides and lifecycle passed')


if __name__ == '__main__':
    main()
