"""Public population binding parity against validated inline ABM fixtures."""
import argparse
import copy
import csv
import io
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
TYPES={'real':'f64','integer':'i64','boolean':'bool','string':'string'}


def bind(model, source='agents.csv'):
    model=copy.deepcopy(model);pop=model['components'][0];agents=pop.pop('agents')
    model['data']=[{'id':'population_data','source':source,
        'schema':{'key_column':'source_key','columns':[{'name':'source_key','type':'u64','unit':''}]+[
            {'name':f['name'],'type':TYPES[f['type']],'unit':f['unit']} for f in pop['fields']]},
        'use':{'kind':'population_init','population':pop['id'],
               'fields':[{'field':f['name'],'column':f['name']} for f in pop['fields']]}}]
    return model,agents


def main():
    p=argparse.ArgumentParser();p.add_argument('executable',type=Path)
    p.add_argument('--without-arrow',action='store_true');p.add_argument('--schema',action='store_true')
    args=p.parse_args();exe=args.executable.resolve();validator=None
    if args.schema:
        from jsonschema import Draft202012Validator
        validator=Draft202012Validator(json.loads((ROOT/'ir/schema/ankurafathom-ir.schema.json').read_text()))
    calls=fixtures=configurations=observations=0
    with tempfile.TemporaryDirectory() as tmp:
        root=Path(tmp);path=root/'model.json';source=root/'agents.csv'
        def call(doc,command='lint',extra=(),code=None,pointer=None):
            nonlocal calls
            calls+=1
            if validator and code is None: validator.validate(doc)
            path.write_text(json.dumps(doc))
            r=subprocess.run([str(exe),command,str(path),*map(str,extra)],cwd=root.parent,capture_output=True,text=True)
            if code:
                assert r.returncode==1,(code,r.stdout,r.stderr)
                error=json.loads(r.stderr)['diagnostics'][0];assert error['code']==code,error
                if pointer is not None: assert error['pointer']==pointer,error
            else: assert r.returncode==0,r.stderr
            return r.stdout
        inline=json.loads((ROOT/'models/typed_abm_sync.ir.json').read_text());base,agents=bind(inline)
        source.write_text('source_key,work,ready,role\n18446744073709551614,1,true,analyst\n18446744073709551615,3,false,partner\n')
        if args.without_arrow:
            call(base,code='DATA_UNAVAILABLE',pointer='/data/0/source')
            call(inline,'run');print('Population bindings fail closed without Arrow; inline ABM remains available');return
        import pyarrow as pa
        import pyarrow.ipc as ipc
        import pyarrow.parquet as pq
        pa_types={'f64':pa.float64(),'i32':pa.int32(),'i64':pa.int64(),'u64':pa.uint64(),'bool':pa.bool_(),'string':pa.string()}
        experiment=root/'experiment.json';experiment.write_text(json.dumps({'seed':37,'replications':2,
            'scenarios':[{'id':0,'parameters':{}},{'id':17,'parameters':{}}]}))
        for fixture in sorted((ROOT/'models').glob('typed_abm_*.ir.json')):
            model=json.loads(fixture.read_text());bound,records=bind(model)
            if not records: continue
            expected=call(model,'run',('--experiment',experiment));fixtures+=1
            columns=bound['data'][0]['schema']['columns']
            # High external keys retain row order without becoming simulation IDs.
            rows=[{'source_key':2**64-len(records)+i,**record} for i,record in enumerate(records)]
            for reversed_ in (False,True):
                ordered=rows[::-1] if reversed_ else rows
                table=pa.table({c['name']:pa.array([r[c['name']] for r in ordered],type=pa_types[c['type']]) for c in columns})
                with source.open('w',newline='') as f:
                    writer=csv.writer(f);writer.writerow([c['name'] for c in columns])
                    for row in ordered: writer.writerow([str(row[c['name']]).lower() if isinstance(row[c['name']],bool) else row[c['name']] for c in columns])
                pq.write_table(table,root/'agents.parquet')
                with ipc.new_file(root/'agents.arrow',table.schema) as writer: writer.write_table(table)
                for suffix in ('csv','parquet','arrow'):
                    bound['data'][0]['source']=f'agents.{suffix}'
                    bound['data'][0]['use']['fields'].reverse()
                    threads=1 if not reversed_ else 8
                    output=call(bound,'run',('--experiment',experiment,'--threads',threads))
                    assert output==expected,(fixture.name,suffix,reversed_)
                    observations+=len(list(csv.DictReader(io.StringIO(output))));configurations+=1
        source.write_text('source_key,work,ready,role\n18446744073709551614,1,true,analyst\n18446744073709551615,3,false,partner\n')
        expected=call(inline,'run')
        for type_ in ('i32','u64'):
            doc=copy.deepcopy(base);doc['data'][0]['schema']['columns'][1]['type']=type_
            assert call(doc,'run')==expected
        # Empty sources remain subject to field and model-reference validation.
        source.write_text('source_key,work,ready,role\n')
        empty=copy.deepcopy(base);empty['outputs']=[{'id':'active','metric':'active'}]
        assert all(float(r['value'])==0 for r in csv.DictReader(io.StringIO(call(empty,'run'))))
        call(base,code='IR_REF')
        source.write_text('source_key,work,ready,role\n18446744073709551614,1,true,analyst\n18446744073709551615,3,false,partner\n')
        structural=[
            (['data'],[], 'IR_TYPE','/data'),
            (['data',0,'required'],False,'IR_DATA','/data/0/required'),
            (['data',0,'use','fields'],[],'IR_TYPE','/data/0/use/fields'),
            (['data',0,'use','extra'],1,'IR_FIELD','/data/0/use/extra'),
            (['components',0,'agents'],[],'IR_DATA','/components/0/agents'),
            (['data',0,'use','kind'],'parameter_table','IR_FIELD','/data/0/use/fields')]
        semantic=[
            (['data',0,'use','population'],'missing','IR_REF','/data/0/use/population'),
            (['data',0,'use','fields',0,'field'],'missing','IR_REF','/data/0/use/fields/0/field'),
            (['data',0,'use','fields',0,'column'],'missing','IR_REF','/data/0/use/fields/0/column'),
            (['data',0,'schema','columns',1,'unit'],'day','IR_UNIT','/data/0/use/fields/0/column'),
            (['data',0,'schema','columns',1,'type'],'f64','DATA_MAPPING','/data/0/use/fields/0/column'),
            (['components',0,'agent_limit'],1,'IR_DATA','/data/0/source'),
            (['data',0,'source'],'missing.csv','DATA_IO','/data/0/source'),
            (['data',0,'source'],'https://example.invalid/agents.csv','IR_DATA','/data/0/source')]
        for cases in (structural,semantic):
            for steps,value,code,pointer in cases:
                doc=copy.deepcopy(base);cursor=doc
                for step in steps[:-1]:cursor=cursor[step]
                cursor[steps[-1]]=value
                if validator and cases is structural:assert not validator.is_valid(doc),steps
                call(doc,code=code,pointer=pointer)
        for missing in (True,False):
            doc=copy.deepcopy(base);fields=doc['data'][0]['use']['fields']
            if missing:fields.pop()
            else:fields[1]=copy.deepcopy(fields[0])
            call(doc,code='DATA_MAPPING',pointer='/data/0/use/fields' if missing else '/data/0/use/fields/1/field')
        doc=copy.deepcopy(base);doc['data'].append(copy.deepcopy(doc['data'][0]));call(doc,code='IR_ID',pointer='/data/1/id')
        # Field integers retain the inline IR exact-numeric range; source keys do not.
        boundary=copy.deepcopy(base);boundary['components'][0]['phases']=[]
        boundary['outputs']=[{'id':'work','agent':0,'field':'work'}]
        for value in (-(2**53-1),2**53-1):
            source.write_text(f'source_key,work,ready,role\n18446744073709551615,{value},true,a\n')
            rows=list(csv.DictReader(io.StringIO(call(boundary,'run'))));assert all(int(float(r['value']))==value for r in rows)
        for type_,value in [('i64',2**53),('i64',-(2**53)),('u64',2**63),('u64',2**64-1)]:
            doc=copy.deepcopy(boundary);doc['data'][0]['schema']['columns'][1]['type']=type_
            source.write_text(f'source_key,work,ready,role\n18446744073709551615,{value},true,a\n')
            call(doc,code='DATA_RANGE',pointer='/data/0/rows/0/work')
        source.write_text('source_key,work,ready,role\n0,1,true,a\n0,2,false,b\n');call(base,code='DATA_KEY')
        source.write_text('source_key,work,ready,role\n0,1,true,a\n1,2,false,b\n')
        alias=root/'alias.csv';alias.symlink_to(source);hard=root/'hard.csv';hard.hardlink_to(source)
        before=source.read_bytes()
        for out in (source,alias,hard):
            call(base,'run',('--out',out),code='IR_USAGE',pointer='');assert source.read_bytes()==before
        # Native spatial/statechart invariants must still reject invalid bound records.
        for filename,field,value,code,pointer in [
            ('typed_abm_async.ir.json','generation',0,'IR_ABM','/data/0/source'),
            ('typed_abm_neighbors.ir.json','x',5,'IR_ABM','/components/0/space')]:
            doc,records=bind(json.loads((ROOT/'models'/filename).read_text()))
            records[0][field]=value;columns=doc['data'][0]['schema']['columns']
            if field=='x':doc['components'][0]['space']['wrap']=False
            with source.open('w',newline='') as f:
                writer=csv.writer(f);writer.writerow([c['name'] for c in columns])
                for i,record in enumerate(records):
                    row={'source_key':i,**record};writer.writerow([str(row[c['name']]).lower() if isinstance(row[c['name']],bool) else row[c['name']] for c in columns])
            call(doc,code=code,pointer=pointer)
        print(f'{calls} CLI checks; {fixtures} inline ABM fixtures; {configurations} format/order configurations; {observations} identical observations')


if __name__=='__main__':main()
