"""Cross-format population identity and field mapping oracle."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile
from runtime_data_contract import canonical, require, write_fixture


def bits(value):
    return {'f64_bits': struct.unpack('<Q', struct.pack('<d', value))[0]}


def main():
    parser=argparse.ArgumentParser();parser.add_argument('probe',type=Path);parser.add_argument('--report',type=Path)
    args=parser.parse_args();probe=str(args.probe.resolve())
    rng=random.Random(48071);cases=agents=invalid=0;hashes=[]
    with tempfile.TemporaryDirectory(prefix='fathom-population-init-') as directory:
        root=Path(directory);request=root/'request.json'
        schema=dict(key='id',columns=[dict(name='id',type='string'),dict(name='active',type='bool'),
            dict(name='level',type='i32',unit='1'),dict(name='tenure',type='i64',unit='day'),
            dict(name='capacity',type='u64',unit='hours'),dict(name='rate',type='f64',unit='USD/hour'),dict(name='label',type='string')])
        pop=dict(store_id=0xffffffff,first_id=2**48-1024,
            schema=[dict(name='enabled',kind='boolean'),dict(name='rank',kind='integer'),dict(name='days',kind='integer'),
                    dict(name='hours',kind='integer'),dict(name='pay',kind='real'),dict(name='name',kind='string')],
            fields=[dict(field='enabled',column='active'),dict(field='rank',column='level',unit='1'),
                    dict(field='days',column='tenure',unit='day'),dict(field='hours',column='capacity',unit='hours'),
                    dict(field='pay',column='rate',unit='USD/hour'),dict(field='name',column='label')])
        source=[dict(id=f'person_{i:04d}',active=i%2==0,level=i%7-3,tenure=-2**63 if i==0 else 2**63-1 if i==1 else i*19,
                     capacity=2**63-1 if i==0 else i*8,rate=-0.0 if i==0 else 5e-324 if i==1 else (i-400)*.125,
                     label=f'group_{i%3},"é"\n') for i in range(257)]
        def run(path, definition):
            request.write_text(json.dumps(definition));return subprocess.run([probe,str(path),str(request)],capture_output=True)

        for variant in range(12):
            declared=copy.deepcopy(schema);mapping=copy.deepcopy(pop);rows=copy.deepcopy(source)
            if variant%2:
                declared['columns'][0]['type']='u64'
                for i,row in enumerate(rows):row['id']=2**64-1-i*65537
            rng.shuffle(rows);rng.shuffle(declared['columns']);rng.shuffle(mapping['fields']);rng.shuffle(mapping['schema'])
            for suffix in ('csv','parquet','arrow'):
                path=root/f'population-{variant}.{suffix}';write_fixture(path,declared,rows,dictionary=variant%2==0,metadata=variant%3==0,chunks=17)
                result=run(path,dict(declared,population=mapping));require(result.returncode==0,result.stderr.decode())
                actual=json.loads(result.stdout)['population'];canonical_hash,_=canonical(declared,rows)
                ordered=sorted(rows,key=lambda row:row['id'].encode() if isinstance(row['id'],str) else row['id'])
                wanted=[dict(key=row['id'],store=mapping['store_id'],id=mapping['first_id']+i,
                    values=dict(enabled=row['active'],rank=row['level'],days=row['tenure'],hours=row['capacity'],pay=bits(row['rate']),name=row['label']))
                    for i,row in enumerate(ordered)]
                require(actual['agents']==wanted,'population field/identity differs from independent source mapping')
                require(actual['vertices']==list(range(mapping['first_id'],mapping['first_id']+len(rows))),'initialized membership differs')
                require(actual['next_id']==mapping['first_id']+len(rows),'identity counter differs')
                require(actual['file_hash']==hashlib.sha256(path.read_bytes()).hexdigest() and actual['canonical_hash']==canonical_hash,'receipt data identity differs')
                cases+=1;agents+=len(rows);hashes.append(hashlib.sha256(json.dumps(wanted,sort_keys=True).encode()).hexdigest())
        path=root/'base.parquet';write_fixture(path,schema,source)
        for change,code in [('missing','DATA_MAPPING'),('duplicate','DATA_MAPPING'),('unknown_target','DATA_MAPPING'),
                            ('unknown_column','DATA_MAPPING'),('unit','DATA_UNIT'),('real_to_integer','DATA_MAPPING'),
                            ('boolean_to_integer','DATA_MAPPING'),('exhausted','DATA_POPULATION')]:
            bad=copy.deepcopy(pop)
            if change=='missing':bad['fields'].pop()
            elif change=='duplicate':bad['fields'][1]['field']=bad['fields'][0]['field']
            elif change=='unknown_target':bad['fields'][0]['field']='absent'
            elif change=='unknown_column':bad['fields'][0]['column']='absent'
            elif change=='unit':bad['fields'][4]['unit']='USD/day'
            elif change=='real_to_integer':bad['fields'][1].update(column='rate',unit='USD/hour')
            elif change=='boolean_to_integer':bad['fields'][1].update(column='active',unit='')
            else:bad['first_id']=2**48-100
            result=run(path,dict(schema,population=bad));require(result.returncode==1 and not result.stdout,'invalid binding published population')
            require(json.loads(result.stderr)['code']==code,result.stderr.decode());invalid+=1
        overflow=copy.deepcopy(source);overflow[-1]['capacity']=2**63
        write_fixture(path,schema,overflow);result=run(path,dict(schema,population=pop))
        require(result.returncode==1 and not result.stdout and json.loads(result.stderr)['code']=='DATA_RANGE','late integer narrowing accepted');invalid+=1
    report=dict(passed=True,configurations=cases,agents=agents,field_comparisons=agents*6,invalid_bindings=invalid,
                evidence_sha256=hashlib.sha256(json.dumps(hashes).encode()).hexdigest())
    if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__=='__main__':main()
