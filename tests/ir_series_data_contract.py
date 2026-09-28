"""Independent rational stage evaluation for public exogenous-series IR."""
import argparse
import bisect
import copy
import csv
from fractions import Fraction as F
import io
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
KNOTS = [F(-1,2), F(3,4), F(3,2), F(13,4)]
VALUES = [F(1), F(3), F(1,2), F(2)]


def sample(t, policy):
    i = bisect.bisect_right(KNOTS, t)-1
    if i < 0: return VALUES[0]
    if i == len(KNOTS)-1 or policy == 'hold': return VALUES[i]
    return VALUES[i]+(VALUES[i+1]-VALUES[i])*(t-KNOTS[i])/(KNOTS[i+1]-KNOTS[i])


def main():
    p = argparse.ArgumentParser();p.add_argument('executable', type=Path)
    p.add_argument('--without-arrow', action='store_true');p.add_argument('--schema', action='store_true')
    args = p.parse_args();exe = args.executable.resolve()
    validator = None
    if args.schema:
        from jsonschema import Draft202012Validator
        validator = Draft202012Validator(json.loads((ROOT/'ir/schema/ankurafathom-ir.schema.json').read_text()))
    base = json.loads((ROOT/'models/data/seasonal_stock.ir.json').read_text())
    base['parameters'] += [{'id':'loss', 'value':.125, 'unit':'1/day'}, {'id':'lag', 'value':.25, 'unit':'day'}]
    base['components'][1]['expr'] = 'rate * seasonality(t) - loss * total'
    base['components'][1]['non_negative'] = False
    base['outputs'].append({'id':'shifted', 'expr':'seasonality(t-lag)', 'unit':'1'})
    calls = comparisons = configurations = 0;max_gap = 0.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp);path=root/'model.json';source=root/'series.csv'
        source.write_text('time,factor\n'+''.join(f'{float(t)},{float(v)}\n' for t,v in reversed(list(zip(KNOTS,VALUES)))))
        base['data'][0]['source']='series.csv'
        def call(doc, command='lint', extra=(), code=None, pointer=None):
            nonlocal calls
            calls += 1
            if validator and code is None: validator.validate(doc)
            path.write_text(json.dumps(doc))
            r = subprocess.run([str(exe),command,str(path),*map(str,extra)],cwd=root.parent,capture_output=True,text=True)
            if code:
                assert r.returncode == 1, (code,r.stdout,r.stderr)
                error = json.loads(r.stderr)['diagnostics'][0]
                assert error['code'] == code, error
                if pointer is not None: assert error['pointer'] == pointer,error
            else: assert r.returncode == 0,r.stderr
            return r.stdout
        if args.without_arrow:
            call(base,code='DATA_UNAVAILABLE',pointer='/data/0/source')
            print('Series input fails closed without Arrow');return
        import pyarrow as pa
        import pyarrow.ipc as ipc
        import pyarrow.parquet as pq
        table = pa.table({'time':list(map(float,KNOTS)), 'factor':list(map(float,VALUES))})
        pq.write_table(table,root/'series.parquet')
        with ipc.new_file(root/'series.arrow',table.schema) as writer: writer.write_table(table)
        experiment=root/'experiment.json'
        experiment.write_text(json.dumps({'seed':4,'replications':2,'scenarios':[
            {'id':0,'parameters':{}},{'id':1,'parameters':{'rate':3}}]}))
        format_refs={}
        for suffix in ('csv','parquet','arrow'):
            for policy in ('hold','linear'):
                for integrator in ('euler','rk4'):
                    for start,dt in ((F(0),F(1,2)),(F(1,4),F(1))):
                        doc=copy.deepcopy(base);doc['data'][0]['source']=f'series.{suffix}'
                        doc['data'][0]['use']['interpolate']=policy;doc['integrator']=integrator
                        doc['time'].update(start=float(start),dt=float(dt),horizon=float(4*dt))
                        ref={}
                        for scenario,rate in ((0,F(2)),(1,F(3))):
                            x=F(0)
                            for step in range(5):
                                t=start+step*dt
                                ref[scenario,float(t),'total_ts']=x
                                ref[scenario,float(t),'factor_ts']=sample(t,policy)
                                ref[scenario,float(t),'shifted']=sample(t-F(1,4),policy)
                                f=lambda time,state: rate*sample(time,policy)-state/F(8)
                                if integrator=='euler': x += dt*f(t,x)
                                else:
                                    k1=f(t,x);k2=f(t+dt/2,x+dt*k1/2)
                                    k3=f(t+dt/2,x+dt*k2/2);k4=f(t+dt,x+dt*k3)
                                    x += dt*(k1+2*k2+2*k3+k4)/6
                        key=policy,integrator,start,dt
                        for threads in (1,8,32):
                            output=call(doc,'run',('--experiment',experiment,'--threads',threads))
                            if key in format_refs: assert output==format_refs[key]
                            else: format_refs[key]=output
                            for row in csv.DictReader(io.StringIO(output)):
                                expected=float(ref[int(row['scenario']),float(row['time']),row['output_id']])
                                gap=abs(float(row['value'])-expected);max_gap=max(max_gap,gap)
                                assert gap<2e-13,(row,expected,gap)
                                comparisons+=1
                        configurations+=1
        # Parameter tables and series coexist; declaration order is irrelevant.
        doc=copy.deepcopy(base)
        (root/'rate.csv').write_text('key,rate\na,4\n')
        binding={'id':'rates','source':'rate.csv','schema':{'key_column':'key','columns':[
            {'name':'key','type':'string','unit':''},{'name':'rate','type':'f64','unit':'kg/day'}]},
            'use':{'kind':'parameter_table','key':'a','parameters':[{'parameter':'rate','column':'rate'}]}}
        doc['data'].append(binding)
        output=call(doc,'run');doc['data'].reverse();assert call(doc,'run')==output
        direct=copy.deepcopy(base);direct['parameters'][0]['value']=4
        assert call(direct,'run')==output
        # Holding beyond both endpoints, signed-zero knots, and singleton tables.
        (root/'singleton.csv').write_text('time,factor\n10,-0\n')
        doc=copy.deepcopy(base);doc['data'][0]['source']='singleton.csv'
        for policy in ('hold','linear'):
            doc['data'][0]['use']['interpolate']=policy
            rows=list(csv.DictReader(io.StringIO(call(doc,'run'))))
            assert all(float(r['value'])==0 for r in rows)
            assert all(r['value'].startswith('-') for r in rows if r['output_id']=='factor_ts')
        # Delay inputs and initial duration checks can call series functions.
        delay_input=json.loads((ROOT/'models/variable_delay.ir.json').read_text())
        delay_input['data']=copy.deepcopy(base['data'])
        delay_input['components'][2]['input']='intake * seasonality(t)'
        delay_input['components'][3]['input']='intake * seasonality(t)'
        rows=list(csv.DictReader(io.StringIO(call(delay_input,'run'))))
        for name in ('material_out','signal_out'):
            observed=[float(row['value']) for row in rows if row['output_id']==name and float(row['time'])==1]
            assert observed==[14.0],observed
        delay=json.loads((ROOT/'models/variable_delay.ir.json').read_text())
        delay['data']=copy.deepcopy(base['data']);delay['data'][0]['schema']['columns'][1]['unit']='day'
        delay['components'][2]['duration']='seasonality(t)'
        call(delay)
        (root/'duration.csv').write_text('time,factor\n0,0.5\n4,0.5\n')
        delay['data'][0]['source']='duration.csv'
        call(delay,code='IR_DELAY',pointer='/components/2/duration')
        structural=[
            (['data',0,'use','interpolate'],'cubic','IR_DATA','/data/0/use/interpolate'),
            (['data',0,'use','extrapolate'],'linear','IR_DATA','/data/0/use/extrapolate'),
            (['data',0,'use','key'],0,'IR_FIELD','/data/0/use/key'),
            (['mode'],'agent_stock_sd','IR_DATA','/data')]
        semantic=[
            (['data',0,'id'],'STEP','IR_ID','/data/0/id'),
            (['data',0,'id'],'t','IR_ID','/data/0/id'),
            (['data',0,'id'],'rate','IR_ID','/data/0/id'),
            (['data',0,'id'],'total','IR_ID','/components/0/id'),
            (['data',0,'schema','columns',0,'unit'],'week','IR_UNIT','/data/0/use/time_column'),
            (['data',0,'schema','columns',1,'unit'],'','IR_UNIT','/data/0/use/value_column'),
            (['data',0,'schema','columns',0,'type'],'i64','IR_DATA','/data/0/use/time_column'),
            (['data',0,'schema','columns',1,'type'],'i64','IR_DATA','/data/0/use/value_column'),
            (['data',0,'use','time_column'],'factor','IR_DATA','/data/0/use/time_column'),
            (['data',0,'use','value_column'],'missing','IR_DATA','/data/0/use/value_column'),
            (['components',1,'expr'],'rate * seasonality(rate)','IR_UNIT','/components/1/expr'),
            (['outputs',1,'unit'],'kg','IR_UNIT','/outputs/1/unit')]
        for mutations in (structural,semantic):
            for steps,value,code,pointer in mutations:
                doc=copy.deepcopy(base);cursor=doc
                for step in steps[:-1]: cursor=cursor[step]
                cursor[steps[-1]]=value
                if validator and mutations is structural: assert not validator.is_valid(doc)
                call(doc,code=code,pointer=pointer)
        doc=copy.deepcopy(base);doc['data'].append(copy.deepcopy(doc['data'][0]))
        call(doc,code='IR_ID',pointer='/data/1/id')
        # Local aliases are protected just like parameter-table sources.
        alias=root/'alias.csv';alias.symlink_to(source)
        hard=root/'hard.csv';hard.hardlink_to(source)
        before=source.read_bytes()
        for output in (source,alias,hard):
            call(base,'run',('--out',output),code='IR_USAGE',pointer='')
            assert source.read_bytes()==before
        source.write_text('time,factor\n')
        call(base,code='DATA_SERIES',pointer='/data/0/source')
        source.write_text('time,factor\n0,1\n0,2\n')
        call(base,code='DATA_KEY')
        source.write_text('time,factor\n0,nan\n')
        call(base,code='DATA_VALUE',pointer='/data/0/rows/0/factor')
        print(f'{calls} CLI checks; {configurations} format/policy/integrator/grid configurations; '
              f'{comparisons} rational comparisons; max gap {max_gap:.17g}')


if __name__=='__main__': main()
