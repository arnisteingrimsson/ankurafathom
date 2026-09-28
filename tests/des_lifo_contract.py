"""Hand LIFO trajectory, hybrid completion pulses, and observation invariance."""
import copy
import csv
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    exe = str(Path(sys.argv[1]).resolve())
    model = json.loads((ROOT/'models/lifo_process.ir.json').read_text())
    hybrid = json.loads((ROOT/'models/hybrid_completion.ir.json').read_text())
    hybrid['time'] = copy.deepcopy(model['time'])
    hybrid['des'] = {k: copy.deepcopy(model[k]) for k in ('components', 'links')}
    hybrid['sd']['components'].pop()  # Pure completion counter, no decay.
    hybrid['outputs'] = copy.deepcopy(model['outputs'])+[dict(id='stock', stock='completed_stock')]
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)/'model.json'
        def run(document):
            path.write_text(json.dumps(document))
            result = subprocess.run([exe, 'run', str(path)], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            return {(float(r['time']), r['output_id']):float(r['value'])
                    for r in csv.DictReader(io.StringIO(result.stdout))}
        baseline, coupled = run(model), run(hybrid)
        for (time, key), value in baseline.items():
            assert coupled[time, key] == value
            if key == 'completed': assert coupled[time, 'stock'] == value
        expected = {0:(3,1,0,2,0), 1:(4,1,1,2,1), 2:(4,2,2,1,2), 3:(4,2,3,0,5), 4:(4,2,4,0,9)}
        for time, entries in expected.items():
            for key, value in zip(('accepted','rejected','completed','waiting','cycle_total'), entries, strict=True):
                assert baseline[time,key] == value, (time,key,baseline[time,key],value)
        assert baseline[4,'queue_mean'] == 1.25 and baseline[4,'utilization'] == 1
        for dt in (.25, 1):
            case = copy.deepcopy(model)
            case['time']['dt'] = dt
            actual = run(case)
            assert all(actual[k] == baseline[k] for k in actual.keys() & baseline.keys())
        case = copy.deepcopy(model)
        for entity in case['components'][0]['schedule']: entity['priority'] = -int(entity['id'])
        assert run(case) == baseline
    print('LIFO IR: exact hand trajectory, priority independence, hybrid pulses and sampling invariance passed')


if __name__ == '__main__':
    main()
