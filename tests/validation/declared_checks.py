"""Analytic and deliberately broken models for native declared validation."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe=Path(sys.argv[1]).resolve()
root=Path(__file__).resolve().parents[2]
base=dict(ir_version='0.1',name='closed transfer',time=dict(unit='day',dt=1,horizon=3),
    parameters=[dict(id='rate',value=2,unit='kg/day'),dict(id='total',value=10,unit='kg')],
    components=[dict(id='a',kind='stock',init=10,unit='kg'),dict(id='b',kind='stock',init=0,unit='kg'),
        dict(id='move',kind='flow',source='a',destination='b',expr='rate',unit='kg/day')],
    outputs=[dict(id='a_ts',stock='a'),dict(id='b_ts',stock='b')],checks=[
        dict(id='mass',kind='conserved',stocks=['a','b']),
        dict(id='range',kind='bounds',output='b_ts',min=0,max=6),
        dict(id='growth',kind='monotone',output='b_ts',direction='increasing'),
        dict(id='balance',kind='assert',expr='a+b == total'),
        dict(id='end',kind='assert',expr='b >= a',when='end')])
try:
    import jsonschema
    ir_schema=json.loads((root/'ir/schema/ankurafathom-ir.schema.json').read_text())
    report_schema=json.loads((root/'ir/schema/validation_report.schema.json').read_text())
except ImportError:jsonschema=None
count=0
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp);model=tmp/'model.json';experiment=tmp/'experiment.json'
    def invoke(doc,verdict='pass',args=(),schema=True):
        global count
        model.write_text(json.dumps(doc))
        if schema and jsonschema:jsonschema.validate(doc,ir_schema)
        proc=subprocess.run([str(exe),'check',str(model),*args],capture_output=True,text=True);count+=1
        assert proc.returncode==(1 if verdict=='fail' else 0),(proc.stdout,proc.stderr)
        report=json.loads(proc.stdout);assert report['verdict']==verdict,report
        if jsonschema:jsonschema.validate(report,report_schema)
        return report
    def outcome(report,id):return next(c for c in report['checks'] if c['id']==id)
    def failure(doc,id,code,time,actual,reference):
        report=invoke(doc,'fail');c=outcome(report,id)
        assert c['code']==code and c['pointer']=='/checks/'+str(next(i for i,x in enumerate(doc['checks']) if x['id']==id)),c
        assert c['first_failure']==dict(scenario=0,replication=0,time=time,actual=actual,reference=reference),c
        return c
    good=invoke(base)
    assert [outcome(good,c['id'])['samples'] for c in base['checks']]==[4,4,3,4,1]
    changed=copy.deepcopy(base);changed['components'][2]['destination']=None
    failure(changed,'mass','CHECK_CONSERVED',1,8,10)
    changed=copy.deepcopy(base);changed['checks'][1]['max']=5
    failure(changed,'range','CHECK_BOUNDS',3,6,5)
    changed=copy.deepcopy(base);changed['checks'][2]['direction']='decreasing'
    failure(changed,'growth','CHECK_MONOTONE',1,2,0)
    changed=copy.deepcopy(base);changed['checks'][3]['expr']='a == total'
    failure(changed,'balance','CHECK_ASSERT',1,8,10)
    changed=copy.deepcopy(base);changed['checks'][4]['when']='always'
    failure(changed,'end','CHECK_ASSERT',0,0,10)
    changed=copy.deepcopy(base);changed['checks'][1].pop('max');changed['checks'][1]['min']=1
    failure(changed,'range','CHECK_BOUNDS',0,0,1)
    # Tolerance acceptance and rejection around independently chosen thresholds.
    for kind in ('absolute_tolerance','relative_tolerance'):
        changed=copy.deepcopy(base);changed['checks']=[dict(id='tolerance',kind='assert',expr='total == total*1.01',**{kind:.101 if kind=='absolute_tolerance' else .01})]
        invoke(changed)
        changed['checks'][0][kind]=.099 if kind=='absolute_tolerance' else .009
        invoke(changed,'fail')
    # Finite inputs must not produce an inf <= inf false pass in tolerance math.
    changed=copy.deepcopy(base)
    changed['parameters'] += [dict(id='huge',value=1e308,unit='kg'),dict(id='negative',value=-1e308,unit='kg')]
    changed['checks']=[dict(kind='assert',expr='huge == negative',absolute_tolerance=1.7e308,relative_tolerance=.2)]
    r=invoke(changed,'fail');assert outcome(r,'check_0')['failures']==4
    changed['checks'][0]['relative_tolerance']=.4;invoke(changed)
    # Unrepresentable conservation totals fail with a sample location, never NaN JSON.
    changed=copy.deepcopy(base);changed['components'][0]['init']=1e308;changed['components'][1]['init']=1e308
    changed['checks']=[dict(kind='conserved',stocks=['a','b'])]
    r=invoke(changed,'fail');assert outcome(r,'check_0')['code']=='CHECK_EVALUATION'
    assert outcome(r,'check_0')['first_error']['time']==0
    # Zero is an explicit dimensional issue; authors normalize instead of hidden unit coercion.
    invalids=[]
    for edit,code,pointer in [
        ({'kind':'unknown'},'IR_CHECK','/checks/0/kind'),
        ({'kind':'assert','expr':'a == rate'},'IR_UNIT','/checks/0/expr'),
        ({'kind':'assert','expr':'missing == total'},'IR_SYMBOL','/checks/0/expr'),
        ({'kind':'assert','expr':'a < total'},'IR_CHECK','/checks/0/expr'),
        ({'kind':'assert','expr':'a <= total <= b'},'IR_EXPR','/checks/0/expr'),
        ({'kind':'assert','expr':'a == 0'},'IR_UNIT','/checks/0/expr'),
        ({'kind':'assert','expr':'a == total','when':'sometimes'},'IR_CHECK','/checks/0/when'),
        ({'kind':'bounds','output':'absent','min':0},'IR_REF','/checks/0/output'),
        ({'kind':'bounds','output':'a_ts'},'IR_CHECK','/checks/0'),
        ({'kind':'bounds','output':'a_ts','min':1,'max':0},'IR_CHECK','/checks/0'),
        ({'kind':'monotone','output':'a_ts','direction':'sideways'},'IR_CHECK','/checks/0/direction'),
        ({'kind':'conserved','stocks':['a','a']},'IR_REF','/checks/0/stocks/1'),
        ({'kind':'conserved','stocks':['missing']},'IR_REF','/checks/0/stocks/0'),
        ({'kind':'conserved','stocks':[]},'IR_TYPE','/checks/0/stocks'),
        ({'kind':'bounds','output':'a_ts','min':0,'absolute_tolerance':-1},'IR_CHECK','/checks/0/absolute_tolerance'),
        ({'kind':'bounds','output':'a_ts','min':0,'extra/field':True},'IR_FIELD','/checks/0/extra~1field')]:
        changed=copy.deepcopy(base);changed['checks']=[edit];invalids.append((changed,code,pointer))
    changed=copy.deepcopy(base);changed['checks'][1]['id']='mass';invalids.append((changed,'IR_ID','/checks/1/id'))
    changed=copy.deepcopy(base);changed['checks']=[];invalids.append((changed,'IR_TYPE','/checks'))
    changed=copy.deepcopy(base);changed['mode']='des';invalids.append((changed,'IR_CHECK_SCOPE','/checks'))
    changed=copy.deepcopy(base);changed['components'][1]['unit']='USD';changed['components'][2]['destination']=None
    invalids.append((changed,'IR_UNIT','/checks/0/stocks/1'))
    for doc,code,pointer in invalids:
        report=invoke(doc,'fail',schema=False);c=report['checks'][0]
        assert c['code']==code and c['pointer']==pointer,(c,code,pointer)
        lint=subprocess.run([str(exe),'lint',str(model)],capture_output=True,text=True);count+=1
        assert lint.returncode==1
        diagnostic=json.loads(lint.stderr)['diagnostics'][0]
        assert (diagnostic['code'],diagnostic['pointer'])==(code,pointer),diagnostic
    # No vacuous scientific pass for a model with no declarations.
    changed=copy.deepcopy(base);changed.pop('checks');r=invoke(changed,'warn')
    assert outcome(r,'declarations')['code']=='CHECK_NONE'
    # Assertion evaluation failure maps private observation back to model declaration.
    changed=copy.deepcopy(base);changed['checks']=[dict(kind='assert',expr='a/0 == total')]
    r=invoke(changed,'fail');assert outcome(r,'check_0')['pointer']=='/checks/0'
    assert outcome(r,'check_0')['code']=='CHECK_EVALUATION'
    assert outcome(r,'check_0')['first_error']['time']==0
    # End-only assertions must not evaluate undefined expressions at earlier times.
    changed=copy.deepcopy(base);changed['checks']=[dict(kind='assert',expr='a/b >= 0',when='end')]
    invoke(changed)
    changed['checks'][0]['when']='always';invoke(changed,'fail')
    # Hidden observer names cannot collide with user output names or mutate run results.
    changed=copy.deepcopy(base);changed['outputs'].append(dict(id='__fathom_check_3_a',stock='a'))
    r=invoke(changed)
    result=tmp/'observations.csv'
    subprocess.run([str(exe),'run',str(model),'--out',str(result)],check=True,capture_output=True);count+=1
    manifest=json.loads(Path(str(result)+'.manifest.json').read_text())
    assert r['result']==manifest['result']
    assert r['inputs']['model']==manifest['inputs']['model']
    # Replicas/scenarios are checked independently. Thread count cannot change first failure.
    experiment.write_text(json.dumps(dict(seed=7,replications=3,scenarios=[dict(id=2,parameters={'rate':2}),dict(id=9,parameters={'rate':3})])))
    first=None
    for threads in (1,8,32):
        r=invoke(base,'fail',['--experiment',str(experiment),'--threads',str(threads)])
        assert outcome(r,'range')['first_failure']==dict(scenario=9,replication=0,time=3,actual=9,reference=6)
        assert outcome(r,'range')['failures']==3
        assert outcome(r,'mass')['samples']==24
        if first is not None:assert r['checks']==first['checks'] and r['result']==first['result']
        first=r
    # Stable absolute time including the initial sample and an end-only assertion.
    changed=copy.deepcopy(base);changed['time']['start']=10
    changed['checks'][1]['max']=5
    failure(changed,'range','CHECK_BOUNDS',13,6,5)
    # Report publication on pass and failure; existing destinations remain byte-identical.
    report_path=tmp/'validation.json'
    r=invoke(base,args=['--out',str(report_path)])
    assert json.loads(report_path.read_text())==r
    before=report_path.read_bytes()
    proc=subprocess.run([str(exe),'check',str(model),'--out',str(report_path)],capture_output=True,text=True);count+=1
    assert proc.returncode==1 and report_path.read_bytes()==before
    failed=tmp/'failed.json';changed=copy.deepcopy(base);changed['checks'][1]['max']=5
    r=invoke(changed,'fail',['--out',str(failed)]);assert json.loads(failed.read_text())==r
    for args in (['--threads','0'],['--threads','257'],['--threads','-1'],['--seed','x'],['--threads','1','--threads','2'],['--fit','yes'],['--out']):
        proc=subprocess.run([str(exe),'check',str(model),*args],capture_output=True,text=True);count+=1
        assert proc.returncode==1 and json.loads(proc.stderr)['diagnostics'][0]['code']=='IR_USAGE'
    # Native run gate recomputes the exact requested experiment before publication.
    for doc,should_pass in [(base,True),(changed,False),({k:v for k,v in base.items() if k!='checks'},False)]:
        model.write_text(json.dumps(doc));destination=tmp/f'gated-{count}.csv'
        proc=subprocess.run([str(exe),'run',str(model),'--require-check','--out',str(destination)],capture_output=True,text=True);count+=1
        assert (proc.returncode==0)==should_pass,(proc.stdout,proc.stderr)
        assert destination.exists()==should_pass
        assert Path(str(destination)+'.manifest.json').exists()==should_pass
        if not should_pass:assert json.loads(proc.stderr)['diagnostics'][0]['code']=='IR_CHECK_FAILED'
    model.write_text(json.dumps(base))
    proc=subprocess.run([str(exe),'run',str(model),'--require-check','--experiment',str(experiment)],capture_output=True,text=True);count+=1
    assert proc.returncode==1 and not proc.stdout
    print(f'Native validation: {count} CLI checks; four rule kinds, analytic failure locations, units, tolerance, experiment ordering, schema and unchanged run identities pass (schema={bool(jsonschema)})')
