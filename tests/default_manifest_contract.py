"""Default file-run receipts, opt-out compatibility, replay and publication safety."""
import argparse
import csv
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from provenance_cli_contract import records_identity
from output_provenance_contract import COLUMNS, check_rows, check_table

ROOT = Path(__file__).resolve().parents[1]


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
        validator = Draft202012Validator(json.loads((ROOT/'ir/schema/ankurafathom-manifest.schema.json').read_text()))
    runs = calls = controls = 0
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        def call(*options, error=None):
            nonlocal calls
            calls += 1
            result = subprocess.run([str(exe), *map(str, options)], cwd=root, capture_output=True, text=True)
            if error:
                assert result.returncode == 1 and not result.stdout, result
                assert json.loads(result.stderr)['diagnostics'][0]['code'] == error, result.stderr
            else:
                assert result.returncode == 0 and not result.stderr, result.stderr
            return result.stdout

        def sidecar(path):
            return Path(str(path) + '.manifest.json')

        model = root/'model.json'
        shutil.copyfile(ROOT/'models/decay.ir.json', model)
        # Standard output stays legacy CSV and creates no files; the opt-out is
        # also allowed for stdout. This is deliberate, not an implicit receipt path.
        before = sorted(root.iterdir())
        stdout = call('run', model)
        assert stdout == call('run', model, '--no-manifest')
        assert sorted(root.iterdir()) == before
        controls += 1

        fixtures = ['decay.ir.json', 'stochastic_process.ir.json', 'typed_abm_rates.ir.json',
                    'agent_pool.ir.json', 'agent_stock_sd.ir.json', 'hybrid_completion.ir.json']
        if not args.without_arrow:
            fixtures += ['data/parameter_decay.ir.json', 'data/seasonal_stock.ir.json', 'data/workforce.ir.json']
        formats = ['csv'] if args.without_arrow else ['csv', 'parquet', 'arrow']
        experiment = root/'experiment.json'
        experiment.write_text(json.dumps({'seed': 42, 'replications': 2, 'scenarios': [
            {'id': 17, 'parameters': {}}, {'id': 65535, 'parameters': {}}]}))
        for number, fixture in enumerate(fixtures):
            source = ROOT/'models'/fixture
            for ensemble in (False, True):
                options = ['--experiment', experiment, '--threads', 8] if ensemble else []
                baseline = call('run', source, *options, '--seed', 42)
                expected_hash = records_identity(csv.DictReader(io.StringIO(baseline)))
                for format in formats:
                    # Relative output paths, spaces and full suffix preservation.
                    output = root/f'{number} {ensemble}.{format}'
                    assert call('run', source, '--out', output.name, *options, '--seed', 42) == ''
                    receipt = sidecar(output)
                    manifest = json.loads(receipt.read_text())
                    assert manifest['manifest_version'] == '0.2'
                    assert manifest['output'] == {'path': str(output), 'format': format, 'schema_version': '0.2'}, manifest['output']
                    assert manifest['result']['sha256'] == expected_hash
                    if validator:
                        validator.validate(manifest)
                    if format == 'csv':
                        reader = csv.DictReader(io.StringIO(output.read_text()))
                        rows = list(reader)
                        assert reader.fieldnames == COLUMNS
                        check_rows(rows, manifest)
                    else:
                        import pyarrow as pa
                        import pyarrow.parquet as pq
                        table = pq.read_table(output) if format == 'parquet' else pa.ipc.open_file(output).read_all()
                        check_table(table, manifest)
                        rows = table.to_pylist()
                        assert json.loads(call('verify-results', output, '--embedded'))['verdict'] == 'pass'
                    assert records_identity(rows) == expected_hash
                    assert json.loads(call('verify-results', receipt))['verdict'] == 'pass'
                    assert json.loads(call('replay', receipt, '--threads', 1))['verdict'] == 'pass'
                    previous = output.read_bytes(), receipt.read_bytes()
                    call('run', source, '--out', output, *options, error='IR_USAGE')
                    assert previous == (output.read_bytes(), receipt.read_bytes())
                    runs += 1
                    # Explicit opt-out retains the original output schema and
                    # creates no sidecar; complete output is byte-stable for CSV.
                    legacy = root/f'legacy-{number}-{ensemble}.{format}'
                    call('run', source, '--no-manifest', '--out', legacy, *options, '--seed', 42)
                    assert not sidecar(legacy).exists()
                    if format == 'csv':
                        assert legacy.read_text() == baseline
                    else:
                        table = pq.read_table(legacy) if format == 'parquet' else pa.ipc.open_file(legacy).read_all()
                        assert table.column_names == COLUMNS[:5]
                        assert table.schema.metadata[b'ankurafathom.schema_version'] == b'0.1'
                        assert records_identity(table.to_pylist()) == expected_hash

        # Explicit destinations override defaults; replay keeps its original
        # receipt/ID and writes no new sidecar at the replay output path.
        output = root/'explicit.csv'; receipt = root/'custom.json'
        call('run', model, '--out', output, '--manifest', receipt)
        assert receipt.exists() and not sidecar(output).exists()
        replayed = root/'replayed.csv'
        call('replay', receipt, '--out', replayed)
        assert replayed.read_bytes() == output.read_bytes() and not sidecar(replayed).exists()
        for options in [('--no-manifest', '--no-manifest'), ('--no-manifest', '--manifest', root/'x.json'),
                        ('--manifest', root/'x.json', '--no-manifest'), ('--no-manifest', 'true'),
                        ('--manifest',), ('--out',), ('--manifest', ''), ('--out', ''),
                        ('--out', '--no-manifest'), ('--manifest', '--no-manifest')]:
            call('run', model, *options, error='IR_USAGE'); controls += 1
        call('run', model, '--manifest', root/'x.json', error='IR_USAGE'); controls += 1
        call('replay', receipt, '--no-manifest', error='IR_USAGE'); controls += 1
        call('lint', model, '--no-manifest', error='IR_USAGE'); controls += 1
        controls += 1

        # Existing regular files, directories, hardlinks and dangling symlinks
        # cannot be consumed as fresh completion-marker destinations.
        for kind in ('file', 'directory', 'hardlink', 'symlink'):
            out = root/f'protected-{kind}.csv'; out.write_text('KEEP')
            target = sidecar(out)
            if kind == 'file': target.write_text('KEEP RECEIPT')
            elif kind == 'directory': target.mkdir()
            elif kind == 'hardlink': target.hardlink_to(model)
            else: target.symlink_to(root/'missing-target')
            call('run', model, '--out', out, error='IR_USAGE')
            assert out.read_text() == 'KEEP'
            assert not (root/'missing-target').exists()
            if kind == 'file': assert target.read_text() == 'KEEP RECEIPT'
            if kind == 'hardlink': assert model.read_bytes() == (ROOT/'models/decay.ir.json').read_bytes()
            if kind == 'symlink': assert target.is_symlink()
            controls += 1

        # Failed executions and failed publication preserve previous outputs and
        # leave neither a receipt nor a sibling temporary file.
        broken = json.loads(model.read_text())
        broken['parameters'][0]['value'] = 1e308
        bad_model = root/'broken.json'; bad_model.write_text(json.dumps(broken))
        out = root/'failed.csv'; out.write_text('KEEP')
        before = sorted(root.iterdir())
        call('run', bad_model, '--out', out, error='IR_RUNTIME')
        assert out.read_text() == 'KEEP' and sorted(root.iterdir()) == before
        folder = root/'output-directory'; folder.mkdir()
        before = sorted(root.iterdir())
        call('run', model, '--out', folder, error='IR_IO')
        assert sorted(root.iterdir()) == before and not sidecar(folder).exists()
        call('run', model, '--out', root/'missing'/'file.csv', error='IR_IO')
        call('run', model, '--out', out, '--manifest', root/'missing'/'receipt.json', error='IR_IO')
        assert out.read_text() == 'KEEP' and sorted(root.iterdir()) == before
        controls += 4
        # Output path protections also apply with an automatically derived sidecar.
        call('run', model, '--out', model, error='IR_USAGE')
        assert not sidecar(model).exists()
        controls += 1
        if args.without_arrow:
            for format in ('parquet', 'arrow'):
                out = root/f'unavailable.{format}'
                call('run', model, '--out', out, error='IR_OUTPUT_UNAVAILABLE')
                assert not out.exists() and not sidecar(out).exists()
                controls += 1
        else:
            out = root/'custom-suffix'; call('run', model, '--out', out, '--format', 'parquet')
            assert json.loads(sidecar(out).read_text())['output']['format'] == 'parquet'
            assert json.loads(call('verify-results', sidecar(out)))['verdict'] == 'pass'
            controls += 1
        print(f'Default manifests: {runs} file runs, {calls} CLI checks, {controls} controls pass')


if __name__ == '__main__':
    main()
