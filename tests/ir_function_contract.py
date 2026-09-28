"""Exercise function loading, diagnostics, and the full standalone SD tick protocol."""
import copy
import csv
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FATHOM = str(Path(sys.argv[1]).resolve())
base = json.loads((ROOT/'models/sd_functions.ir.json').read_text())

with tempfile.TemporaryDirectory() as folder:
    path = Path(folder)/'model.json'

    def command(model, action='lint'):
        path.write_text(json.dumps(model))
        return subprocess.run([FATHOM, action, str(path)], text=True, capture_output=True)

    def reject(model, code, pointer):
        result = command(model)
        assert result.returncode != 0, result.stdout
        diagnostics = json.loads(result.stderr)['diagnostics']
        assert any(d['code'] == code and d['pointer'] == pointer for d in diagnostics), diagnostics

    result = command(base, 'run')
    assert result.returncode == 0, result.stdout+result.stderr
    rows = list(csv.DictReader(io.StringIO(result.stdout)))
    assert [(float(r['time']), r['output_id'], float(r['value'])) for r in rows] == [
        (t, name, value) for t, pair in enumerate([(0,0),(0,0),(10,5),(17,7.5),(28.5,8.75)])
        for name, value in zip(('total_ts','smooth_ts'), pair)]
    for expr, code in [('STEP(height)', 'IR_EXPR'), ('STEP(height,0,1)', 'IR_EXPR'),
                       ('STEP(height,height)', 'IR_UNIT'), ('SMOOTH(height,1)', 'IR_EXPR'), ('unknown(height)', 'IR_FUNCTION')]:
        model = copy.deepcopy(base)
        model['components'][1]['input'] = expr
        reject(model, code, '/components/1/input')
    for reserved in ('STEP', 'PULSE', 'RAMP'):
        model = copy.deepcopy(base)
        model['parameters'][0]['id'] = reserved
        result = command(model)
        assert result.returncode != 0 and '/parameters/0/id' in result.stderr, result.stdout
    hybrid = json.loads((ROOT/'models/hybrid_rate.ir.json').read_text())
    hybrid['sd']['parameters'] = [{'id':'height','value':1,'unit':'engagement/day'}]
    hybrid['sd']['components'].append(dict(id='input_flow', kind='flow', source=None,
        destination='signal', expr='STEP(height,1)', unit='engagement/day'))
    reject(hybrid, 'IR_HYBRID', '/sd/components/1/expr')
    model = copy.deepcopy(base)
    model['components'][2]['expr'] = 'height*PULSE(1,-1)'
    result = command(model, 'run')
    assert result.returncode != 0, 'negative pulse width accepted at runtime'
print('IR function trajectory and rejection checks passed')
