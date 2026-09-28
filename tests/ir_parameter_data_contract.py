"""Public model-file binding contract: independent recurrence and schema checks."""
import argparse
import copy
import csv
import io
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def model():
    result = json.loads((ROOT / 'models/decay.ir.json').read_text())
    result['time'] = {'unit': 'day', 'dt': 1, 'horizon': 4}
    result['data'] = [{
        'id': 'rates', 'source': 'inputs/rates.csv', 'required': True,
        'schema': {'key_column': 'practice', 'columns': [
            {'name': 'practice', 'type': 'string', 'unit': ''},
            {'name': 'rate', 'type': 'f64', 'unit': '1/day'}]},
        'use': {'kind': 'parameter_table', 'key': 'disputes',
                'parameters': [{'parameter': 'decay_rate', 'column': 'rate'}]}}]
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path)
    parser.add_argument('--without-arrow', action='store_true')
    parser.add_argument('--schema', action='store_true')
    args = parser.parse_args()
    exe = args.executable.resolve()
    validator = None
    if args.schema:
        from jsonschema import Draft202012Validator
        schema = json.loads((ROOT / 'ir/schema/ankurafathom-ir.schema.json').read_text())
        Draft202012Validator.check_schema(schema)
        validator = Draft202012Validator(schema)
    calls = 0
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / 'inputs').mkdir()
        source = root / 'inputs/rates.csv'
        source.write_text('practice,rate\nforensics,0.5\ndisputes,0.25\n')
        path = root / 'model.json'
        base = model()

        def call(doc, command='lint', extra=(), code=None, pointer=None):
            nonlocal calls
            calls += 1
            if validator and code is None:
                validator.validate(doc)
            path.write_text(json.dumps(doc))
            result = subprocess.run([str(exe), command, str(path), *map(str, extra)],
                                    cwd=root.parent, capture_output=True, text=True)
            if code:
                assert result.returncode == 1, (code, result.stdout, result.stderr)
                diagnostic = json.loads(result.stderr)['diagnostics'][0]
                assert diagnostic['code'] == code, diagnostic
                if pointer:
                    assert diagnostic['pointer'] == pointer, diagnostic
            else:
                assert result.returncode == 0, result.stderr
            return result.stdout

        if args.without_arrow:
            call(base, code='DATA_UNAVAILABLE', pointer='/data/0/source')
            no_data = copy.deepcopy(base)
            del no_data['data']
            call(no_data, 'run')
            print('CSV-only build rejects bound data and preserves ordinary models')
            return
        call(base)
        experiment = root / 'experiment.json'
        experiment.write_text(json.dumps({'seed': 17, 'replications': 3, 'scenarios': [
            {'id': 0, 'parameters': {}}, {'id': 1, 'parameters': {'decay_rate': 0.5}}]}))
        references = {}
        for integrator in ('euler', 'rk4'):
            document = copy.deepcopy(base)
            document['integrator'] = integrator
            for threads in (1, 8, 32):
                output = call(document, 'run', ('--experiment', experiment, '--threads', threads))
                if threads == 1:
                    references[integrator] = output
                assert output == references[integrator]
                rows = list(csv.DictReader(io.StringIO(output)))
                assert len(rows) == 30
                for row in rows:
                    rate = .25 if int(row['scenario']) == 0 else .5
                    factor = 1-rate if integrator == 'euler' else 1-rate+rate**2/2-rate**3/6+rate**4/24
                    expected = 100 * factor**int(float(row['time']))
                    assert abs(float(row['value'])-expected) < 1e-12
        import pyarrow as pa
        import pyarrow.ipc as ipc
        import pyarrow.parquet as pq
        table = pa.table({'practice': ['disputes', 'forensics'], 'rate': [.25, .5]})
        pq.write_table(table, root / 'inputs/rates.parquet')
        with ipc.new_file(root / 'inputs/rates.arrow', table.schema) as writer:
            writer.write_table(table)
        for suffix in ('parquet', 'arrow'):
            document = copy.deepcopy(base)
            document['data'][0]['source'] = f'inputs/rates.{suffix}'
            assert call(document, 'run', ('--experiment', experiment)) == references['euler']
        # Exact typed key selection, including integers above float64 precision.
        for type_, key, text in [('bool', True, 'true'), ('i32', -2147483648, '-2147483648'),
                                ('i64', -9223372036854775808, '-9223372036854775808'),
                                ('i64', 9223372036854775807, '9223372036854775807'),
                                ('u64', 18446744073709551615, '18446744073709551615'),
                                ('f64', .125, '0.125'), ('string', '', '""')]:
            (root / 'inputs/key.csv').write_text(f'practice,rate\n{text},0.25\n')
            document = copy.deepcopy(base)
            binding = document['data'][0]
            binding['source'] = 'inputs/key.csv'
            binding['schema']['columns'][0]['type'] = type_
            binding['use']['key'] = key
            assert call(document, 'run', ('--experiment', experiment)) == references['euler']
        # Equivalent dimensional spelling is accepted; no scale conversions.
        document = copy.deepcopy(base)
        document['data'][0]['schema']['columns'][1]['unit'] = 'kg/day/kg'
        call(document)
        # Multiple independent bindings, with one parameter left at its literal default.
        document = copy.deepcopy(base)
        document['parameters'] += [{'id': 'other', 'value': 9, 'unit': '1/day'},
                                   {'id': 'unbound', 'value': 7, 'unit': '1'}]
        second = copy.deepcopy(document['data'][0]); second['id'] = 'second'
        second['use']['key'] = 'forensics'; second['use']['parameters'][0]['parameter'] = 'other'
        document['data'].append(second)
        document['outputs'] += [{'id': 'other_ts', 'expr': 'other', 'unit': '1/day'},
                                {'id': 'unbound_ts', 'expr': 'unbound', 'unit': '1'}]
        output = list(csv.DictReader(io.StringIO(call(document, 'run'))))
        assert {float(r['value']) for r in output if r['output_id'] == 'other_ts'} == {.5}
        assert {float(r['value']) for r in output if r['output_id'] == 'unbound_ts'} == {7}
        # Output alias protection must preserve bytes for direct, symlink and hardlink aliases.
        symlink = root / 'alias.csv'; symlink.symlink_to(source)
        hardlink = root / 'hard.csv'; hardlink.hardlink_to(source)
        original = source.read_bytes()
        for output_path in (source, symlink, hardlink):
            call(base, 'run', ('--out', output_path), code='IR_USAGE', pointer='')
            assert source.read_bytes() == original
        structural = [
            (['data'], {}, 'IR_TYPE', '/data'), (['data'], [], 'IR_TYPE', '/data'),
            (['data',0,'extra'], True, 'IR_FIELD', '/data/0/extra'),
            (['data',0,'required'], False, 'IR_DATA', '/data/0/required'),
            (['data',0,'id'], 'bad-id', 'IR_ID', '/data/0/id'),
            (['data',0,'schema','columns',0,'type'], 'date', 'IR_TYPE', '/data/0/schema/columns/0/type'),
            (['data',0,'schema','columns',0,'categories'], 1, 'IR_TYPE', '/data/0/schema/columns/0/categories'),
            (['data',0,'schema','columns',0,'categories'], [1], 'IR_TYPE', '/data/0/schema/columns/0/categories'),
            (['data',0,'use','kind'], 'population_init', 'IR_DATA', '/data/0/use/kind'),
            (['data',0,'use','parameters'], [], 'IR_TYPE', '/data/0/use/parameters'),
            (['mode'], 'des', 'IR_DATA', '/data'), (['mode'], 'hybrid', 'IR_DATA', '/data'),
            (['mode'], 'abm', 'IR_ABM', '/components'), (['mode'], 'agent_stock_sd', 'IR_DATA', '/data')]
        semantic = [
            (['data',0,'schema','columns',0,'categories'], ['disputes', 'disputes'], 'DATA_SCHEMA', '/data/0/schema/columns/0'),
            (['data',0,'schema','columns',1,'categories'], ['bad'], 'DATA_SCHEMA', '/data/0/schema/columns/1'),
            (['data',0,'source'], 'https://example.invalid/data.csv', 'IR_DATA', '/data/0/source'),
            (['data',0,'source'], 'missing.csv', 'DATA_IO', '/data/0/source'),
            (['data',0,'source'], 'inputs/rates.csv\0other', 'IR_DATA', '/data/0/source'),
            (['data',0,'use','key'], 'missing', 'DATA_KEY', '/data/0/use/key'),
            (['data',0,'use','key'], None, 'IR_DATA_KEY', '/data/0/use/key'),
            (['data',0,'use','key'], True, 'IR_DATA_KEY', '/data/0/use/key'),
            (['data',0,'schema','key_column'], 'missing', 'IR_REF', '/data/0/schema/key_column'),
            (['data',0,'schema','columns',1,'unit'], '1/week', 'IR_UNIT', '/data/0/use/parameters/0/column'),
            (['data',0,'schema','columns',1,'type'], 'i64', 'IR_DATA', '/data/0/use/parameters/0/column'),
            (['data',0,'use','parameters',0,'parameter'], 'missing', 'IR_REF', '/data/0/use/parameters/0/parameter'),
            (['data',0,'use','parameters',0,'column'], 'missing', 'IR_DATA', '/data/0/use/parameters/0/column')]
        for steps, value, code, pointer in structural + semantic:
            document = copy.deepcopy(base); cursor = document
            for step in steps[:-1]: cursor = cursor[step]
            cursor[steps[-1]] = value
            if validator and (steps, value, code, pointer) in structural:
                assert not validator.is_valid(document), steps
            call(document, code=code, pointer=pointer)
        for type_, key in [('i32', 2147483648), ('i32', -2147483649), ('i64', 9223372036854775808),
                           ('i64', -9223372036854775809), ('u64', -1), ('u64', 18446744073709551616),
                           ('u64', 1.0), ('i64', 1.0), ('bool', 1), ('f64', True)]:
            document = copy.deepcopy(base)
            document['data'][0]['schema']['columns'][0]['type'] = type_
            document['data'][0]['use']['key'] = key
            call(document, code='IR_DATA_KEY', pointer='/data/0/use/key')
        for duplicate_id in (True, False):
            document = copy.deepcopy(base); second = copy.deepcopy(document['data'][0])
            if not duplicate_id: second['id'] = 'second'
            document['data'].append(second)
            call(document, code='IR_ID' if duplicate_id else 'IR_DATA',
                 pointer='/data/1/id' if duplicate_id else '/data/1/use/parameters/0/parameter')
        # Lint uses effective data values for model-dependent delay validation.
        delay = json.loads((ROOT / 'models/variable_delay.ir.json').read_text())
        delay['data'] = copy.deepcopy(base['data'])
        delay['data'][0]['schema']['columns'][1]['unit'] = 'day'
        delay['data'][0]['use']['parameters'][0]['parameter'] = 'tau'
        call(delay, code='IR_DELAY', pointer='/components/2/duration')
        delay['parameters'][1]['value'] = .25  # Invalid literal is replaced by a valid table value.
        source.write_text('practice,rate\ndisputes,2\n')
        call(delay)
        source.write_text('practice,rate\ndisputes,nan\n')
        call(base, code='DATA_VALUE', pointer='/data/0/rows/0/rate')
        print(f'{calls} CLI checks passed: formats, Euler/RK4, 1/8/32 threads, keys, precedence, aliases and diagnostics')


if __name__ == '__main__':
    main()
