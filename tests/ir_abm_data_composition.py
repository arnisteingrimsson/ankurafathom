"""Composed workforce/parameter tables: analytic values, ordering and portable replay."""
import argparse
import copy
import csv
import hashlib
import io
import itertools
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from ir_population_data_contract import bind

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
        schema = json.loads((ROOT/'ir/schema/ankurafathom-ir.schema.json').read_text())
        Draft202012Validator.check_schema(schema)
        validator = Draft202012Validator(schema)
    calls = observations = configurations = controls = 0
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        inputs = root/'inputs'; inputs.mkdir()
        path = inputs/'model.json'
        def call(*arguments, code=None, pointer=None):
            nonlocal calls
            calls += 1
            result = subprocess.run([str(exe), *map(str, arguments)], cwd=root,
                                    text=True, capture_output=True)
            if code:
                assert result.returncode == 1, (arguments, result.stdout, result.stderr)
                error = json.loads(result.stderr)['diagnostics'][0]
                assert error['code'] == code, error
                if pointer is not None: assert error['pointer'] == pointer, error
            else: assert result.returncode == 0, (arguments, result.stderr)
            return result.stdout
        def save(document, valid=True):
            if validator and valid: validator.validate(document)
            path.write_text(json.dumps(document))

        inline = json.loads((ROOT/'models/typed_abm_sync.ir.json').read_text())
        inline['parameters'].append(dict(id='multiplier', value=3, unit='1'))
        inline['components'][0]['phases'][1]['assign'][0]['expr'] = 'work * multiplier'
        base, agents = bind(inline)
        def parameter(name, value, source):
            (inputs/source).write_text(f'practice,value\nforensics,99\ndisputes,{value}\n')
            return dict(id=name+'_table', source=source,
                schema=dict(key_column='practice', columns=[
                    dict(name='practice',type='string',unit=''),dict(name='value',type='f64',unit='1')]),
                use=dict(kind='parameter_table',key='disputes',parameters=[dict(parameter=name,column='value')]))
        base['data'] += [parameter('gain', 2, 'gain.csv'), parameter('multiplier', 3, 'multiplier.csv')]
        for p in base['parameters']: p['value'] = 99
        (inputs/'agents.csv').write_text('source_key,work,ready,role\n20,3,false,partner\n10,1,true,analyst\n')
        if args.without_arrow:
            save(base)
            call('lint',path,code='DATA_UNAVAILABLE',pointer='/data/1/source')
            save(inline); call('run',path)
            print('Composed ABM bindings fail closed without Arrow; inline model still runs')
            return
        import pyarrow as pa
        import pyarrow.ipc as ipc
        import pyarrow.parquet as pq
        experiment = root/'experiment.json'
        experiment.write_text(json.dumps(dict(seed=37, replications=2, scenarios=[
            dict(id=0, parameters={}),dict(id=17,parameters=dict(gain=4,multiplier=2))])))
        tables = {
            'agents': pa.table(dict(source_key=pa.array([20,10],type=pa.uint64()),
                work=pa.array([3,1],type=pa.int64()),ready=[False,True],role=['partner','analyst'])),
            'gain': pa.table(dict(practice=['forensics','disputes'],value=[99.,2.])),
            'multiplier': pa.table(dict(practice=['disputes','forensics'],value=[3.,99.]))}
        for name, table in tables.items():
            pq.write_table(table,inputs/f'{name}.parquet')
            with ipc.new_file(inputs/f'{name}.arrow',table.schema) as writer: writer.write_table(table)
        save(inline)
        reference = call('run',path,'--experiment',experiment)
        for row in csv.DictReader(io.StringIO(reference)):
            n = round(float(row['time'])/inline['time']['dt'])
            g,m = (2,3) if row['scenario']=='0' else (4,2)
            first = m**n + g*m*(m**n-1)//(m-1)
            total = 4*m**n + 2*g*m*(m**n-1)//(m-1)
            expected = dict(first=first,total=total,ready=int(n%2==0),active=2)
            assert float(row['value']) == expected[row['output_id']], row
        for suffix, order, threads in itertools.product(('csv','parquet','arrow'),itertools.permutations(range(3)),(1,8,32)):
            doc = copy.deepcopy(base)
            for binding,name in zip(doc['data'],('agents','gain','multiplier')): binding['source']=f'{name}.{suffix}'
            doc['data'] = [doc['data'][i] for i in order]
            save(doc)
            output = call('run',path,'--experiment',experiment,'--threads',threads)
            assert output == reference, (suffix,order,threads)
            observations += len(list(csv.DictReader(io.StringIO(output)))); configurations += 1
        # Parameter-only documents keep inline agents; async rates must be bound
        # before statechart construction and retain addressed stochastic draws.
        for name, value in (('typed_abm_sync.ir.json',2),('typed_abm_rates.ir.json',1.25)):
            original = json.loads((ROOT/'models'/name).read_text())
            key = original['parameters'][0]['id']; original['parameters'][0]['value']=value
            binding = parameter(key,value,'policy.csv')
            binding['schema']['columns'][1]['unit']=original['parameters'][0]['unit']
            save(original); expected=call('run',path,'--threads',8,'--seed',42)
            for population in (False,True):
                if population:
                    doc, records=bind(original)
                    columns=doc['data'][0]['schema']['columns']
                    with (inputs/'agents.csv').open('w',newline='') as file:
                        writer=csv.writer(file);writer.writerow(c['name'] for c in columns)
                        for i,record in enumerate(records):
                            writer.writerow([i]+[str(record[c['name']]).lower() if isinstance(record[c['name']],bool) else record[c['name']] for c in columns[1:]])
                else: doc=copy.deepcopy(original);doc['data']=[]
                doc['parameters'][0]['value']=99;doc['data'].append(binding)
                for reverse in (False,True):
                    if reverse: doc['data'].reverse()
                    save(doc);assert call('run',path,'--threads',32,'--seed',42)==expected
                    configurations+=1
        # Original declaration pointers survive mixed binding order.
        (inputs/'agents.csv').write_text('source_key,work,ready,role\n20,3,false,partner\n10,1,true,analyst\n')
        def reject(document,code,pointer):
            nonlocal controls
            save(document,False);call('lint',path,code=code,pointer=pointer);controls+=1
        bad=copy.deepcopy(base);bad['data'][2]['id']=bad['data'][0]['id'];reject(bad,'IR_ID','/data/2/id')
        bad=copy.deepcopy(base);bad['data'][2]['use']['parameters'][0]['parameter']='gain';reject(bad,'IR_DATA','/data/2/use/parameters/0/parameter')
        bad=copy.deepcopy(base);bad['data'].append(copy.deepcopy(bad['data'][0]));bad['data'][3]['id']='second_population';reject(bad,'IR_DATA','/data/3/use/kind')
        bad=copy.deepcopy(base);bad['data'][2]['schema']['columns'][1]['unit']='day';reject(bad,'IR_UNIT','/data/2/use/parameters/0/column')
        bad=copy.deepcopy(base);bad['data'][2]['source']='absent.csv';reject(bad,'DATA_IO','/data/2/source')
        bad=copy.deepcopy(base);bad['data'][2]['use']['kind']='exogenous_series';reject(bad,'IR_DATA','/data/2/use/kind')
        bad=copy.deepcopy(base);bad['components'][0]['agents']=agents;reject(bad,'IR_DATA','/components/0/agents')
        bad=copy.deepcopy(base);bad['data'].reverse();bad['data'][2]['use']['fields'][0]['field']='missing';reject(bad,'IR_REF','/data/2/use/fields/0/field')
        # Every composed source is recorded and travels with a portable bundle.
        bundles=[]
        for suffix in ('csv','parquet','arrow'):
            doc=copy.deepcopy(base)
            for binding,name in zip(doc['data'],('agents','gain','multiplier')):binding['source']=f'{name}.{suffix}'
            doc['data'].reverse();save(doc)
            output=root/f'results-{suffix}.parquet'
            call('run',path,'--experiment',experiment,'--out',output,'--threads',8)
            receipt=Path(str(output)+'.manifest.json');manifest=json.loads(receipt.read_text())
            assert len(manifest['inputs']['data'])==3
            for item in manifest['inputs']['data']:
                assert item['file_sha256']==hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()
            assert manifest['inputs']['scenarios'][0]['effective_parameters']=={'gain':2,'multiplier':3}
            assert manifest['inputs']['scenarios'][1]['effective_parameters']=={'gain':4,'multiplier':2}
            assert json.loads(call('replay',receipt))['result']==manifest['result']
            target=root/f'bundle-{suffix}';call('bundle',receipt,'--out',target)
            moved=root/f'relocated-{suffix}';target.rename(moved);bundles.append((moved,manifest))
        shutil.rmtree(inputs);experiment.unlink()
        for target, manifest in bundles:
            for threads in (1,8,32):
                result=json.loads(call('replay',target,'--threads',threads))
                assert result['result']==manifest['result'] and result['manifest_id']==manifest['id']
        # A pilot preparation example: five years of potential work capacity and
        # payroll, with table defaults and an explicit no-productivity scenario.
        demo=ROOT/'models/data/workforce_capacity.ir.json'
        rows=list(csv.DictReader(io.StringIO(call('run',demo,'--experiment',demo.with_suffix('').with_suffix('.experiment.json')))))
        for row in rows:
            month=float(row['time'])
            expected={'potential_work_hours':month*(280 if row['scenario']=='0' else 320),
                      'payroll':month*22400,'headcount':2}[row['output_id']]
            assert float(row['value'])==expected,row
        print(f'ABM composition: {configurations} ordering/format/thread cases, {observations} exact observations, '
              f'{controls} controls, 3 relocated bundles and {len(rows)} analytic workforce values; {calls} CLI calls')

if __name__ == '__main__':
    main()
