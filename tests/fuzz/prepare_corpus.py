"""Copy stable seed inputs to writable, separate corpus directories."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('destination', type=Path)
args = parser.parse_args()
expression = args.destination/'expression'; expression.mkdir(parents=True, exist_ok=True)
loader = args.destination/'ir_loader'; loader.mkdir(parents=True, exist_ok=True)
for index, source in enumerate(('0', 'x + 2 * y', '1 / 0', 'NONNEGATIVE(-x)',
        'STEP(2,3)', 'PULSE(1,2)', 'RAMP(1,2,3)', 'XMILE_NEXT_PULSE(1,2,3,4)',
        'lookup(t)', '(1 + 2', 'a\0b', '1e309', '0x1p-1074',
        'PULSE(1e100,2)', 'RAMP(1e308,-1e308,1e308)', 'MIN(1,2)',
        'MAX(-1,2)', 'IF_POSITIVE(0,1/0,2)', 'IF_POSITIVE(1,2,1/0)')):
    (expression/f'seed-{index}').write_bytes(source.encode())
# Deliberately deep valid syntax checks parser and AST cleanup/evaluation limits.
for name, source in [('nested', '('*4096+'1'+')'*4096),
                     ('unary', '-'*8192+'1'), ('chain', '+'.join(['1']*8192))]:
    (expression/name).write_text(source)
for name in ('decay.ir.json', 'stochastic_process.ir.json', 'typed_abm_rates.ir.json',
             'agent_pool.ir.json', 'agent_stock_sd.ir.json', 'hybrid_completion.ir.json'):
    (loader/name).write_bytes((ROOT/'models'/name).read_bytes())
(loader/'nested-json').write_text('{"invalid":'+'['*4096+'0'+']'*4096+'}')
(loader/'empty').write_text('{}')
document = json.loads((ROOT/'models/decay.ir.json').read_text())
document['components'][1]['expr'] = '-'*8192+'1'
(loader/'nested-expression').write_text(json.dumps(document))
