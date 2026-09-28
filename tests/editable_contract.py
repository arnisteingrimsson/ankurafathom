"""Run with python -I after pip install -e .; do not alter sys.path."""
import copy
import importlib.metadata
import json
from pathlib import Path
import struct

import ankurafathom as af

ROOT = Path(__file__).resolve().parents[1]
receipt = json.loads(importlib.metadata.distribution('ankurafathom').read_text('direct_url.json'))
assert receipt['dir_info']['editable'] is True, receipt
assert Path(af.__file__).resolve() == ROOT/'bindings/python/ankurafathom/__init__.py'
source = json.loads((ROOT/'models/decay.ir.json').read_text())
model = af.Model.from_json(source)
reference = af.run(model).to_pylist()
assert len(reference) == 11
for i, row in enumerate(reference):
    assert abs(row['value'] - 100 * 0.98**i) < 1e-10, row
def encoded(rows):
    return [(row['scenario'], row['replication'], row['output_id'],
             struct.pack('>d', row['time']), struct.pack('>d', row['value'])) for row in rows]
for threads in (1, 8, 32):
    assert encoded(af.run(model, threads=threads).to_pylist()) == encoded(reference)
controls = 0
for expression in ('-'*8192+'1', '('*4096+'1'+')'*4096,
                   '+'.join(['1']*8192), ' '*65536+'1',
                   'NONNEGATIVE('*256+'1'+')'*256):
    bad = copy.deepcopy(source)
    bad['components'][1]['expr'] = expression
    try:
        af.Model.from_json(bad)
    except af.FathomError as error:
        assert error.status == 2 and error.code == 'IR_EXPR'
        assert error.pointer == '/components/1/expr'
    else:
        raise AssertionError('oversized expression crossed the Python/C ABI boundary')
    assert af.lint(bad)['verdict'] == 'fail'
    # A failed native load must not poison subsequent operations.
    assert encoded(af.run(model).to_pylist()) == encoded(reference)
    controls += 1
print(f'Editable installation: source redirect/import, 11 analytic values, 3 thread counts and {controls} expression-limit controls pass')
