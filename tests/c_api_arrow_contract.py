"""Read a C-stream-to-IPC roundtrip with PyArrow, compare every bit to C getters."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import pyarrow as pa

ROOT = Path(__file__).resolve().parents[1]
def bits(value):
    return struct.unpack('<Q', struct.pack('<d', value))[0]
def call(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    assert result.returncode == 0, (command, result.stderr)
    return result.stdout

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('stream_probe', type=Path)
    parser.add_argument('row_probe', type=Path)
    args = parser.parse_args()
    stream_probe, row_probe = args.stream_probe.resolve(), args.row_probe.resolve()
    schema = pa.schema([
        pa.field('scenario', pa.uint32(), nullable=False),
        pa.field('replication', pa.uint32(), nullable=False),
        pa.field('time', pa.float64(), nullable=False),
        pa.field('output_id', pa.string(), nullable=False),
        pa.field('value', pa.float64(), nullable=False)], metadata={
            b'ankurafathom.schema_version': b'0.1', b'ankurafathom.table': b'observations'})
    comparisons = observations = 0
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        fixtures = [ROOT/'models'/name for name in (
            'decay.ir.json', 'stochastic_process.ir.json', 'typed_abm_rates.ir.json',
            'agent_stock_sd.ir.json', 'agent_pool.ir.json', 'hybrid_completion.ir.json',
            'data/parameter_decay.ir.json', 'data/seasonal_stock.ir.json', 'data/workforce.ir.json')]
        edge = json.loads(fixtures[0].read_text())
        values = [-0.0, 5e-324, -5e-324, 1.7976931348623157e308]
        edge['time'] = {'unit': 'day', 'dt': 1, 'horizon': 17000}
        edge['components'] = [{'id': f's{i}', 'kind': 'stock', 'init': value, 'non_negative': False, 'unit': 'kg'} for i, value in enumerate(values)]
        edge['outputs'] = [{'id': f'out{i}', 'stock': f's{i}'} for i in range(len(values))]
        edge_path = tmp/'edges.json'; edge_path.write_text(json.dumps(edge)); fixtures.append(edge_path)
        experiment = tmp/'experiment.json'
        experiment.write_text(json.dumps({'seed': 42, 'replications': 2,
            'scenarios': [{'id': 17, 'parameters': {}}, {'id': 65535, 'parameters': {}}]}))
        cases = [(model, exp, threads) for model in fixtures for exp, threads in (
            ('-', 1), (experiment, 1), (experiment, 8), (experiment, 32))]
        cases += [(fixtures[0], ROOT/f'models/decay.{kind}.experiment.json', 8)
                  for kind in ('grid', 'lhs', 'sobol')]
        for model, exp, threads in cases:
            output = tmp/'rows.arrow'
            call([stream_probe, model, exp, threads, output])
            with pa.memory_map(str(output), 'r') as source:
                reader = pa.ipc.open_file(source)
                if model == edge_path:
                    assert reader.num_record_batches > 1
                table = reader.read_all()
            table.validate(full=True)
            assert table.schema.equals(schema, check_metadata=True), table.schema
            expected = []
            header, *lines = call([row_probe, model, exp, threads, 'file', model.parent, '-']).splitlines()
            for line in lines:
                scenario, replication, time, value, name = line.split()
                expected.append((int(scenario), int(replication), int(time, 16),
                                 bytes.fromhex(name).decode(), int(value, 16)))
            actual = [(r['scenario'], r['replication'], bits(r['time']), r['output_id'], bits(r['value']))
                      for r in table.to_pylist()]
            assert actual == expected, (model, exp, threads)
            assert len(actual) == int(header.split()[1])
            if model == edge_path:
                assert [row[4] for row in actual[:4]] == [bits(v) for v in values]
            comparisons += 1; observations += len(actual)
    print(f'{comparisons} Arrow C Stream SDK/PyArrow roundtrips / {observations} exact observations pass')

if __name__ == '__main__':
    main()
