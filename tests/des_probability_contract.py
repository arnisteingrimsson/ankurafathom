"""Probabilistic routing is stable under declarations, observations and replay."""
import copy
import csv
import io
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    exe = str(Path(sys.argv[1]).resolve())
    model = json.loads((ROOT/'models/probability_process.ir.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        path, experiment = Path(directory)/'model.json', Path(directory)/'experiment.json'
        settings = dict(seed=987654321, replications=4, scenarios=[dict(id=17, parameters={}), dict(id=18, parameters={})])
        experiment.write_text(json.dumps(settings))
        def call(document, command='run', repeated=False):
            path.write_text(json.dumps(document))
            args = [exe, command, str(path)]
            if repeated: args += ['--experiment', str(experiment)]
            return subprocess.run(args, capture_output=True, text=True)
        def values(result):
            assert result.returncode == 0, result.stderr
            return {(float(r['time']), r['output_id']): float(r['value'])
                    for r in csv.DictReader(io.StringIO(result.stdout))}
        baseline = call(model)
        base_values = values(baseline)
        rng = random.Random(530914)
        for _ in range(32):
            case = copy.deepcopy(model)
            rng.shuffle(case['components'])
            rng.shuffle(case['links'])
            result = call(case)
            assert result.returncode == 0 and result.stdout == baseline.stdout, result.stderr
        for dt in (.125, .5, 1):
            case = copy.deepcopy(model)
            case['time']['dt'] = dt
            actual = values(call(case))
            shared = actual.keys() & base_values.keys()
            assert shared and all(actual[k] == base_values[k] for k in shared)
        for probability in (0, 1):
            case = copy.deepcopy(model)
            case['components'][2]['probability']['match'] = probability
            actual = values(call(case))
            assert actual[24, 'matched'] == 32*probability
            assert actual[24, 'otherwise'] == 32*(1-probability)
            assert actual[24, 'completed'] == 32
        first, replay = call(model, repeated=True), call(model, repeated=True)
        assert first.returncode == replay.returncode == 0 and first.stdout == replay.stdout
        rows = list(csv.DictReader(io.StringIO(first.stdout)))
        final_matches = [r['value'] for r in rows if r['output_id'] == 'matched' and float(r['time']) == 24]
        assert len(final_matches) == 8 and len(set(final_matches)) > 1
        mutations = []
        case = copy.deepcopy(model)
        case['components'].append(dict(id='choice2', kind='router', probability=dict(match=.5, stream=301)))
        mutations.append((case, 'IR_DES', '/components/6/probability/stream'))
        case = copy.deepcopy(model)
        case['components'][2]['priority_at_most'] = 0
        mutations.append((case, 'IR_DES', '/components/2'))
        case = copy.deepcopy(model)
        case['components'][2].pop('probability')
        mutations.append((case, 'IR_DES', '/components/2'))
        for case, code, pointer in mutations:
            result = call(case, 'lint')
            assert result.returncode != 0
            diagnostic = json.loads(result.stderr)['diagnostics'][0]
            assert diagnostic['code'] == code and diagnostic['pointer'] == pointer, diagnostic
        case = copy.deepcopy(model)
        case['components'][2]['probability']['stream'] = 65535
        case['components'][0]['schedule'][0]['id'] = (1 << 48)-1
        assert call(case).returncode == 0
    print('Probability contract: declaration/observation invariance, endpoint conservation, experiment replay and rule diagnostics passed')


if __name__ == '__main__':
    main()
