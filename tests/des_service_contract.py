"""Service stream ownership diagnostics and declaration-order determinism."""
import copy
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    exe = str(Path(sys.argv[1]).resolve())
    model = json.loads((ROOT/'models/independent_service_process.ir.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)/'model.json'
        def call(document, command):
            path.write_text(json.dumps(document))
            return subprocess.run([exe, command, str(path)], capture_output=True, text=True)
        baseline = call(model, 'run')
        assert baseline.returncode == 0, baseline.stderr
        rng = random.Random(8324)
        for _ in range(32):
            case = copy.deepcopy(model)
            rng.shuffle(case['components'])
            rng.shuffle(case['links'])
            result = call(case, 'run')
            assert result.returncode == 0 and result.stdout == baseline.stdout, result.stderr
        # The source's service metadata is irrelevant only where all stations override it.
        case = copy.deepcopy(model)
        for job in case['components'][0]['schedule']: job['service'] *= 1000
        result = call(case, 'run')
        assert result.returncode == 0 and result.stdout == baseline.stdout, result.stderr
        mutations = []
        case = copy.deepcopy(model)
        case['components'][2]['service']['stream'] = 201
        mutations.append((case, 'IR_DES', '/components/2/service/stream'))
        for stream in (12, 13):
            case = copy.deepcopy(model)
            case['components'][0] = dict(id='arrivals', kind='source', exponential=dict(
                count=8, arrival_rate=.5, service_rate=1, start=0, first_id=0, stream=12))
            case['components'][1]['service']['stream'] = stream
            mutations.append((case, 'IR_DES', '/components/1/service/stream'))
        case = copy.deepcopy(model)
        case['components'][0]['schedule'][0]['id'] = 1 << 48
        mutations.append((case, 'IR_DES', '/components/0/schedule/0/id'))
        hybrid = json.loads((ROOT/'models/hybrid_rate.ir.json').read_text())
        for stream in (20, 21):
            case = copy.deepcopy(hybrid)
            case['des']['components'][1]['service'] = dict(kind='exponential', rate=1, stream=stream)
            mutations.append((case, 'IR_HYBRID', '/bridges/1/stream'))
        for case, code, pointer in mutations:
            result = call(case, 'lint')
            assert result.returncode != 0
            diagnostic = json.loads(result.stderr)['diagnostics'][0]
            assert diagnostic['code'] == code and diagnostic['pointer'] == pointer, diagnostic
        # Adjacent single station streams are valid, including the largest address.
        case = copy.deepcopy(model)
        case['components'][1]['service']['stream'] = 65535
        case['components'][0]['schedule'][0]['id'] = (1 << 48)-1
        assert call(case, 'run').returncode == 0
        case = copy.deepcopy(hybrid)
        case['des']['components'][1]['service'] = dict(kind='exponential', rate=1, stream=22)
        assert call(case, 'run').returncode == 0
    print('Service contract: 32 permutations, metadata independence, 6 diagnostics and address boundaries passed')


if __name__ == '__main__':
    main()
