"""Portable replay with absent originals, independent hashes and adversarial members."""
import argparse
import copy
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from provenance_cli_contract import records_identity, reseal

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
        from referencing import Registry, Resource
        schema = json.loads((ROOT/'ir/schema/ankurafathom-manifest.schema.json').read_text())
        registry = Registry().with_resource(schema['$id'], Resource.from_contents(schema))
        validator = Draft202012Validator(json.loads((ROOT/'ir/schema/ankurafathom-bundle.schema.json').read_text()), registry=registry)
    calls = bundles = controls = 0
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        def call(*options, error=None):
            nonlocal calls
            calls += 1
            result = subprocess.run([str(exe), *map(str, options)], cwd=root, capture_output=True, text=True, timeout=90)
            if error:
                assert result.returncode == 1 and not result.stdout, (options, result)
                diagnostic = json.loads(result.stderr)['diagnostics'][0]
                assert diagnostic['code'] == error, (options, diagnostic)
                return diagnostic
            assert result.returncode == 0 and not result.stderr, (options, result.stderr)
            return result.stdout

        fixtures = [(name, 'csv', False) for name in ('decay.ir.json', 'stochastic_process.ir.json',
            'typed_abm_rates.ir.json', 'agent_pool.ir.json', 'agent_stock_sd.ir.json', 'hybrid_completion.ir.json')]
        if not args.without_arrow:
            fixtures += [(f'data/{name}', format, False) for name in ('parameter_decay.ir.json', 'seasonal_stock.ir.json', 'workforce.ir.json')
                         for format in ('csv', 'parquet', 'arrow')]
            fixtures.append(('data/seasonal_stock.ir.json', 'csv', True))
        preserved = []
        for index, (name, data_format, multiple) in enumerate(fixtures):
            for ensemble in (False, True):
                source = root/f'sources-{index}-{ensemble}'
                source.mkdir()
                document = json.loads((ROOT/'models'/name).read_text())
                if multiple:
                    second = copy.deepcopy(document['data'][0]); second['id'] = 'aaa_series'
                    document['data'].append(second)
                if document.get('data'):
                    if data_format != 'csv':
                        import pyarrow as pa
                        import pyarrow.csv as pacsv
                        import pyarrow.parquet as pq
                    for i, binding in enumerate(document['data']):
                        original = ROOT/'models/data'/binding['source']
                        table_path = source/f'input-{i}.{data_format}'
                        if data_format == 'csv':
                            shutil.copyfile(original, table_path)
                        else:
                            types = {'string': pa.string(), 'f64': pa.float64(), 'i64': pa.int64(), 'u64': pa.uint64(), 'bool': pa.bool_()}
                            table = pacsv.read_csv(original, convert_options=pacsv.ConvertOptions(
                                column_types={f['name']: types[f['type']] for f in binding['schema']['columns']}))
                            if data_format == 'parquet': pq.write_table(table, table_path)
                            else:
                                with pa.OSFile(str(table_path), 'wb') as sink:
                                    with pa.ipc.new_file(sink, table.schema) as writer: writer.write_table(table)
                        # Test both absolute and relative source declarations.
                        binding['source'] = str(table_path) if ensemble else table_path.name
                model = source/'model.json'
                model.write_text(json.dumps(document, indent=2) + '\n')
                experiment = source/'experiment.json'
                experiment.write_text(json.dumps({'seed': 42, 'replications': 2, 'scenarios': [
                    {'id': 17, 'parameters': {}}, {'id': 65535, 'parameters': {}}]}) + '\n')
                extra = ['--experiment', experiment] if ensemble else []
                output = source/'result.csv'
                call('run', model, '--out', output, '--threads', 8, '--seed', 18446744073709551615, *extra)
                receipt = Path(str(output) + '.manifest.json')
                manifest = json.loads(receipt.read_text())
                with output.open() as file: assert records_identity(csv.DictReader(file)) == manifest['result']['sha256']
                destination = root/f'capture-{index}-{ensemble}'
                if index == 6 and not ensemble:
                    changed_table = Path(manifest['inputs']['data'][0]['path'])
                    before_bytes = changed_table.read_bytes()
                    changed_table.write_bytes(before_bytes + b'\n')
                    call('bundle', receipt, '--out', destination, error='IR_BUNDLE')
                    assert not destination.exists() and not list(root.glob('.fathom-bundle-*'))
                    changed_table.write_bytes(before_bytes)
                    controls += 1
                verdict = json.loads(call('bundle', receipt, '--out', destination))
                assert verdict['scope'] == 'input-bundle' and verdict['manifest_id'] == manifest['id']
                envelope = json.loads((destination/'bundle.json').read_text())
                assert envelope == {'bundle_version': '0.1', 'manifest': manifest}
                if validator: validator.validate(envelope)
                assert (destination/'model.json').read_bytes() == model.read_bytes()
                assert (destination/'experiment.json').exists() == ensemble
                if ensemble: assert (destination/'experiment.json').read_bytes() == experiment.read_bytes()
                for i, item in enumerate(manifest['inputs']['data']):
                    member = destination/'data'/f'{i}{Path(item["path"]).suffix}'
                    assert hashlib.sha256(member.read_bytes()).hexdigest() == item['file_sha256']
                    assert member.read_bytes() == Path(item['path']).read_bytes()
                # Whole-directory relocation, then delete inputs, source receipt and
                # original result. Any fallback to original paths must now fail.
                moved = root/f'relocated {index}-{ensemble}'
                destination.rename(moved)
                shutil.rmtree(source)
                for threads in (1, 8, 32):
                    verdict = json.loads(call('replay', moved, '--threads', threads))
                    assert verdict['manifest_id'] == manifest['id'] and verdict['result'] == manifest['result']
                    assert verdict['threads'] == threads
                replayed = root/f'replayed-{index}-{ensemble}.csv'
                call('replay', moved, '--out', replayed)
                with replayed.open() as file:
                    rows = list(csv.DictReader(file))
                    assert records_identity(rows) == manifest['result']['sha256']
                    assert {r['manifest_id'] for r in rows} == {manifest['id']}
                original_receipt = root/f'original-{index}-{ensemble}.json'
                original_receipt.write_text(json.dumps(manifest))
                assert json.loads(call('verify-results', original_receipt, '--results', replayed))['verdict'] == 'pass'
                assert not Path(str(replayed) + '.manifest.json').exists()
                if data_format != 'csv':
                    binary = root/f'replayed-{index}-{ensemble}.{data_format}'
                    call('replay', moved, '--out', binary)
                    assert json.loads(call('verify-results', binary, '--embedded'))['manifest_id'] == manifest['id']
                preserved.append(moved)
                bundles += 1

        for kind in ('grid', 'lhs', 'sobol'):
            source = root/f'design-source-{kind}'; source.mkdir()
            model = source/'model.json'; shutil.copyfile(ROOT/'models/decay.ir.json', model)
            experiment = source/'experiment.json'
            shutil.copyfile(ROOT/f'models/decay.{kind}.experiment.json', experiment)
            output = source/'result.csv'; call('run', model, '--experiment', experiment, '--out', output)
            receipt = Path(str(output) + '.manifest.json')
            manifest = json.loads(receipt.read_text())
            target = root/f'design-bundle-{kind}'; call('bundle', receipt, '--out', target)
            shutil.rmtree(source)
            for threads in (1, 8):
                verdict = json.loads(call('replay', target, '--threads', threads))
                assert verdict['result'] == manifest['result'] and verdict['manifest_id'] == manifest['id']
            bundles += 1

        baseline = preserved[1]  # decay with an experiment and simple decimal values
        sentinel = root/'sentinel.csv'
        sentinel.write_text('KEEP')
        def bad_bundle(edit, error='IR_BUNDLE'):
            nonlocal controls
            target = root/f'invalid-{controls}'
            shutil.copytree(baseline, target)
            edit(target)
            diagnostic = call('replay', target, '--out', sentinel, error=error)
            assert sentinel.read_text() == 'KEEP'
            controls += 1
            return diagnostic

        def envelope_edit(path, edit, seal=False):
            file = path/'bundle.json'
            envelope = json.loads(file.read_text())
            edit(envelope)
            if seal: reseal(envelope['manifest'])
            file.write_text(json.dumps(envelope))
        bad_bundle(lambda p: (p/'model.json').write_text((p/'model.json').read_text() + ' '))
        bad_bundle(lambda p: (p/'experiment.json').write_text((p/'experiment.json').read_text() + ' '))
        bad_bundle(lambda p: (p/'model.json').unlink())
        bad_bundle(lambda p: (p/'unexpected.json').write_text('{}'))
        bad_bundle(lambda p: (p/'unexpected').mkdir())
        bad_bundle(lambda p: envelope_edit(p, lambda e: e.update(bundle_version='99')))
        bad_bundle(lambda p: envelope_edit(p, lambda e: e.update(files=['../../outside'])))
        bad_bundle(lambda p: envelope_edit(p, lambda e: e['manifest'].update(id='0'*64)), 'IR_MANIFEST')
        diagnostic = bad_bundle(lambda p: envelope_edit(p, lambda e: e['manifest']['inputs']['execution']['build'].update(compiler='foreign'), True), 'IR_REPLAY_MISMATCH')
        assert diagnostic['pointer'] == '/inputs'
        diagnostic = bad_bundle(lambda p: envelope_edit(p, lambda e: e['manifest']['result'].update(sha256='0'*64), True), 'IR_REPLAY_MISMATCH')
        assert diagnostic['pointer'] == '/result'
        def link_member(path, name):
            member = path/name; member.unlink(); member.symlink_to(baseline/name)
        bad_bundle(lambda p: link_member(p, 'model.json'))
        bad_bundle(lambda p: link_member(p, 'bundle.json'))
        def fifo(path):
            (path/'model.json').unlink(); os.mkfifo(path/'model.json')
        bad_bundle(fifo)
        def oversized(path):
            with (path/'model.json').open('wb') as file: file.truncate(256*1024*1024+1)
        assert 'limit' in bad_bundle(oversized)['message']
        alias = root/'bundle-alias'; alias.symlink_to(baseline, target_is_directory=True)
        for spelling in (str(alias), str(alias) + '/'):
            call('replay', spelling, error='IR_BUNDLE'); controls += 1
        for path in (baseline/'model.json', baseline/'new.csv', baseline/'sub'/'new.csv'):
            call('replay', baseline, '--out', path, error='IR_USAGE'); controls += 1
        alias = root/'member-alias'; alias.hardlink_to(baseline/'model.json')
        call('replay', baseline, '--out', alias, error='IR_USAGE'); controls += 1
        assert alias.read_bytes() == (baseline/'model.json').read_bytes()

        if not args.without_arrow:
            data_baseline = preserved[12]  # first CSV parameter-table bundle
            for change in ('bytes', 'missing', 'symlink', 'directory'):
                target = root/f'data-invalid-{change}'; shutil.copytree(data_baseline, target)
                member = target/'data/0.csv'
                if change == 'bytes': member.write_bytes(member.read_bytes()+b'\n')
                elif change == 'missing': member.unlink()
                elif change == 'symlink': member.unlink(); member.symlink_to(data_baseline/'data/0.csv')
                else: shutil.rmtree(target/'data'); (target/'data').symlink_to(data_baseline/'data', target_is_directory=True)
                call('replay', target, '--out', sentinel, error='IR_BUNDLE')
                assert sentinel.read_text() == 'KEEP'; controls += 1

            for field, value, error in (
                    ('canonical_sha256', '0'*64, 'IR_REPLAY_MISMATCH'),
                    ('file_sha256', '0'*64, 'IR_BUNDLE'),
                    ('path', '/unavailable/outside.csv', 'IR_BUNDLE')):
                target = root/f'data-metadata-{field}'; shutil.copytree(data_baseline, target)
                envelope_edit(target, lambda e: e['manifest']['inputs']['data'][0].update({field: value}), True)
                diagnostic = call('replay', target, '--out', sentinel, error=error)
                assert diagnostic['pointer'] != '/manifest/id' and sentinel.read_text() == 'KEEP'
                controls += 1

        # Packing is input capture, not a claim of current-build or numeric replay.
        source = root/'packing'; source.mkdir()
        model = source/'model.json'; shutil.copyfile(ROOT/'models/decay.ir.json', model)
        output = source/'result.csv'; call('run', model, '--out', output)
        receipt = Path(str(output)+'.manifest.json')
        original_manifest = json.loads(receipt.read_text())
        for kind in ('build', 'result', 'legacy'):
            manifest = copy.deepcopy(original_manifest)
            if kind == 'build': manifest['inputs']['execution']['build']['compiler'] = 'foreign'
            elif kind == 'result': manifest['result']['sha256'] = '0'*64
            else: manifest['manifest_version'] = '0.1'; del manifest['output']['schema_version']
            changed = source/f'{kind}.json'; changed.write_text(json.dumps(reseal(manifest)))
            target = root/f'packed-{kind}'; call('bundle', changed, '--out', target)
            if kind == 'legacy': assert json.loads(call('replay', target))['verdict'] == 'pass'
            else: call('replay', target, '--out', sentinel, error='IR_REPLAY_MISMATCH')
            assert sentinel.read_text() == 'KEEP'; controls += 1
        memory = copy.deepcopy(original_manifest); memory['manifest_version'] = '0.3'
        memory['inputs']['model']['path'] = ''
        memory['output'].update(path='', format='memory')
        memory_path = source/'memory.json'; memory_path.write_text(json.dumps(reseal(memory)))
        call('bundle', memory_path, '--out', root/'memory-bundle', error='IR_REPLAY_UNSUPPORTED'); controls += 1
        model.write_text(model.read_text()+' ')
        call('bundle', receipt, '--out', root/'changed-source', error='IR_BUNDLE')
        assert not (root/'changed-source').exists() and not list(root.glob('.fathom-bundle-*'))
        controls += 1
        model.write_bytes((ROOT/'models/decay.ir.json').read_bytes())
        for kind in ('directory', 'file', 'dangling'):
            target = root/f'existing-{kind}'
            if kind == 'directory': target.mkdir()
            elif kind == 'file': target.write_text('KEEP')
            else: target.symlink_to(root/'absent')
            call('bundle', receipt, '--out', target, error='IR_BUNDLE'); controls += 1
        call('bundle', receipt, '--out', root/'missing-parent'/'bundle', error='IR_IO'); controls += 1
        for options in ((), ('--out',), ('--out', ''), ('--threads', 8), ('--out', root/'x', '--out', root/'y')):
            call('bundle', receipt, *options, error='IR_USAGE'); controls += 1
        assert not list(root.glob('.fathom-bundle-*'))
        print(f'Portable bundles: {bundles} relocated runs; {calls} CLI checks; {controls} corruption/publication controls pass')


if __name__ == '__main__':
    main()
