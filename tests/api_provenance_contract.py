"""C/Python run receipts, independent hashes, CLI parity and saved-artifact verification."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from provenance_hash_contract import numeric_hash
from provenance_cli_contract import canonical, reseal

ROOT = Path(__file__).resolve().parents[1]
def call(command, ok=True, raw=False):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    assert (result.returncode == 0) == ok, (command, result.stdout, result.stderr)
    text = result.stdout if ok else result.stderr
    return text if raw else json.loads(text)

def hash_payload(serialized):
    # Preserve the producer's decimal spellings, while independently locating
    # top-level JSON fields. Python repr and nlohmann dump can roundtrip the same
    # binary64 with different decimal strings (notably LHS coordinates).
    text = serialized.strip()
    decoder = json.JSONDecoder()
    fields = []
    position = 1
    while position < len(text) - 1:
        start = position
        key, position = decoder.raw_decode(text, position)
        assert text[position] == ':'
        _, end = decoder.raw_decode(text, position + 1)
        if key != 'id':
            fields.append(text[start:end])
        position = end + 1
    return ('{' + ','.join(fields) + '}').encode()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('package', type=Path)
    parser.add_argument('probe', type=Path)
    parser.add_argument('cli', type=Path)
    parser.add_argument('--without-arrow', action='store_true')
    parser.add_argument('--schema', action='store_true')
    args = parser.parse_args()
    sys.path.insert(0, str(args.package.resolve()))
    import ankurafathom as af
    validator = None
    if args.schema:
        from jsonschema import Draft202012Validator
        validator = Draft202012Validator(json.loads((ROOT/'ir/schema/ankurafathom-manifest.schema.json').read_text()))
    receipts = artifacts = controls = 0
    def validate(manifest, source, experiment, mode, threads, seed, serialized):
        if validator:
            validator.validate(manifest)
        assert manifest['manifest_version'] == '0.3'
        assert manifest['output'] == {'path': '', 'format': 'memory', 'schema_version': '0.2'}
        assert manifest['id'] == hashlib.sha256(hash_payload(serialized)).hexdigest()
        assert manifest['validation'] == {'model': 'pass', 'execution': 'pass', 'accuracy': 'not_assessed'}
        for name, raw in (('model', source.read_bytes()), ('experiment', None if experiment is None else experiment.read_bytes())):
            item = manifest['inputs'][name]
            if raw is None:
                assert item is None
                continue
            assert item['file_sha256'] == hashlib.sha256(raw).hexdigest()
            assert item['canonical_sha256'] == hashlib.sha256(item['canonical_json'].encode()).hexdigest()
            assert json.loads(item['canonical_json']) == json.loads(raw)
            assert item['path'] == (str(source.resolve()) if name == 'model' and mode == 'file' else '')
        assert manifest['inputs']['execution']['threads'] == threads
        assert manifest['inputs']['execution']['seed'] == seed
        for data in manifest['inputs']['data']:
            assert data['file_sha256'] == hashlib.sha256(Path(data['path']).read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        model_path = ROOT/'models/decay.ir.json'
        files = ['decay.ir.json', 'stochastic_process.ir.json', 'agent_pool.ir.json', 'hybrid_completion.ir.json']
        if not args.without_arrow:
            files += ['data/parameter_decay.ir.json', 'data/seasonal_stock.ir.json', 'data/workforce.ir.json']
        experiment = tmp/'experiment.json'
        experiment.write_text(json.dumps({'seed': 42, 'replications': 2,
            'scenarios': [{'id': 17, 'parameters': {}}, {'id': 65535, 'parameters': {}}]}))
        cases = [(ROOT/'models'/name, exp, threads, seed) for name in files for exp, threads, seed in (
            (None, 1, None), (experiment, 1, 0), (experiment, 8, (1 << 64) - 1))]
        cases += [(model_path, ROOT/f'models/decay.{kind}.experiment.json', 8, None) for kind in ('grid', 'lhs', 'sobol')]
        for source, exp, threads, override in cases:
            effective_seed = override if override is not None else (json.loads(exp.read_text())['seed'] if exp else 0)
            # Independent CLI receipt: same inputs/execution except known memory paths.
            cli_manifest = tmp/'cli.json'
            if cli_manifest.exists():
                cli_manifest.unlink()
            cli_command = [args.cli, 'run', source, '--out', tmp/'cli.csv', '--manifest', cli_manifest, '--threads', threads]
            if exp:
                cli_command += ['--experiment', exp]
            if override is not None:
                cli_command += ['--seed', override]
            completed = subprocess.run(list(map(str, cli_command)), capture_output=True, text=True)
            assert completed.returncode == 0, completed.stderr
            reference = json.loads(cli_manifest.read_text())
            assert reference['manifest_version'] == '0.2'
            for mode in ('file', 'json'):
                serialized = call([args.probe, source, exp or '-', threads, mode, source.parent,
                                   '-' if override is None else override, '--manifest'], raw=True)
                manifest = json.loads(serialized)
                validate(manifest, source, exp, mode, threads, effective_seed, serialized)
                if source == model_path and exp is None and mode == 'json':
                    control_manifest = copy.deepcopy(manifest)
                expected = copy.deepcopy(reference['inputs'])
                if mode == 'json':
                    expected['model']['path'] = ''
                if exp:
                    expected['experiment']['path'] = ''
                assert manifest['inputs'] == expected
                assert manifest['result'] == reference['result']
                receipts += 1
            if args.without_arrow:
                continue
            import pyarrow as pa
            import pyarrow.parquet as pq
            model = af.Model.from_json(source.read_bytes(), base_directory=source.parent)
            table = af.run(model, af.Experiment(exp.read_bytes()) if exp else None,
                           threads=threads, seed=override, provenance=True)
            del model
            serialized = table.schema.metadata[b'ankurafathom.manifest'].decode()
            manifest = json.loads(serialized)
            validate(manifest, source, exp, 'json', threads, effective_seed, serialized)
            assert manifest['inputs'] == expected and manifest['result'] == reference['result']
            assert table.schema.metadata[b'ankurafathom.schema_version'] == b'0.2'
            assert table.schema.metadata[b'ankurafathom.manifest_id'].decode() == manifest['id']
            assert table.column_names[-2:] == ['manifest_id', 'scenario_parameters']
            grouped = []
            parameters = {s['id']: s['effective_parameters'] for s in manifest['inputs']['scenarios']}
            for row in table.to_pylist():
                assert row['manifest_id'] == manifest['id']
                assert json.loads(row['scenario_parameters']) == parameters[row['scenario']]
                if not grouped or (row['scenario'], row['replication']) != (grouped[-1]['scenario'], grouped[-1]['replication']):
                    grouped.append({'scenario': row['scenario'], 'replication': row['replication'], 'rows': []})
                grouped[-1]['rows'].append({'id': row['output_id'], 'time_bits': struct.unpack('<Q', struct.pack('<d', row['time']))[0],
                                           'value_bits': struct.unpack('<Q', struct.pack('<d', row['value']))[0]})
            assert numeric_hash(grouped) == manifest['result']['sha256']
            receipts += 1
            sidecar = tmp/'memory.json'; sidecar.write_bytes(canonical(manifest))
            for kind in ('arrow', 'parquet'):
                output = tmp/f'rows.{kind}'
                if kind == 'arrow':
                    with pa.OSFile(str(output), 'wb') as sink, pa.ipc.new_file(sink, table.schema) as writer:
                        writer.write_table(table)
                else:
                    pq.write_table(table, output)
                for request in ([output, '--embedded'], [sidecar, '--results', output]):
                    verified = call([args.cli, 'verify-results', *request])
                    assert verified['manifest_id'] == manifest['id'] and verified['scope'] == 'artifact-integrity'
                    artifacts += 1
        # Use a simple decay receipt whose floats have identical canonical spellings
        # in both serializers, so resealed controls exercise envelope checks.
        manifest = control_manifest
        sidecar = tmp/'memory.json'; sidecar.write_bytes(canonical(manifest))
        diagnostic = call([args.cli, 'verify-results', sidecar], ok=False)
        assert diagnostic['diagnostics'][0]['code'] == 'IR_USAGE'; controls += 1
        diagnostic = call([args.cli, 'replay', sidecar, '--out', tmp/'forbidden.csv'], ok=False)
        assert diagnostic['diagnostics'][0]['code'] == 'IR_REPLAY_UNSUPPORTED'
        assert not (tmp/'forbidden.csv').exists(); controls += 1
        mutations = [lambda m: m['output'].update(path='/invented/file'), lambda m: m['output'].update(format='arrow'),
                     lambda m: m['inputs']['model'].update(path='relative'), lambda m: m['inputs']['model'].update(canonical_sha256='0'*64),
                     lambda m: m['inputs']['model'].update(file_sha256='bad'), lambda m: m.update(manifest_version='0.2')]
        for mutate in mutations:
            altered = copy.deepcopy(manifest); mutate(altered); reseal(altered)
            sidecar.write_bytes(canonical(altered))
            diagnostic = call([args.cli, 'replay', sidecar], ok=False)
            assert diagnostic['diagnostics'][0]['code'] == 'IR_MANIFEST'
            assert diagnostic['diagnostics'][0]['pointer'] != '/manifest/id'
            controls += 1
        if args.without_arrow:
            try:
                af.run(af.Model.from_json(model_path.read_bytes()), provenance=True)
            except af.FathomError as error:
                assert error.status == 7 and error.code == 'FATHOM_UNAVAILABLE'
                controls += 1
            else:
                raise AssertionError('CSV-only provenance table export succeeded')
        if not args.without_arrow:
            # Snapshot receipts do not reopen inputs after model load.
            source = tmp/'snapshot.json'; source.write_bytes(model_path.read_bytes())
            model = af.Model.from_json(source.read_bytes()); source.unlink()
            snapshot = af.run(model, provenance=True)
            assert json.loads(snapshot.schema.metadata[b'ankurafathom.manifest'])['inputs']['model']['path'] == ''
            controls += 1
            # Data identities also come from snapshots, even after the table is removed.
            data_path = tmp/'synthetic_decay_rates.csv'
            raw_data = (ROOT/'models/data/synthetic_decay_rates.csv').read_bytes()
            data_path.write_bytes(raw_data)
            bound = af.Model.from_json((ROOT/'models/data/parameter_decay.ir.json').read_bytes(), base_directory=tmp)
            data_path.unlink()
            captured = af.run(bound, provenance=True)
            receipt = json.loads(captured.schema.metadata[b'ankurafathom.manifest'])
            assert receipt['inputs']['data'][0]['file_sha256'] == hashlib.sha256(raw_data).hexdigest()
            assert receipt['inputs']['data'][0]['path'] == str(data_path)
            output = tmp/'snapshot.parquet'; pq.write_table(captured, output)
            assert call([args.cli, 'verify-results', output, '--embedded'])['verdict'] == 'pass'
            controls += 1
            for value in (1, None, 'yes'):
                try:
                    af.run(model, provenance=value)
                except TypeError:
                    controls += 1
                else:
                    raise AssertionError('non-bool provenance accepted')
            altered = table.set_column(4, table.schema.field('value'), pa.array([0.0] * table.num_rows))
            output = tmp/'tampered.parquet'; pq.write_table(altered, output)
            diagnostic = call([args.cli, 'verify-results', output, '--embedded'], ok=False)
            assert diagnostic['diagnostics'][0]['code'] == 'IR_RESULT_VERIFY'; controls += 1
    print(f'API provenance: {receipts} receipts, {artifacts} saved-artifact checks, {controls} controls pass')

if __name__ == '__main__':
    main()
