"""Design expansion through the CLI, analytic outputs, and optional schema parity."""
import copy
import csv
import gzip
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCHEMA_MODE = '--schema' in sys.argv
ARGS = [a for a in sys.argv[1:] if a!='--schema']
EXE = str(Path(ARGS[0]).resolve())
if SCHEMA_MODE:
    from jsonschema import Draft202012Validator
    VALIDATOR = Draft202012Validator(json.loads((ROOT/'ir/schema/ankurafathom-experiment.schema.json').read_text()))


def require(ok, why):
    if not ok:
        raise ValueError(why)


def run(model, experiment, *options):
    return subprocess.run([EXE, 'run', str(model), '--no-manifest', '--experiment', str(experiment), *map(str, options)], capture_output=True)


def model(names):
    document = dict(ir_version='0.1', name='design_parameter_probe', time=dict(unit='day', dt=.5, horizon=1),
                    parameters=[], components=[], outputs=[])
    for name in names:
        document['parameters'].append(dict(id=name, value=0, unit='kg/day'))
        document['components'].extend([dict(id='s_'+name, kind='stock', init=0, non_negative=False, unit='kg'),
                                       dict(id='f_'+name, kind='flow', source=None, destination='s_'+name,
                                            expr=name, non_negative=False, unit='kg/day')])
        document['outputs'].append(dict(id=name+'_ts', stock='s_'+name))
    return document


def main():
    plan = json.loads((ROOT/'tests/oracles/runtime/design-plan.json').read_text())
    refs = json.loads(gzip.decompress((ROOT/'tests/oracles/runtime/design-reference.json.gz').read_bytes()))
    selected = ['grid_0', 'grid_3', 'lhs_3_7_0', f'lhs_32_64_{2**64-1}', 'sobol_8_64', 'sobol_1_1']
    checks = 0;invalid = 0;semantic = 0
    with tempfile.TemporaryDirectory(prefix='fathom-design-') as directory:
        root = Path(directory);mp = root/'model.json';ep = root/'experiment.json';out = root/'result.csv'
        for case in (c for c in plan['cases'] if c['id'] in selected):
            names = sorted(case.get('axes', case.get('bounds')))
            mp.write_text(json.dumps(model(names)))
            design = {k: v for k, v in case.items() if k!='id'}
            definition = dict(seed=42, replications=2, design=design)
            if SCHEMA_MODE:
                require(VALIDATOR.is_valid(definition), 'valid design rejected by schema')
            ep.write_text(json.dumps(definition))
            serial = run(mp, ep, '--threads', 1);parallel = run(mp, ep, '--threads', 7)
            require(serial.returncode==parallel.returncode==0, (serial.stderr+parallel.stderr).decode())
            require(serial.stdout==parallel.stdout, 'design changed with execution thread count')
            expected = {s: dict(zip(names, values)) for s, values in refs[case['id']]}
            rows = list(csv.DictReader(io.StringIO(serial.stdout.decode())))
            require(len(rows)==len(expected)*2*3*len(names), 'design output count')
            for r in rows:
                target = float(r['time'])*expected[int(r['scenario'])][r['output_id'][:-3]]
                require(abs(float(r['value'])-target)<=2e-13, 'design parameter analytic output mismatch');checks += 1
        base = dict(seed=42, replications=1, design=dict(kind='lhs', count=7, first_id=0, design_seed=0, bounds={'x': [0, 1]}))
        mp.write_text(json.dumps(model(['x'])))
        sentinel = b'previous complete result\n';out.write_bytes(sentinel)
        def bad(change, structural, code=None):
            nonlocal invalid, semantic
            value = copy.deepcopy(base);change(value);ep.write_text(json.dumps(value))
            if SCHEMA_MODE:
                require(VALIDATOR.is_valid(value)==structural, 'schema classification differs')
            response = run(mp, ep, '--out', out)
            require(response.returncode!=0 and not response.stdout and out.read_bytes()==sentinel, 'invalid design published results')
            if code:
                require(json.loads(response.stderr)['diagnostics'][0]['code']==code, response.stderr.decode())
            invalid += 1;semantic += int(structural)
        bad(lambda d: d.update(scenarios=[dict(id=0, parameters={})]), False, 'IR_EXPERIMENT')
        bad(lambda d: d.pop('design'), False, 'IR_EXPERIMENT')
        bad(lambda d: d['design'].update(kind='unknown'), False, 'IR_EXPERIMENT')
        bad(lambda d: d['design'].update(count=0), False, 'IR_TYPE')
        bad(lambda d: d['design'].update(count=1.0), True, 'IR_TYPE')
        bad(lambda d: d['design'].update(first_id=65535, count=2), True, 'IR_EXPERIMENT')
        bad(lambda d: d['design'].update(bounds={'x': [1, 1]}), True, 'IR_EXPERIMENT')
        bad(lambda d: d['design'].update(bounds={'x': [2, 1]}), True, 'IR_EXPERIMENT')
        bad(lambda d: d['design'].update(bounds={'x': [0]}), False, 'IR_TYPE')
        bad(lambda d: d['design'].update(bounds={'unknown': [0, 1]}), True, 'IR_OVERRIDE')
        bad(lambda d: d['design'].update(design_seed=42), True, 'IR_EXPERIMENT')
        bad(lambda d: d['design'].update(design_seed=-1), False, 'IR_TYPE')
        bad(lambda d: d['design'].update(scramble=True), False, 'IR_FIELD')
        bad(lambda d: d.update(design=dict(kind='sobol', count=3, bounds={'x': [0, 1]})), False, 'IR_EXPERIMENT')
        bad(lambda d: d.update(design=dict(kind='grid', axes={'x': []})), False, 'IR_EXPERIMENT')
        bad(lambda d: d.update(design=dict(kind='grid', axes={'x': [True]})), False, 'IR_TYPE')
        bad(lambda d: d.update(design=dict(kind='grid', axes={})), False, 'IR_EXPERIMENT')
        ep.write_text(json.dumps(base))
        result = run(mp, ep, '--seed', 0, '--out', out)
        require(result.returncode!=0 and json.loads(result.stderr)['diagnostics'][0]['code']=='IR_EXPERIMENT' and out.read_bytes()==sentinel, 'seed override bypassed design-key separation')
        report = dict(passed=True, designs=len(selected), analytic_observations=checks, invalid_cases=invalid,
                      semantic_cases=semantic, seed_override_guard=True)
        if len(ARGS)>1:
            Path(ARGS[1]).write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report))

if __name__=='__main__':
    main()
