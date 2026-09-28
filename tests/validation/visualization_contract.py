"""Independent graph topology, escaping, provenance and publication checks."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

exe=Path(sys.argv[1]).resolve();root=Path(__file__).resolve().parents[2];count=0
try:
    from jsonschema import Draft202012Validator
    schema=Draft202012Validator(json.loads((root/'ir/schema/visualization_report.schema.json').read_text()))
except ImportError: schema=None
with tempfile.TemporaryDirectory() as directory:
    tmp=Path(directory)
    def invoke(args,code=None,env=None):
        global count
        p=subprocess.run([str(exe),*map(str,args)],capture_output=True,text=True,env=env);count+=1
        if code:
            assert p.returncode==1,(args,p.stdout,p.stderr)
            assert json.loads(p.stderr)['diagnostics'][0]['code']==code,(args,p.stderr)
            return
        assert p.returncode==0,(args,p.stderr)
        return json.loads(p.stdout) if p.stdout.strip() else None
    def write(doc,name):
        path=tmp/(name+'.json');path.write_text(json.dumps(doc));return path
    def viz(path,name,*extra):
        target=tmp/name
        result=invoke(['viz',path,'--out',target,*extra])
        assert result==json.loads((target/'graph.json').read_text())
        if schema:schema.validate(result)
        assert sorted(p.name for p in target.iterdir())==['dependencies.dot','dependencies.mmd','dependencies.svg','graph.json','index.html','stock-flow.dot','stock-flow.mmd','stock-flow.svg']
        for view in ('stock-flow','dependencies'):
            tree=ET.parse(target/(view+'.svg'));text=''.join(tree.getroot().itertext())
            assert result['model_sha256'] in text and 'Dependencies are not causal effects' in text
            assert all(n.tag.split('}')[-1] not in ('script','foreignObject') for n in tree.iter())
            assert not any(k.endswith('href') for n in tree.iter() for k in n.attrib)
            assert result['model_sha256'] in (target/(view+'.mmd')).read_text()
        return result
    def edges(graph):return {(e['from'],e['to'],e['role']) for e in graph['edges']}
    bass=json.loads((root/'models/bass_sd.ir.json').read_text());path=write(bass,'bass')
    r=viz(path,'bass-all');assert r['manifest_id'] is None and r['verification']['results']=='not-verified'
    assert {n['id'] for n in r['stock_flow']['nodes']}=={'symbol:potential','symbol:adopted','symbol:adoption'}
    assert edges(r['stock_flow'])=={('symbol:potential','symbol:adoption','material-source'),('symbol:adoption','symbol:adopted','material-destination')}
    expected={('symbol:innovation','symbol:pressure','expression-symbol'),('symbol:imitation','symbol:pressure','expression-symbol'),
        ('symbol:adopted','symbol:pressure','expression-symbol'),('symbol:market','symbol:pressure','expression-symbol'),
        ('symbol:pressure','symbol:adoption','expression-symbol'),('symbol:potential','symbol:adoption','expression-symbol'),
        ('symbol:adoption','symbol:potential','stock-outflow'),('symbol:adoption','symbol:adopted','stock-inflow'),
        ('symbol:adopted','output:adopters','observes'),('symbol:potential','output:remaining','observes')}
    assert edges(r['dependencies'])==expected
    focused=viz(path,'bass-focus','--output','adopters')
    assert edges(focused['dependencies'])==expected-{('symbol:potential','output:remaining','observes')}
    assert 'symbol:t' not in {n['id'] for n in focused['dependencies']['nodes']}
    reordered=copy.deepcopy(bass);reordered['components'].reverse();reordered['outputs'].reverse()
    rr=viz(write(reordered,'reordered'),'reordered-view')
    assert r['dependencies']==rr['dependencies'] and r['stock_flow']==rr['stock_flow']
    result=tmp/'result.csv';invoke(['run',path,'--out',result])
    manifest=Path(str(result)+'.manifest.json');m=json.loads(manifest.read_text())
    linked=viz(path,'linked','--manifest',manifest)
    assert linked['manifest_id']==m['id'] and linked['verification']['inputs']=='exact-manifest-match'
    # Structure linkage deliberately does not claim numeric result verification.
    result.write_text('corrupted result')
    viz(path,'linked-without-results','--manifest',manifest)
    changed=copy.deepcopy(bass);changed['parameters'][0]['value']=.02;path.write_text(json.dumps(changed))
    invoke(['viz',path,'--out',tmp/'drift','--manifest',manifest],'IR_REPLAY_MISMATCH');assert not (tmp/'drift').exists()
    path.write_text(json.dumps(bass))
    for modelname in ('lookup','delay','auxiliary_decay'):
        d=viz(root/'models'/f'{modelname}.ir.json',modelname)
        e=edges(d['dependencies'])
        if modelname=='lookup':assert ('symbol:rate_table','symbol:loss','function') in e and ('symbol:t','symbol:loss','expression-symbol') in e
        if modelname=='delay':assert ('symbol:intake','symbol:queue','delay-input') in e and ('symbol:queue','symbol:completion','expression-symbol') in e
    delay=json.loads((root/'models/delay.ir.json').read_text())
    delay['parameters'].append(dict(id='duration',value=2,unit='day'))
    delay['components'][1]['duration']='duration'
    dynamic=viz(write(delay,'dynamic-delay'),'dynamic-delay-view')
    assert ('symbol:duration','symbol:queue','delay-duration') in edges(dynamic['dependencies'])
    schedule=copy.deepcopy(bass);schedule['parameters'].append(dict(id='launch',value=.2,unit='year'))
    schedule['components'][3]['expr']='STEP(pressure*potential, launch)'
    scheduled=viz(write(schedule,'schedule'),'schedule-view','--output','adopters')
    assert {('symbol:t','symbol:adoption','expression-symbol'),('symbol:dt','symbol:adoption','expression-symbol')}<=edges(scheduled['dependencies'])
    # Names and labels must never become executable DOT/HTML/mermaid syntax.
    escaped=copy.deepcopy(bass);escaped['name']='</title><script>alert("x")</script> \\n \\" [URL="https://evil.invalid"]';escaped['outputs'][0]['id']='adopted';escaped['checks'][1]['output']='adopted'
    escaped_view=viz(write(escaped,'escaping'),'escaping-view','--output','adopted')
    assert {'symbol:adopted','output:adopted'}<={n['id'] for n in escaped_view['dependencies']['nodes']}
    page=(tmp/'escaping-view/index.html').read_text();assert '<script>' not in page and '&lt;script&gt;' in page
    unicode=copy.deepcopy(bass);unicode['name']='Árni 日本語'
    viz(write(unicode,'unicode'),'unicode-view')
    assert 'Árni 日本語' in (tmp/'unicode-view/stock-flow.mmd').read_text()
    # Signed boundaries/self-loops retain endpoint semantics, independent of rate sign.
    split=dict(ir_version='0.1',name='split',time=dict(unit='day',dt=1,horizon=1),parameters=[dict(id='rate',value=2,unit='kg/day')],
        components=[dict(id='a',kind='stock',init=10,unit='kg'),dict(id='loop',kind='flow',source='a',destination='a',expr='rate',unit='kg/day'),
        dict(id='signed',kind='flow',source=None,destination='a',expr='-rate',unit='kg/day',non_negative=False)],outputs=[dict(id='a',stock='a')])
    splitview=viz(write(split,'split'),'split-view')
    assert edges(splitview['stock_flow'])=={('symbol:a','symbol:loop','material-source'),('symbol:loop','symbol:a','material-destination'),
        ('boundary:signed:source','symbol:signed','material-source'),('symbol:signed','symbol:a','material-destination')}
    assert {('symbol:loop','symbol:a','stock-inflow'),('symbol:loop','symbol:a','stock-outflow')}<=edges(splitview['dependencies'])
    # Diagrams do not execute the model or reject unsupported explanation clipping.
    broken=json.loads((root/'models/decay.ir.json').read_text());broken['parameters'][0]['value']=1000
    broken['components'][0]['clip_outflows']=True;broken['components'][0]['outflow_order']=['loss'];broken['integrator']='euler'
    viz(write(broken,'clipping'),'clipping-view')
    del broken['components'][0]['clip_outflows'];del broken['components'][0]['outflow_order']
    viz(write(broken,'runtime-negative-stock'),'runtime-negative-stock-view')
    for args,code in [([], 'IR_USAGE'),(['--out'], 'IR_USAGE'),(['--out',tmp/'x','--bogus','x'],'IR_USAGE'),
        (['--out',tmp/'x','--out',tmp/'y'],'IR_USAGE'),(['--out',tmp/'x','--output','unknown'],'IR_VIZ_OUTPUT')]:
        invoke(['viz',path,*args],code)
    invoke(['viz',root/'models/abm_adoption.ir.json','--out',tmp/'non-sd'],'IR_VIZ_SCOPE')
    sentinel=tmp/'occupied';sentinel.mkdir();(sentinel/'keep').write_text('keep')
    for dest in (sentinel,tmp/'bass-all',path):invoke(['viz',path,'--out',dest],'IR_VIZ_DESTINATION')
    link=tmp/'symlink';link.symlink_to(tmp/'absent',target_is_directory=True)
    invoke(['viz',path,'--out',link],'IR_VIZ_DESTINATION');assert link.is_symlink()
    env=os.environ.copy();env['PATH']=str(tmp/'no-dot')
    invoke(['viz',path,'--out',tmp/'missing-renderer'],'IR_VIZ_RENDER',env)
    assert not (tmp/'missing-renderer').exists() and not list(tmp.glob('.fathom-viz-*'))
    failing=tmp/'failing-renderer';failing.mkdir();(failing/'dot').symlink_to('/usr/bin/false')
    env['PATH']=str(failing)
    invoke(['viz',path,'--out',tmp/'failed-renderer'],'IR_VIZ_RENDER',env)
    assert not (tmp/'failed-renderer').exists() and not list(tmp.glob('.fathom-viz-*'))
    assert (sentinel/'keep').read_text()=='keep' 
print(json.dumps(dict(cli_cases=count,graphviz='rendered SVG checked as XML',schema=schema is not None)))
