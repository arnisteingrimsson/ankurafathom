"""Independent discrete accounting, provenance and adversarial explanation checks."""
import copy
import csv
import io
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from html.parser import HTMLParser

exe=Path(sys.argv[1]).resolve();ROOT=Path(__file__).resolve().parents[2]
try:
    from jsonschema import Draft202012Validator
    schema=Draft202012Validator(json.loads((ROOT/'ir/schema/explanation_report.schema.json').read_text()))
except ImportError:schema=None
count=0
class HTMLAudit(HTMLParser):
    def __init__(self):super().__init__();self.svg=0;self.remote=[];self.scripts=0;self.text=[]
    def handle_starttag(self,tag,attrs):
        self.svg+=tag=='svg';self.scripts+=tag=='script'
        for k,v in attrs:
            if k in ('src','href') and v:self.remote.append(v)
    def handle_data(self,text):self.text.append(text)
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp)
    def invoke(args,code=None):
        global count
        p=subprocess.run([str(exe),*map(str,args)],text=True,capture_output=True);count+=1
        if code:
            assert p.returncode==1,(args,p.stdout,p.stderr)
            d=json.loads(p.stderr)['diagnostics'][0];assert d['code']==code,(args,d,code)
            return d
        assert p.returncode==0,(args,p.stdout,p.stderr)
        return p.stdout
    def save(doc,name,experiment=None,fmt='csv'):
        model=tmp/(name+'.json');model.write_text(json.dumps(doc))
        result=tmp/(name+'.'+fmt);args=['run',model,'--out',result]
        if experiment:
            exp=tmp/(name+'-experiment.json');exp.write_text(json.dumps(experiment));args+=['--experiment',exp]
        invoke(args);return model,result,Path(str(result)+'.manifest.json')
    def explain(path,output='material_ts',at=1,extra=()):
        result=json.loads(invoke(['explain',path,'--output',output,'--at',at,*extra]))
        if schema:schema.validate(result)
        return result
    base=json.loads((ROOT/'models/decay.ir.json').read_text())
    for method in ('euler','rk4'):
        model=copy.deepcopy(base);model['integrator']=method
        experiment=dict(seed=42,replications=2,scenarios=[dict(id=0,parameters={}),dict(id=7,parameters={'decay_rate':.4})])
        path,result,manifest=save(model,method,experiment)
        for s,k in ((0,.2),(7,.4)):
            z=-k*.1;factor=1+z if method=='euler' else 1+z+z*z/2+z**3/6+z**4/24
            for rep in (0,1):
                expected_start=100*factor**9;expected_end=100*factor**10
                first=None
                for threads in (1,8):
                    r=explain(result,extra=['--scenario',s,'--replication',rep,'--threads',threads])
                    assert math.isclose(r['start_value'],expected_start,abs_tol=1e-12)
                    assert math.isclose(r['value'],expected_end,abs_tol=1e-12)
                    flow=r['accounting']['flows'][0]
                    assert flow['flow']=='loss' and flow['endpoint_sign']==-1
                    assert math.isclose(flow['contribution'],expected_end-expected_start,abs_tol=1e-12)
                    assert abs(r['accounting']['rounding_residual'])<1e-12
                    nodes={n['id']:n for n in r['dependencies']['nodes']}
                    assert nodes['decay_rate']['value']==k and nodes['decay_rate']['scenario_override']==(s==7)
                    assert r['verification']['artifact']=='verified'
                    if first is not None:assert r==first
                    first=r
        r=explain(manifest,extra=['--from',0]);assert r['window']['steps']==10
        assert math.isclose(r['accounting']['flow_total'],100*( (1-.02 if method=='euler' else 1-.02+.02**2/2-.02**3/6+.02**4/24)**10 -1),abs_tol=1e-12)
        initial=explain(result,at=0);assert initial['accounting']['flow_total']==0 and initial['window']['steps']==0
        for args,code in [(['--at','.35'],'IR_EXPLAIN_TIME'),(['--at','2'],'IR_EXPLAIN_TIME'),
                          (['--at','1','--from','2'],'IR_EXPLAIN_TIME'),(['--at','.5','--from','.6'],'IR_EXPLAIN_TIME'),
                          (['--at','1','--scenario','9'],'IR_EXPLAIN_ADDRESS'),(['--at','1','--replication','2'],'IR_EXPLAIN_ADDRESS')]:
            invoke(['explain',result,'--output','material_ts',*args],code)
        invoke(['explain',result,'--output','missing','--at','1'],'IR_EXPLAIN_OUTPUT')
    if '--without-arrow' not in sys.argv:
        for fmt in ('parquet','arrow'):
            path,result,manifest=save(base,'binary-'+fmt,fmt=fmt)
            r=explain(result)
            assert r['verification']['artifact']=='verified'
            assert r['result_sha256']==json.loads(manifest.read_text())['result']['sha256']
    # Output/stock namespaces and passive-generated names must remain distinct.
    model=copy.deepcopy(base);model['outputs']=[dict(id='material',stock='material'),dict(id='__explain_integral_loss',stock='material'),dict(id='fraction',expr='material/material',unit='1')]
    path,result,manifest=save(model,'collisions')
    r=explain(result,'material');assert len({n['id'] for n in r['dependencies']['nodes']})==len(r['dependencies']['nodes'])
    assert {'material','output:material'}<={n['id'] for n in r['dependencies']['nodes']}
    r=explain(result,'fraction');assert r['accounting'] is None and r['value']==1
    # Signed, self-loop and split transfers use endpoint signs, not positive-only magnitudes.
    model=dict(ir_version='0.1',name='split',time=dict(unit='day',dt=1,horizon=2),parameters=[dict(id='rate',value=2,unit='kg/day')],components=[
        dict(id='a',kind='stock',init=10,unit='kg'),dict(id='b',kind='stock',init=0,unit='kg'),
        dict(id='transfer',kind='flow',source='a',destination='b',expr='rate',unit='kg/day'),
        dict(id='return',kind='flow',source=None,destination='a',expr='-rate/2',unit='kg/day',non_negative=False),
        dict(id='loop',kind='flow',source='a',destination='a',expr='rate',unit='kg/day')],outputs=[dict(id='a',stock='a'),dict(id='b',stock='b')])
    path,result,manifest=save(model,'split')
    r=explain(result,'a',2,['--from',0]);assert r['value']==4 and r['accounting']['flow_total']==-6
    assert {f['flow']:f['contribution'] for f in r['accounting']['flows']}=={'transfer':-4,'return':-2,'loop':0}
    r=explain(result,'b',2,['--from',0]);assert r['value']==4 and r['accounting']['flow_total']==4
    # Auxiliary chains, table functions and delay outputs are visible, with explicit scope.
    for name,output,at in [('auxiliary_decay','stock',1),('lookup','material_ts',2),('delay','queue_ts',1)]:
        model=json.loads((ROOT/'models'/f'{name}.ir.json').read_text())
        # Use actual declared output IDs to cover fixture changes without guessing aliases.
        output=model['outputs'][0]['id'];at=model['time']['horizon']
        path,result,manifest=save(model,name)
        r=explain(result,output,at)
        assert r['verification']['passive_observations']=='bit-identical'
        if name=='lookup':assert any(n['kind']=='table' for n in r['dependencies']['nodes'])
    # Bass diffusion has an independent Euler recurrence and feedback dependencies.
    bass=json.loads((ROOT/'models/bass_sd.ir.json').read_text())
    path,result,manifest=save(bass,'bass')
    r=explain(result,'adopters',1)
    potential,adopted=990.,10.
    for step in range(10):
        previous=adopted;rate=(.01+.3*adopted/1000)*potential
        potential-=.1*rate;adopted+=.1*rate
    assert math.isclose(r['value'],adopted,abs_tol=1e-12)
    assert math.isclose(r['accounting']['flow_total'],adopted-previous,abs_tol=1e-12)
    assert {'potential','adopted','adoption','pressure'}<={n['id'] for n in r['dependencies']['nodes']}
    # Retained report is self-contained HTML, includes IDs and one SVG; never executable markup.
    html=tmp/'explanation.html'
    summary=json.loads(invoke(['explain',result,'--output','adopters','--at','1','--format','html','--out',html]))
    audit=HTMLAudit();audit.feed(html.read_text())
    assert audit.svg==1 and not audit.scripts and not audit.remote
    assert summary['manifest_id'] in html.read_text() and summary['model_sha256'] in html.read_text()
    original=html.read_bytes();invoke(['explain',result,'--output','adopters','--at','1','--out',html],'IR_USAGE');assert html.read_bytes()==original
    # Portable bundles work after original inputs and results are removed.
    bundle=tmp/'bundle';invoke(['bundle',manifest,'--out',bundle])
    path.unlink();result.unlink();manifest.unlink()
    r=explain(bundle,'adopters',1);assert math.isclose(r['value'],adopted,abs_tol=1e-12)
    invoke(['explain',bundle,'--output','adopters','--at','1','--out',bundle/'new.json'],'IR_USAGE');assert not (bundle/'new.json').exists()
    # Captured data lineage, source escaping, and source-file drift.
    if '--without-arrow' not in sys.argv:
        model=json.loads((ROOT/'models/data/parameter_decay.ir.json').read_text())
        for binding in model['data']:
            old=ROOT/'models/data'/binding['source']
            # A slash is not a filename character: use an entity-breaking but legal basename.
            new=tmp/'rates-<img onerror=alert(1)>.csv';shutil.copyfile(old,new);binding['source']=new.name
        path,result,manifest=save(model,'bound')
        output=model['outputs'][0]['id'];at=model['time']['horizon'];r=explain(result,output,at)
        sources=[n for n in r['dependencies']['nodes'] if n.get('data_sources')]
        assert sources and sources[0]['data_sources'][0]['file_sha256']
        page=invoke(['explain',result,'--output',output,'--at',at,'--format','html'])
        assert '<img onerror' not in page and '&lt;img onerror' in page
        original=new.read_bytes();new.write_bytes(original.replace(b'0.25',b'0.26'))
        invoke(['explain',manifest,'--output',output,'--at',at],'IR_REPLAY_MISMATCH');new.write_bytes(original)
    path,result,manifest=save(base,'corrupt')
    output='material_ts';at=1
    original=path.read_bytes();path.write_bytes(original.replace(b'decay_reference',b'changed_name'))
    invoke(['explain',manifest,'--output',output,'--at',at],'IR_REPLAY_MISMATCH');path.write_bytes(original)
    # Corrupt saved values: reject before attributing any result.
    text=result.read_text();reader=list(csv.reader(io.StringIO(text)));reader[1][4]='123'
    stream=io.StringIO();writer=csv.writer(stream);writer.writerows(reader);result.write_text(stream.getvalue())
    invoke(['explain',result,'--output',output,'--at',at],'IR_EXPLAIN_RESULT')
    # Unsupported clipping must be rejected rather than attributing requested rates as applied flows.
    model=json.loads((ROOT/'models/clipping.ir.json').read_text());path,result,manifest=save(model,'clipped')
    invoke(['explain',manifest,'--output',model['outputs'][0]['id'],'--at',model['time']['horizon']],'IR_EXPLAIN_SCOPE')
    for args in ([],['--output','x'],['--output','x','--at','nan'],['--output','x','--at','0','--threads','0'],['--output','x','--at','0','--format','png']):
        invoke(['explain',manifest,*args],'IR_USAGE')
    print(f'Explanation: {count} CLI cases; Euler/RK4 analytic accounting, Bass feedback, signed flows, identities, data, bundles, failures and HTML pass')
