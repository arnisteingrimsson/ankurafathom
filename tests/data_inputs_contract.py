"""Independent rational interpolation and keyed-parameter mapping checks."""
import argparse
import copy
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile
from runtime_data_contract import canonical, require, write_fixture


def bits(value):return {'f64_bits':struct.unpack('<Q',struct.pack('<d',value))[0]}
def number(value):return struct.unpack('<d',struct.pack('<Q',value['f64_bits']))[0]


def main():
    parser=argparse.ArgumentParser();parser.add_argument('probe',type=Path);parser.add_argument('--report',type=Path)
    args=parser.parse_args();probe=str(args.probe.resolve());rng=random.Random(8417)
    series_cases=parameter_cases=compared=parameter_values=invalid=0;maximum_gap=0.;evidence=[]
    with tempfile.TemporaryDirectory(prefix='fathom-inputs-') as directory:
        root=Path(directory);request=root/'request.json'
        def run(path,spec):
            request.write_text(json.dumps(spec));return subprocess.run([probe,str(path),str(request)],capture_output=True)
        def good(path,spec):
            result=run(path,spec);require(result.returncode==0,result.stderr.decode());return json.loads(result.stdout)
        def bad(path,spec,code):
            nonlocal invalid
            result=run(path,spec);require(result.returncode==1 and not result.stdout,'invalid input published results')
            require(json.loads(result.stderr)['code']==code,result.stderr.decode());invalid+=1
        schema=dict(key='t',columns=[dict(name='t',type='f64',unit='day'),dict(name='factor',type='f64',unit='1')])
        def reference(rows,t,policy):
            ordered=sorted(rows,key=lambda r:r['t'])
            if t<=ordered[0]['t']:return ordered[0]['factor'],True,abs(ordered[0]['factor'])
            if t>=ordered[-1]['t']:return ordered[-1]['factor'],True,abs(ordered[-1]['factor'])
            for a,b in zip(ordered,ordered[1:]):
                if t==b['t']:return b['factor'],True,abs(b['factor'])
                if a['t']<=t<b['t']:
                    if policy=='hold':return a['factor'],True,abs(a['factor'])
                    alpha=(Fraction(t)-Fraction(a['t']))/(Fraction(b['t'])-Fraction(a['t']))
                    value=(1-alpha)*Fraction(a['factor'])+alpha*Fraction(b['factor'])
                    return float(value),False,max(abs(a['factor']),abs(b['factor']))
            raise AssertionError('uncovered query')
        for case in range(12):
            count=1 if case==0 else 3+case
            times=[-1.+i*.75 for i in range(count)]
            rows=[dict(t=t,factor=-0.0 if i==0 else (rng.randrange(-100,101))*.125) for i,t in enumerate(times)]
            queries=[-3.,20.,*times,*[math.nextafter(t,-math.inf) for t in times],*[i*.125 for i in range(45)]]
            grid=dict(start=0.125*(case%3),step=.125,count=47)
            rng.shuffle(rows)
            for suffix in ('csv','parquet','arrow'):
                path=root/f'series-{case}.{suffix}';declared=copy.deepcopy(schema)
                if case%2:declared['columns'].reverse()
                write_fixture(path,declared,rows,metadata=case%2==0,chunks=2)
                for policy in ('hold','linear'):
                    spec=dict(declared,series=dict(time='t',value='factor',time_unit='day',value_unit='1',interpolate=policy,queries=queries,grid=grid))
                    result=good(path,spec)['series'];logical,_=canonical(declared,rows)
                    require(result['file_hash']==hashlib.sha256(path.read_bytes()).hexdigest() and result['canonical_hash']==logical,'series provenance mismatch')
                    sampled_times=[grid['start']+i*grid['step'] for i in range(grid['count'])]
                    require(result['times']==[bits(t) for t in sampled_times],'sample clock evaluation changed')
                    for t,value in zip(queries+sampled_times,result['queries']+result['values']):
                        expected,exact,scale=reference(rows,t,policy);actual=number(value)
                        if exact:require(value==bits(expected),'hold/knot/endpoint float bits changed')
                        else:
                            gap=abs(actual-expected);maximum_gap=max(maximum_gap,gap)
                            require(math.isfinite(actual) and gap<=32*math.ulp(1.)*max(scale,1.),'linear value differs from rational oracle')
                        compared+=1
                    evidence.append(result['values']);series_cases+=1
        # Parameter source keys are never converted to double, even above 2^53.
        for key_type in ('string','u64'):
            params=dict(key='group',columns=[dict(name='group',type=key_type),dict(name='rate',type='f64',unit='1/day'),dict(name='initial',type='f64',unit='kg')])
            rows=[dict(group=f'g_{i}' if key_type=='string' else 2**64-1-i,rate=i*.125,initial=-0.0 if i==0 else -i*.5) for i in range(29)]
            keys=[r['group'] for r in rows];rng.shuffle(rows)
            for suffix in ('csv','parquet','arrow'):
                path=root/f'params-{key_type}.{suffix}';write_fixture(path,params,rows,dictionary=True,chunks=3)
                for order in (0,1):
                    fields=[dict(parameter='growth',column='rate',unit='1/day'),dict(parameter='stock',column='initial',unit='kg')]
                    if order:fields.reverse()
                    spec=dict(params,parameters=dict(fields=fields,keys=keys));actual=good(path,spec)['parameters'];logical,_=canonical(params,rows)
                    by_key={r['group']:r for r in rows}
                    wanted=[dict(key=k,values=dict(growth=bits(by_key[k]['rate']),stock=bits(by_key[k]['initial'])),
                        file_hash=hashlib.sha256(path.read_bytes()).hexdigest(),canonical_hash=logical) for k in keys]
                    require(actual==wanted,'parameter mapping/receipt differs from source values')
                    evidence.append(actual);parameter_cases+=1;parameter_values+=len(keys)*2
        path=root/'errors.csv';write_fixture(path,schema,[dict(t=0.,factor=1.),dict(t=1.,factor=2.)])
        spec=dict(schema,series=dict(time='t',value='factor',time_unit='day',value_unit='1',interpolate='linear',queries=[0.],grid=dict(start=0.,step=.1,count=3)))
        for key,value,code in [('time','factor','DATA_SERIES'),('value','absent','DATA_MAPPING'),('time_unit','week','DATA_UNIT'),('value_unit','kg','DATA_UNIT')]:
            altered=copy.deepcopy(spec);altered['series'][key]=value;bad(path,altered,code)
        for grid in [dict(start=-1.,step=1.,count=2),dict(start=0.,step=0.,count=2),dict(start=0.,step=1.,count=0),
                     dict(start=0.,step=1.,count=1000001),dict(start=1e30,step=1.,count=2),dict(start=1.7e308,step=1.7e308,count=2)]:
            altered=copy.deepcopy(spec);altered['series']['grid']=grid;bad(path,altered,'DATA_GRID')
        empty=root/'empty.arrow';write_fixture(empty,schema,[]);bad(empty,spec,'DATA_SERIES')
        ints=dict(key='t',columns=[dict(name='t',type='i64',unit='day'),dict(name='factor',type='f64',unit='1')])
        integer_file=root/'integer.arrow';write_fixture(integer_file,ints,[dict(t=0,factor=1.)]);bad(integer_file,dict(ints,series=spec['series']),'DATA_MAPPING')
        parameters=dict(schema,parameters=dict(fields=[dict(parameter='p',column='factor',unit='1')],keys=[100.]))
        bad(path,parameters,'DATA_KEY')
        for fields,code in [([], 'DATA_MAPPING'),([dict(parameter='',column='factor',unit='1')],'DATA_MAPPING'),
                            ([dict(parameter='p',column='factor',unit='kg')],'DATA_UNIT'),
                            ([dict(parameter='p',column='absent',unit='1')],'DATA_MAPPING'),
                            ([dict(parameter='p',column='factor',unit='1')]*2,'DATA_MAPPING')]:
            altered=copy.deepcopy(parameters);altered['parameters'].update(fields=fields,keys=[0.]);bad(path,altered,code)
    # Raw file hashes include writer details; exclude them from evidence identity.
    def stable(value):
        if isinstance(value,dict):return {k:stable(v) for k,v in value.items() if k!='file_hash'}
        if isinstance(value,list):return [stable(v) for v in value]
        return value
    report=dict(passed=True,series_configurations=series_cases,series_comparisons=compared,maximum_absolute_gap=maximum_gap,
                parameter_configurations=parameter_cases,parameter_values=parameter_values,invalid_cases=invalid,
                evidence_sha256=hashlib.sha256(json.dumps(stable(evidence),sort_keys=True).encode()).hexdigest())
    if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__=='__main__':main()
