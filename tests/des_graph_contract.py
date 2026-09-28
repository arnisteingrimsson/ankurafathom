"""Declaration-order and topology diagnostics for the standalone DES graph."""
import copy
import csv
import io
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile


def main():
    exe = str(Path(sys.argv[1]).resolve())
    fixture = json.loads(Path(sys.argv[2]).read_text())
    def call(document, command, directory):
        path = Path(directory) / 'model.json'
        path.write_text(json.dumps(document))
        return subprocess.run([exe, command, str(path)], capture_output=True, text=True)
    with tempfile.TemporaryDirectory() as directory:
        baseline = call(fixture, 'run', directory)
        assert baseline.returncode == 0, baseline.stderr
        rng = random.Random(1749)
        for _ in range(32):
            case = copy.deepcopy(fixture)
            rng.shuffle(case['components'])
            rng.shuffle(case['links'])
            result = call(case, 'run', directory)
            assert result.returncode == 0 and result.stdout == baseline.stdout, result.stderr
        # Boundary thresholds exercise both all-match and all-otherwise paths.
        # The final result must still conserve entities, including terminal losses.
        for threshold in (-2147483648, 2147483647):
            case = copy.deepcopy(fixture)
            case['components'][1]['priority_at_most'] = threshold
            result = call(case, 'run', directory)
            assert result.returncode == 0, result.stderr
            rows = list(csv.DictReader(io.StringIO(result.stdout)))
            final = {r['output_id']: float(r['value']) for r in rows if float(r['time']) == 4}
            assert final['arrivals_emitted'] == final['done_completed'] + final['lost_discarded']
        mutations = []
        case = copy.deepcopy(fixture)
        case['links'][-2]['to'] = 'triage'
        mutations.append(('cycle', case, '/links'))
        case = copy.deepcopy(fixture)
        case['links'].pop()
        mutations.append(('missing_router_exit', case, '/links'))
        case = copy.deepcopy(fixture)
        case['links'].append(copy.deepcopy(case['links'][1]))
        mutations.append(('fanout', case, '/links/11'))
        case = copy.deepcopy(fixture)
        case['links'][0]['to'] = 'missing'
        mutations.append(('missing_endpoint', case, '/links/0'))
        case = copy.deepcopy(fixture)
        case['links'][0]['port'] = 'rejected'
        mutations.append(('wrong_port', case, '/links/0'))
        case = copy.deepcopy(fixture)
        case['links'].append({'from': 'done', 'to': 'backup'})
        mutations.append(('sink_output', case, '/links/11'))
        case = copy.deepcopy(fixture)
        case['links'][8]['to'] = 'done'
        case['components'] = [c for c in case['components'] if c['id'] != 'lost']
        case['outputs'] = [o for o in case['outputs'] if o['component'] != 'lost']
        mutations.append(('rejection_is_not_completion', case, '/links'))
        case = copy.deepcopy(fixture)
        case['components'].append({'id':'unreachable','kind':'server','capacity':1})
        case['links'].append({'from':'unreachable','to':'done'})
        mutations.append(('unreachable_station', case, '/links'))
        case = copy.deepcopy(fixture)
        case['links'][0]['to'] = 'arrivals'
        mutations.append(('source_input', case, '/links/0'))
        for name, case, pointer in mutations:
            result = call(case, 'lint', directory)
            assert result.returncode != 0, name
            diagnostic = json.loads(result.stderr)['diagnostics'][0]
            assert diagnostic['code'] == 'IR_LINK' and diagnostic['pointer'] == pointer, (name, diagnostic)
    print('DES graph: 32 declaration permutations, 2 threshold extremes, 9 topology diagnostics passed')


if __name__ == '__main__':
    main()
