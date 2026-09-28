"""Python product binding: exact C parity, ownership, diagnostics and concurrent use."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import gc
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
def require_error(kind, function):
    try:
        function()
    except kind as error:
        return error
    raise AssertionError(f'expected {kind.__name__}')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('package', type=Path)
    parser.add_argument('probe', type=Path)
    parser.add_argument('--without-arrow', action='store_true')
    args = parser.parse_args()
    sys.path.insert(0, str(args.package.resolve()))
    import ankurafathom as af
    import pyarrow as pa
    from ankurafathom import _native
    comparisons = observations = controls = 0
    path = ROOT/'models/decay.ir.json'
    source = json.loads(path.read_text())
    model = af.Model.from_json(source)
    assert af.lint(source) == {'verdict': 'pass', 'diagnostics': []}
    assert af.lint(b'{')['verdict'] == 'fail'
    for raw in (b'{', b'{}{}', b'{}\0bad', b'', b'[]'):
        error = require_error(af.FathomError, lambda: af.Model.from_json(raw))
        assert error.status == 2 and error.code.startswith('IR_') and isinstance(error.pointer, str)
        controls += 1
    bad = dict(source, unknown=1)
    error = require_error(af.FathomError, lambda: af.Model.from_json(bad))
    assert error.pointer == '/unknown'
    # Error records survive later successful calls and other thread-local calls.
    af.Model.from_json(source)
    assert error.pointer == '/unknown' and error.status == 2
    long_key = '🙂' * 600
    error = require_error(af.FathomError, lambda: af.Model.from_json(dict(source, **{long_key: 1})))
    assert error.truncated & 2 and error.pointer.startswith('/🙂') and error.pointer.endswith('\ufffd')
    controls += 2
    for value in (None, 1, [], bytearray(b'{}')):
        require_error(TypeError, lambda: af.Model.from_json(value)); controls += 1
    require_error(TypeError, af.Model); controls += 1
    require_error(ValueError, lambda: af.Model.from_json(source, base_directory='bad\0base')); controls += 1
    require_error(af.FathomError, lambda: af.Model.from_json(source, base_directory='relative')); controls += 1
    require_error(ValueError, lambda: af.Model.from_json(dict(source, invalid=float('nan')))); controls += 1
    for threads in (0, 257, -1, True, 1.0, '1'):
        require_error(ValueError, lambda: af.run(model, threads=threads)); controls += 1
    for seed in (-1, 1 << 64, True, 1.0, '1'):
        require_error(ValueError, lambda: af.run(model, seed=seed)); controls += 1
    require_error(TypeError, lambda: af.run(source)); controls += 1
    require_error(TypeError, lambda: af.run(model, {})); controls += 1
    bad_experiment = af.Experiment({'seed': 0, 'replications': 1, 'scenarios': [
        {'id': 0, 'parameters': {'missing': 1}}]})
    error = require_error(af.FathomError, lambda: af.run(model, bad_experiment))
    assert error.code == 'IR_OVERRIDE' and error.pointer == '/experiment/scenarios/0/parameters/missing'
    controls += 1
    require_error(af.FathomError, lambda: af.run(model, af.Experiment(b''))); controls += 1
    if args.without_arrow:
        for _ in range(4):
            error = require_error(af.FathomError, lambda: af.run(model))
            assert error.status == 7 and error.code == 'FATHOM_UNAVAILABLE'
            controls += 1
        assert af.lint(source)['verdict'] == 'pass'
        print(f'Python CSV-only: {controls} controls, structured unavailable export and reusable models pass')
        return

    def bits(value):
        return struct.unpack('<Q', struct.pack('<d', value))[0]
    def encoded(table):
        table.validate(full=True)
        assert table.schema.metadata == {b'ankurafathom.schema_version': b'0.1', b'ankurafathom.table': b'observations'}
        assert table.schema.names == ['scenario', 'replication', 'time', 'output_id', 'value']
        assert [f.type for f in table.schema] == [pa.uint32(), pa.uint32(), pa.float64(), pa.string(), pa.float64()]
        assert all(not f.nullable for f in table.schema)
        return [(r['scenario'], r['replication'], bits(r['time']), r['output_id'], bits(r['value']))
                for r in table.to_pylist()]
    def expected(model_path, exp_path, threads, seed):
        result = subprocess.run([str(args.probe.resolve()), str(model_path), str(exp_path), str(threads),
            'file', str(model_path.parent), '-' if seed is None else str(seed)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        header, *lines = result.stdout.splitlines()
        rows = []
        for line in lines:
            scenario, replication, time, value, name = line.split()
            rows.append((int(scenario), int(replication), int(time, 16), bytes.fromhex(name).decode(), int(value, 16)))
        assert len(rows) == int(header.split()[1])
        return rows
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        spec = {'seed': 42, 'replications': 2, 'scenarios': [
            {'id': 17, 'parameters': {}}, {'id': 65535, 'parameters': {}}]}
        experiment = af.Experiment(spec)
        exp_path = tmp/'experiment.json'; exp_path.write_text(json.dumps(spec))
        spec['replications'] = 19  # The Experiment retains its original snapshot.
        fixtures = [ROOT/'models'/name for name in (
            'decay.ir.json', 'stochastic_process.ir.json', 'typed_abm_rates.ir.json',
            'agent_stock_sd.ir.json', 'agent_pool.ir.json', 'hybrid_completion.ir.json',
            'data/parameter_decay.ir.json', 'data/seasonal_stock.ir.json', 'data/workforce.ir.json')]
        for model_path in fixtures:
            # All public JSON forms are accepted, retaining the same captured model.
            for raw in (model_path.read_text(), model_path.read_bytes(), json.loads(model_path.read_text())):
                loaded = af.Model.from_json(raw, base_directory=model_path.parent)
                for exp, ep, threads, seed in ((None, '-', 1, None), (experiment, exp_path, 1, 0),
                        (experiment, exp_path, 8, (1 << 64) - 1), (experiment, exp_path, 32, 42)):
                    table = af.run(loaded, exp, threads=threads, seed=seed)
                    rows = encoded(table)
                    assert rows == expected(model_path, ep, threads, seed), (model_path, threads, seed)
                    comparisons += 1; observations += len(rows)
        for kind in ('grid', 'lhs', 'sobol'):
            ep = ROOT/f'models/decay.{kind}.experiment.json'
            table = af.run(model, af.Experiment(ep.read_bytes()), threads=8)
            rows = encoded(table); assert rows == expected(path, ep, 8, None)
            comparisons += 1; observations += len(rows)
        edge = dict(source)
        edge['time'] = {'unit': 'day', 'dt': 1, 'horizon': 17000}
        values = [-0.0, 5e-324, -5e-324, 1.7976931348623157e308]
        edge['components'] = [{'id': f's{i}', 'kind': 'stock', 'init': value, 'non_negative': False, 'unit': 'kg'} for i, value in enumerate(values)]
        edge['outputs'] = [{'id': f'out{i}', 'stock': f's{i}'} for i in range(4)]
        edge_path = tmp/'edge.json'; edge_path.write_text(json.dumps(edge))
        owned = af.Model.from_json(edge)
        retained = af.run(owned); del owned; gc.collect()
        rows = encoded(retained)
        assert [r[4] for r in rows[:4]] == [bits(v) for v in values]
        assert retained.column('time').num_chunks > 1
        assert rows == expected(edge_path, '-', 1, None)
        comparisons += 1; observations += len(rows)
        sliced = retained.slice(65530, 20); del retained; gc.collect()
        assert encoded(sliced) == rows[65530:65550]
        controls += 1
        # Loading owns Python input contents; later dict changes cannot affect a run.
        snapshot = json.loads(path.read_text()); owned = af.Model.from_json(snapshot)
        snapshot['components'][0]['init'] = 999
        assert af.run(owned).column('value')[0].as_py() == 100
        controls += 1
        # Concurrent native execution shares an immutable model and keeps errors isolated.
        def work(index):
            if index % 2:
                error = require_error(af.FathomError, lambda: af.run(model, bad_experiment))
                return error.code, error.pointer
            return encoded(af.run(model, experiment, threads=8, seed=42))
        reference = encoded(af.run(model, experiment, threads=1, seed=42))
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(work, range(16)))
        for i, result in enumerate(results):
            assert result == (('IR_OVERRIDE', '/experiment/scenarios/0/parameters/missing') if i % 2 else reference)
        controls += 16
        # Capsule destruction covers abandonment and importer failure as well as success.
        for _ in range(8):
            capsule = _native.run_stream(model._handle)
            del capsule
        class RejectImporter:
            @staticmethod
            def from_stream(stream):
                raise RuntimeError('injected importer failure')
        with patch.object(pa, 'RecordBatchReader', RejectImporter):
            for _ in range(8):
                error = require_error(RuntimeError, lambda: af.run(model))
                assert str(error) == 'injected importer failure'
        gc.collect(); controls += 16
        assert encoded(af.run(model)) == expected(path, '-', 1, None)
    print(f'Python binding: {comparisons} C-ABI comparisons / {observations} exact observations; {controls} controls pass')

if __name__ == '__main__':
    main()
