"""Independent canonical encoding/hash oracle and Arrow-backed data reader tests."""
import argparse
import copy
import csv
import hashlib
import io
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile

TYPE_CODES = {'bool': 1, 'i32': 2, 'i64': 3, 'u64': 4, 'f64': 5, 'string': 6}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def canonical(schema, rows):
    """Byte format specified in RUNTIME_DATA.md; no Arrow serialization involved."""
    columns = sorted(schema['columns'], key=lambda c: c['name'])
    key = schema['key']
    ordered = sorted(rows, key=lambda r: r[key].encode() if isinstance(r[key], str) else r[key])
    data = bytearray(b'AnkuraFathom.table.v1\0')

    def u64(value):
        data.extend(struct.pack('<Q', value))

    def text(value):
        encoded = value.encode('utf-8');u64(len(encoded));data.extend(encoded)

    u64(len(columns));u64(len(ordered));text(key)
    for column in columns:
        text(column['name']);data.append(TYPE_CODES[column['type']]);text(column.get('unit', ''))
        domain = sorted(column.get('categories', []), key=lambda s: s.encode())
        u64(len(domain))
        for item in domain:
            text(item)
    formats = {'bool': '<?', 'i32': '<i', 'i64': '<q', 'u64': '<Q', 'f64': '<d'}
    expected = []
    for row in ordered:
        values = []
        for column in columns:
            value = row[column['name']];kind = column['type']
            if kind == 'string':
                text(value)
            else:
                data.extend(struct.pack(formats[kind], value))
            values.append({'f64_bits': struct.unpack('<Q', struct.pack('<d', value))[0]} if kind == 'f64' else value)
        expected.append(values)
    return hashlib.sha256(data).hexdigest(), expected


def write_fixture(path, schema, rows, dictionary=False, metadata=False, chunks=3):
    import pyarrow as pa
    import pyarrow.parquet as pq
    columns = schema['columns']
    if path.suffix == '.csv':
        stream = io.StringIO(newline='');writer = csv.writer(stream, lineterminator='\r\n' if metadata else '\n')
        writer.writerow(c['name'] for c in columns)
        for row in rows:
            writer.writerow(str(row[c['name']]).lower() if c['type'] == 'bool' else row[c['name']] for c in columns)
        path.write_bytes(stream.getvalue().encode())
        return
    types = {'bool': pa.bool_(), 'i32': pa.int32(), 'i64': pa.int64(), 'u64': pa.uint64(), 'f64': pa.float64(), 'string': pa.string()}
    arrays = []
    for column in columns:
        values = [row[column['name']] for row in rows]
        array = pa.array(values, type=types[column['type']])
        if dictionary and column['type'] == 'string' and path.suffix != '.parquet':
            array = array.dictionary_encode()
        arrays.append(array)
    table = pa.Table.from_arrays(arrays, names=[c['name'] for c in columns])
    if metadata:
        table = table.replace_schema_metadata({b'irrelevant': b'writer metadata'})
    if path.suffix == '.parquet':
        pq.write_table(table, path, compression='gzip' if metadata else 'NONE', use_dictionary=dictionary, row_group_size=chunks)
    else:
        with pa.OSFile(str(path), 'wb') as target:
            with pa.ipc.new_file(target, table.schema) as writer:
                writer.write_table(table, max_chunksize=chunks)


def main():
    parser = argparse.ArgumentParser();parser.add_argument('probe', type=Path)
    parser.add_argument('--without-arrow', action='store_true');parser.add_argument('--report', type=Path)
    args = parser.parse_args();probe = str(args.probe.resolve())
    valid_cases = invalid_cases = cells = 0
    hashes = []
    with tempfile.TemporaryDirectory(prefix='fathom-data-') as directory:
        root = Path(directory);request = root / 'request.json'

        def invoke(path, schema):
            request.write_text(json.dumps(schema))
            return subprocess.run([probe, str(path), str(request)], capture_output=True)

        def good(path, schema, rows):
            nonlocal valid_cases, cells
            source = path.read_bytes()
            key_type = next(c['type'] for c in schema['columns'] if c['name'] == schema['key'])
            candidates = {'bool': [False, True], 'i32': [42], 'i64': [42], 'u64': [42, 2**64-1],
                          'f64': [42.25, 0.0], 'string': ['__absent_key__', '']}
            lookups = candidates[key_type]
            result = invoke(path, dict(schema, lookups=lookups))
            require(result.returncode == 0 and not result.stderr, result.stderr.decode())
            actual = json.loads(result.stdout);digest, expected = canonical(schema, rows)
            require(actual['file_hash'] == hashlib.sha256(source).hexdigest(), 'file snapshot hash mismatch')
            require(actual['canonical_hash'] == digest, 'canonical byte encoding/hash differs from Python oracle')
            require(actual['rows'] == expected, 'typed values or stable key order changed')
            require(actual['columns'] == sorted(c['name'] for c in schema['columns']), 'column order not canonical')
            keys = sorted([r[schema['key']] for r in rows], key=lambda k: k.encode() if isinstance(k, str) else k)
            require(actual['lookups'] == [keys.index(k) if k in keys else None for k in lookups], 'missing/existing typed key lookup mismatch')
            valid_cases += 1;cells += sum(len(row) for row in expected);hashes.append(digest)
            return actual

        def bad(path, schema, code, pointer=None):
            nonlocal invalid_cases
            result = invoke(path, schema)
            require(result.returncode == 1 and not result.stdout, 'invalid table published a result: ' + result.stdout.decode())
            error = json.loads(result.stderr)
            require(error['code'] == code, f'expected {code}: {error}')
            if pointer is not None:
                require(error['pointer'] == pointer, f'diagnostic pointer: {error}')
            invalid_cases += 1

        base = dict(key='id', columns=[dict(name='id', type='u64')])
        require(json.loads(subprocess.check_output([probe]))['available'] != args.without_arrow, 'data feature flag mismatch')
        for schema, code in [({'key': 'id', 'columns': []}, 'DATA_SCHEMA'),
                             ({'key': 'absent', 'columns': base['columns']}, 'DATA_KEY'),
                             ({'key': 'id', 'columns': base['columns'] * 2}, 'DATA_SCHEMA'),
                             ({'key': 'bad/name', 'columns': [dict(name='bad/name', type='u64')]}, 'DATA_SCHEMA'),
                             ({'key': 'id', 'columns': [dict(name='id', type='u64', categories=['x'])]}, 'DATA_SCHEMA'),
                             ({'key': 'id', 'columns': [dict(name='id', type='string', categories=['x', 'x'])]}, 'DATA_SCHEMA')]:
            bad(root / 'missing.csv', schema, code)
        bad(root / 'missing.csv', dict(base, max_file_bytes=0), 'DATA_LIMIT', '/options')
        bad(root / 'missing.csv', dict(base, max_rows=0), 'DATA_LIMIT', '/options')
        bad(root / 'unknown.ext', base, 'DATA_FORMAT', '/source')
        if args.without_arrow:
            for suffix in ('csv', 'parquet', 'arrow'):
                bad(root / f'missing.{suffix}', base, 'DATA_UNAVAILABLE', '/source')
        else:
            import pyarrow as pa
            import pyarrow.parquet as pq
            schema = dict(key='id', columns=[dict(name='id', type='u64'), dict(name='active', type='bool'),
                dict(name='level', type='i32'), dict(name='signed', type='i64'), dict(name='rate', type='f64', unit='USD/hour'),
                dict(name='dept', type='string', categories=['risk', 'équipe', 'consulting']), dict(name='note', type='string')])
            bit_patterns = [0, 0x8000000000000000, 1, 0x8000000000000001, 0x0010000000000000,
                            0x7fefffffffffffff, 0xffefffffffffffff, 0x3fb999999999999a, 0x3ff0000000000001]
            ids = [0, 2**64-1, 2**63, 3, 11, 17, 2**53+1, 1, 99]
            notes = ['', 'null', 'NA', 'nan', 'comma,quote"', 'line\nbreak', 'nul\0byte', 'Ω', '𐀀']
            rows = [dict(id=ids[i], active=i%2 == 0, level=(-2**31 if i == 0 else 2**31-1 if i == 1 else i),
                         signed=(-2**63 if i == 0 else 2**63-1 if i == 1 else -i),
                         rate=struct.unpack('<d', struct.pack('<Q', b))[0], dept=['risk', 'équipe', 'consulting'][i%3], note=notes[i])
                    for i, b in enumerate(bit_patterns)]
            rng = random.Random(6711);physical_hashes = set();logical_hashes = set()
            for variant in range(8):
                shuffled = copy.deepcopy(rows);rng.shuffle(shuffled)
                altered = copy.deepcopy(schema);rng.shuffle(altered['columns'])
                for column in altered['columns']:
                    if 'categories' in column:
                        rng.shuffle(column['categories'])
                for suffix in ('csv', 'parquet', 'arrow'):
                    path = root / f'encoding-{variant}.{suffix}'
                    write_fixture(path, altered, shuffled, dictionary=variant%2 == 0, metadata=variant%3 == 0, chunks=1+variant)
                    actual = good(path, altered, rows)
                    physical_hashes.add(actual['file_hash']);logical_hashes.add(actual['canonical_hash'])
            require(len(physical_hashes) >= 20 and len(logical_hashes) == 1, 're-encoding did not preserve exactly one logical table identity')
            # Units, categories and values are part of logical identity; file
            # metadata, chunking and physical row/column order are not.
            original_hash = next(iter(logical_hashes));path = root / 'base.parquet';write_fixture(path, schema, rows)
            for change in ('unit', 'domain', 'value', 'signed_zero'):
                changed = copy.deepcopy(schema);values = copy.deepcopy(rows)
                if change == 'unit':
                    changed['columns'][4]['unit'] = 'USD/day'
                elif change == 'domain':
                    changed['columns'][5]['categories'].append('legal')
                elif change == 'value':
                    values[2]['level'] += 1
                else:
                    values[1]['rate'] = 0.0
                varied = root / f'{change}.parquet';write_fixture(varied, changed, values)
                require(good(varied, changed, values)['canonical_hash'] != original_hash, 'semantic change did not affect hash')
            # All supported key types, including floating keys and UTF-8 byte
            # ordering, are independent of file order and reader chunking.
            keys = {'bool': [True, False], 'i32': [2**31-1, -2**31, 0], 'i64': [2**63-1, -2**63, 0],
                    'u64': [2**64-1, 0, 2**63], 'f64': [1.25, -3.5, 0.0, 5e-324],
                    'string': ['Ω', 'é', '', 'a', 'A', '\0', '𐀀']}
            for kind, values in keys.items():
                key_schema = dict(key='id', columns=[dict(name='id', type=kind)])
                for suffix in ('csv', 'parquet', 'arrow'):
                    source = root / f'keys-{kind}.{suffix}';values_rows = [dict(id=v) for v in values]
                    write_fixture(source, key_schema, values_rows, dictionary=True);good(source, key_schema, values_rows)
            for suffix in ('csv', 'parquet', 'arrow'):
                empty = root / f'empty.{suffix}';write_fixture(empty, schema, []);good(empty, schema, [])
            # File snapshot owns its values after the path disappears.
            detached = root / 'detached.arrow';write_fixture(detached, schema, rows, dictionary=True)
            good(detached, dict(schema, remove_after_load=True), rows);require(not detached.exists(), 'snapshot detach test did not remove path')
            odd = root / 'data.anything';odd.write_bytes(path.read_bytes());good(odd, dict(schema, format='parquet'), rows)
            bad(root / 'missing.parquet', schema, 'DATA_IO', '/source')
            bad(root, dict(schema, format='csv'), 'DATA_IO', '/source')
            bad(Path('https://invalid.test/table.parquet'), schema, 'DATA_IO', '/source')
            bad(path, dict(schema, max_file_bytes=len(path.read_bytes())-1), 'DATA_LIMIT', '/source')
            bad(path, dict(schema, max_rows=len(rows)-1), 'DATA_LIMIT', '/source')
            good(path, dict(schema, max_file_bytes=len(path.read_bytes()), max_rows=len(rows)), rows)
            changed = copy.deepcopy(schema);changed['columns'][2]['type'] = 'i64'
            bad(path, changed, 'DATA_SCHEMA', '/schema/level')
            changed = copy.deepcopy(schema);changed['columns'][2]['name'] = 'unknown'
            bad(path, changed, 'DATA_SCHEMA', '/schema/unknown')
            bad(path, dict(schema, columns=schema['columns'][:-1]), 'DATA_SCHEMA', '/schema')
            changed = copy.deepcopy(schema);changed['columns'][5]['categories'] = ['risk']
            bad(path, changed, 'DATA_VALUE', '/rows/1/dept')
            for suffix in ('csv', 'parquet', 'arrow'):
                duplicate = root / f'duplicate.{suffix}';write_fixture(duplicate, schema, rows+[rows[0]])
                bad(duplicate, schema, 'DATA_KEY', '/key_column')
                nonfinite = copy.deepcopy(rows);nonfinite[0]['rate'] = float('inf')
                invalid = root / f'infinite.{suffix}';write_fixture(invalid, schema, nonfinite)
                bad(invalid, schema, 'DATA_VALUE', '/rows/0/rate')
                zero_schema = dict(key='id', columns=[dict(name='id', type='f64')])
                zero = root / f'zeros.{suffix}';write_fixture(zero, zero_schema, [{'id': -0.0}, {'id': 0.0}])
                bad(zero, zero_schema, 'DATA_KEY', '/key_column')
            # Nulls, duplicate physical names and mismatched binary types must
            # not be disguised by casts or ignored columns.
            null = root / 'null.parquet';pq.write_table(pa.table({'id': pa.array([None], type=pa.uint64())}), null)
            bad(null, base, 'DATA_NULL', '/rows/0/id')
            null = root / 'null-dictionary.arrow'
            table = pa.table({'id': pa.array(['x', None]).dictionary_encode()})
            with pa.OSFile(str(null), 'wb') as sink:
                with pa.ipc.new_file(sink, table.schema) as writer:
                    writer.write_table(table)
            bad(null, dict(key='id', columns=[dict(name='id', type='string')]), 'DATA_NULL', '/rows/1/id')
            duplicate_names = root / 'duplicate-names.arrow'
            table = pa.Table.from_arrays([pa.array([1], pa.uint64()), pa.array([2], pa.uint64())], names=['id', 'id'])
            with pa.OSFile(str(duplicate_names), 'wb') as sink:
                with pa.ipc.new_file(sink, table.schema) as writer:
                    writer.write_table(table)
            bad(duplicate_names, dict(key='id', columns=[dict(name='id', type='u64'), dict(name='other', type='u64')]), 'DATA_SCHEMA', '/schema')
            for text, code in [('id\n18446744073709551616\n', 'DATA_READ'), ('id\n-1\n', 'DATA_READ'),
                               ('id\n\n', 'DATA_NULL'), ('id\n1,2\n', 'DATA_READ')]:
                invalid = root / 'invalid.csv';invalid.write_text(text);bad(invalid, base, code)
            invalid = root / 'invalid.csv';invalid.write_bytes(b'id\n\xff\n')
            bad(invalid, dict(key='id', columns=[dict(name='id', type='string')]), 'DATA_READ')
            for suffix in ('parquet', 'arrow'):
                corrupt = root / f'corrupt.{suffix}';corrupt.write_bytes(b'not a valid file')
                bad(corrupt, schema, 'DATA_READ', '/source')
            # Reproducible larger table: sorting and hashing across many chunks.
            large_schema = dict(key='id', columns=[dict(name='id', type='u64'), dict(name='value', type='f64')])
            large_rows = [dict(id=i*17, value=(i%37-18)*.125) for i in range(10003)]
            rng.shuffle(large_rows)
            for suffix in ('csv', 'parquet', 'arrow'):
                large = root / f'large.{suffix}';write_fixture(large, large_schema, large_rows, chunks=127)
                good(large, large_schema, large_rows)
    report = dict(passed=True, arrow=not args.without_arrow, valid_tables=valid_cases,
                  invalid_cases=invalid_cases, typed_cell_comparisons=cells,
                  canonical_oracle='independent Python struct encoding + hashlib.sha256',
                  evidence_sha256=hashlib.sha256(json.dumps(hashes, separators=(',', ':')).encode()).hexdigest())
    if args.report:
        args.report.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
