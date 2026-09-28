"""Decode C++ result files with PyArrow and compare ordered IEEE-754 values.

CSV-only mode uses just the standard library and verifies fail-closed format
selection. Parquet bytes are deliberately not used as the determinism contract.
"""
import argparse
import csv
import io
import json
from pathlib import Path
import resource
import signal
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BITS = [0, 0x8000000000000000, 1, 0x8000000000000001, 0x0010000000000000,
        0x7fefffffffffffff, 0xffefffffffffffff, 0x3fb999999999999a, 0x3ff0000000000001]


def require(ok, why):
    if not ok:
        raise ValueError(why)


def csv_rows(data):
    return [(int(r.get('scenario', 0)), int(r.get('replication', 0)),
             struct.pack('<d', float(r['time'])), r['output_id'],
             struct.pack('<d', float(r['value'])))
            for r in csv.DictReader(io.StringIO(data.decode()))]


def check_schema(table):
    import pyarrow as pa
    expected = pa.schema([pa.field('scenario', pa.uint32(), False),
                          pa.field('replication', pa.uint32(), False),
                          pa.field('time', pa.float64(), False),
                          pa.field('output_id', pa.string(), False),
                          pa.field('value', pa.float64(), False)],
                         metadata={b'ankurafathom.schema_version': b'0.1',
                                   b'ankurafathom.table': b'observations'})
    require(table.schema.equals(expected, check_metadata=True), 'result schema/metadata changed')
    require(all(c.null_count == 0 for c in table.columns), 'null result values')
    table.validate(full=True)


def table_rows(table):
    check_schema(table)
    return [(s, r, struct.pack('<d', t), name, struct.pack('<d', value))
            for s, r, t, name, value in zip(*(c.to_pylist() for c in table.columns))]


def decode(path, format):
    import pyarrow as pa
    import pyarrow.parquet as pq
    if format == 'parquet':
        return pq.read_table(path)
    with pa.memory_map(str(path), 'r') as source:
        return pa.ipc.open_file(source).read_all()


def corruption_controls(root):
    import pyarrow as pa
    original = decode(root / 'values.arrow', 'arrow')
    expected = table_rows(original)
    bad_metadata = original.replace_schema_metadata({b'ankurafathom.schema_version': b'wrong'})
    nullable = pa.Table.from_arrays(original.columns, schema=pa.schema([
        pa.field(f.name, f.type, True) for f in original.schema], metadata=original.schema.metadata))
    wrong_type = original.set_column(0, pa.field('scenario', pa.int64(), False), original.column(0).cast(pa.int64()))
    wrong_order = original.take(pa.array(list(reversed(range(original.num_rows)))))
    missing = original.slice(0, original.num_rows - 1)
    changed_bits = original.column(4).to_pylist();changed_bits[1] = 0.0
    signed_zero = original.set_column(4, original.schema.field(4), pa.array(changed_bits, type=pa.float64()))
    changed_bits = original.column(4).to_pylist();changed_bits[2] = 0.0
    subnormal = original.set_column(4, original.schema.field(4), pa.array(changed_bits, type=pa.float64()))
    changed_ids = original.column(0).to_pylist();changed_ids[-1] = 0
    wrong_address = original.set_column(0, original.schema.field(0), pa.array(changed_ids, type=pa.uint32()))
    with_null = original.column(4).to_pylist();with_null[0] = None
    null = original.set_column(4, original.schema.field(4), pa.array(with_null, type=pa.float64()))
    for corrupted in (bad_metadata, nullable, wrong_type, wrong_order, missing, signed_zero, subnormal, wrong_address, null):
        rejected = False
        try:
            require(table_rows(corrupted) == expected, 'corrupted rows differ')
        except (ValueError, pa.ArrowInvalid):
            rejected = True
        require(rejected, 'output oracle accepted corrupted evidence')
    return 9


def native_fixtures(root):
    import pyarrow as pa
    import pyarrow.parquet as pq
    expected = [(s, r, struct.pack('<d', i * .125), 'value,"quoted"\n\r\té', struct.pack('<Q', bits))
                for s, r in [(0, 0), (65535, 65535)] for i, bits in enumerate(BITS)]
    require(csv_rows((root / 'values.csv').read_bytes()) == expected, 'native CSV bit/string preservation')
    for format in ('parquet', 'arrow'):
        require(table_rows(decode(root / f'values.{format}', format)) == expected, 'native columnar roundtrip')
        require(table_rows(decode(root / f'empty.{format}', format)) == [], 'empty result lost typed schema')
        boundary = table_rows(decode(root / f'boundary.{format}', format))
        wanted = [(65535, 65535, struct.pack('<d', i * .125), 'boundary', struct.pack('<Q', BITS[i % len(BITS)]))
                  for i in range(65539)]
        require(boundary == wanted, 'row group/batch boundary bit loss')
    require(pq.ParquetFile(root / 'boundary.parquet').num_row_groups == 2, 'row group fixture did not cross boundary')
    with pa.memory_map(str(root / 'boundary.arrow'), 'r') as source:
        require(pa.ipc.open_file(source).num_record_batches == 2, 'IPC fixture did not cross boundary')
    return len(expected) * 3 + 65539 * 2


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path)
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--without-arrow', action='store_true')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    executable = str(args.executable.resolve())

    def run(model, *options):
        return subprocess.run([executable, 'run', str(model), '--no-manifest', *map(str, options)], capture_output=True)

    def success(model, *options):
        result = run(model, *options)
        require(result.returncode == 0, result.stderr.decode())
        return result.stdout

    def failure(code, model, *options):
        result = run(model, *options)
        require(result.returncode != 0 and not result.stdout, 'failed run published stdout')
        require(json.loads(result.stderr)['diagnostics'][0]['code'] == code, result.stderr.decode())

    comparisons = 0
    controls = 0
    if not args.without_arrow:
        require(args.fixtures is not None, 'native fixtures required')
        comparisons += native_fixtures(args.fixtures)
        controls = corruption_controls(args.fixtures)
    with tempfile.TemporaryDirectory(prefix='fathom-output-contract-') as directory:
        root = Path(directory)
        model = ROOT / 'models/decay.ir.json'
        destination = root / 'previous.parquet'
        sentinel = b'previous complete result\n'
        destination.write_bytes(sentinel)
        for options in [('--format', 'invalid'), ('--format', 'csv', '--format', 'arrow')]:
            failure('IR_USAGE', model, *options, '--out', destination)
        for format in ('parquet', 'arrow'):
            failure('IR_USAGE', model, '--format', format)
        if args.without_arrow:
            for suffix in ('parquet', 'arrow', 'ipc'):
                failure('IR_OUTPUT_UNAVAILABLE', model, '--out', root / f'result.{suffix}')
            for format in ('parquet', 'arrow'):
                failure('IR_OUTPUT_UNAVAILABLE', model, '--out', destination, '--format', format)
            require(destination.read_bytes() == sentinel, 'unavailable output replaced destination')
            success(model, '--format', 'csv', '--out', destination)
            require(destination.read_bytes() == success(model), 'explicit CSV override changed values')
        else:
            # Independent closed form in addition to comparison against CSV output.
            spec = json.loads(model.read_text())
            # The IR restricts identifiers to ASCII names; arbitrary UTF-8 and
            # CSV quoting are exercised directly by the native API fixtures.
            quoted = root / 'model.json';quoted.write_text(json.dumps(spec))
            csv_single = csv_rows(success(quoted))
            for _, _, time, name, value in csv_single:
                t = struct.unpack('<d', time)[0];v = struct.unpack('<d', value)[0]
                require(abs(v - 100 * .98 ** round(t / .1)) < 3e-13, 'decay analytical oracle mismatch')
                require(name == 'material_ts', 'output identifier changed')
            for format in ('parquet', 'arrow'):
                path = root / f'single.{format}'
                require(success(quoted, '--out', path) == b'', 'file run emitted stdout')
                require(table_rows(decode(path, format)) == csv_single, 'single-run output mismatch')
                comparisons += len(csv_single)
            # Stochastic workload: freeze 1,000 scenario addresses and compare
            # all bits at 1/8/64 workers, in both binary formats.
            spec = json.loads((ROOT / 'models/stochastic_process.ir.json').read_text())
            spec['time'].update(dt=.5, horizon=4)
            spec['components'][0]['exponential']['count'] = 6
            stochastic = root / 'stochastic.json';stochastic.write_text(json.dumps(spec))
            experiment = root / 'experiment.json'
            definition = dict(seed=2**64-1, replications=3,
                              scenarios=[dict(id=i, parameters={}) for i in range(1000)])
            experiment.write_text(json.dumps(definition))
            baseline = csv_rows(success(stochastic, '--experiment', experiment))
            require(len({(r[0], r[1]) for r in baseline}) == 3000, 'trajectory coverage')
            for threads in (1, 8, 64):
                for format in ('parquet', 'arrow'):
                    path = root / f'ensemble.{format}'
                    success(stochastic, '--experiment', experiment, '--threads', threads, '--out', path)
                    require(table_rows(decode(path, format)) == baseline, 'thread-count/format changed numeric bits or order')
                    comparisons += len(baseline)
            # All exposed execution families use the same output adapter.
            small = root / 'small.json'
            small.write_text(json.dumps(dict(seed=73, replications=2, scenarios=[dict(id=i, parameters={}) for i in (0, 65535)])))
            for fixture in ('abm_adoption', 'agent_stock_sd', 'typed_abm_async', 'typed_abm_rates',
                            'typed_stochastic_process', 'typed_resource_process'):
                path = ROOT / 'models' / f'{fixture}.ir.json'
                expected = csv_rows(success(path, '--experiment', small, '--seed', 2**64-1))
                for format in ('parquet', 'arrow'):
                    out = root / f'mode.{format}'
                    success(path, '--experiment', small, '--threads', 8, '--seed', 2**64-1, '--out', out)
                    require(table_rows(decode(out, format)) == expected, 'execution-mode output mismatch: ' + fixture)
                    comparisons += len(expected)
            # Output paths cannot alias model/experiment inputs. Failure at model
            # execution or final rename must preserve the previous destination.
            original = quoted.read_bytes()
            for name in ('symlink.parquet', 'hardlink.arrow'):
                alias = root / name
                if name.startswith('sym'):
                    alias.symlink_to(quoted)
                else:
                    alias.hardlink_to(quoted)
                failure('IR_USAGE', quoted, '--out', alias)
                require(quoted.read_bytes() == original, 'input alias changed')
            failure('IR_USAGE', model, '--experiment', small, '--out', small, '--format', 'parquet')
            failed = root / 'failed.json'
            failed.write_text(json.dumps(dict(seed=0, replications=1, scenarios=[
                dict(id=0, parameters={'decay_rate': .2}), dict(id=1, parameters={'decay_rate': 100})])))
            for format in ('parquet', 'arrow'):
                destination.write_bytes(sentinel)
                failure('IR_RUNTIME', model, '--experiment', failed, '--threads', 8, '--out', destination, '--format', format)
                require(destination.read_bytes() == sentinel, 'failed ensemble changed result')
                folder = root / f'directory.{format}';folder.mkdir();(folder / 'keep').write_bytes(sentinel)
                failure('IR_IO', model, '--out', folder)
                require((folder / 'keep').read_bytes() == sentinel, 'failed rename changed directory')
                failure('IR_IO', model, '--out', root / 'missing' / f'file.{format}')
                # Restrict only the child process. EFBIG exercises a real I/O
                # failure after encoding starts, without filling the host disk.
                def limit_file_size():
                    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
                    resource.setrlimit(resource.RLIMIT_FSIZE, (128, 128))
                result = subprocess.run([executable, 'run', str(model), '--no-manifest', '--out', str(destination), '--format', format],
                                        capture_output=True, preexec_fn=limit_file_size)
                require(result.returncode != 0 and not result.stdout, 'limited writer published stdout')
                require(json.loads(result.stderr)['diagnostics'][0]['code'] == 'IR_IO', result.stderr.decode())
                require(destination.read_bytes() == sentinel, 'mid-write failure replaced previous result')
            # Explicit format overrides extensions; .ipc selects an IPC file.
            success(model, '--out', destination, '--format', 'csv')
            require(destination.read_bytes() == success(model), 'explicit CSV selection ignored')
            odd = root / 'result.data';success(model, '--out', odd, '--format', 'parquet')
            require(table_rows(decode(odd, 'parquet')) == csv_rows(success(model)), 'explicit Parquet selection ignored')
            ipc = root / 'result.ipc';success(model, '--out', ipc)
            require(table_rows(decode(ipc, 'arrow')) == csv_rows(success(model)), 'IPC suffix ignored')
        require(not list(root.glob('.fathom-*.tmp-*')), 'temporary result leak')
    report = dict(passed=True, arrow=not args.without_arrow, decoded_row_comparisons=comparisons,
                  corruption_controls=controls,
                  thread_counts=[] if args.without_arrow else [1, 8, 64],
                  bit_preservation=True, failure_safe_publication=True)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
