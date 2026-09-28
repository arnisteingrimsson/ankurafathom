"""Hand-derived dynamic query behavior and failure diagnostics beyond Mesa cases."""
import copy,csv,io,json,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def main():
    exe=str(Path(sys.argv[1]).resolve())
    fixture=json.loads((ROOT/'models/typed_abm_neighbors.ir.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory)/'model.json'
        def call(m,mode='run',valid=True):
            path.write_text(json.dumps(m));r=subprocess.run([exe,mode,str(path)],capture_output=True,text=True)
            if not valid:
                assert r.returncode!=0,'invalid model accepted'
                d=json.loads(r.stderr)['diagnostics'][0];assert d['code'].startswith('IR_') and d['pointer'].startswith('/'),d
                return d
            assert r.returncode==0,r.stderr
            if mode=='lint':return
            return {(float(x['time']),x['output_id']):float(x['value']) for x in csv.DictReader(io.StringIO(r.stdout))}
        rows=call(fixture)
        assert rows[0,'first_near']==5 and rows[0,'first_links']==10 and rows[0,'first_same']==1
        assert rows[1,'first_value']==3 and rows[4,'first_x']==4
        assert all(rows[t,'count']==4 and rows[t,'last_links']==0 for t in range(5))
        perm=copy.deepcopy(fixture);p=perm['components'][0];p['network']['edges'].reverse();p['queries'].reverse();p['fields'].reverse()
        assert call(perm)==rows,'declaration order changed trajectory'
        # A collision must fail even if no query reads spatial state after the phase.
        for expr in ['0','x+0.5']:
            m=copy.deepcopy(fixture);p=m['components'][0];p['queries']=[];p['phases']=[{'assign':[{'field':'x','expr':expr}]}];m['outputs']=[{'id':'x','agent':0,'field':'x'}]
            call(m,'lint');assert call(m,valid=False)['code']=='IR_ABM_RUNTIME'
        m=copy.deepcopy(fixture);m['components'][0]['space']['wrap']=False
        call(m,'lint');call(m,valid=False)
        # Query units flow into expressions; continuous units may differ from simulation time.
        m=copy.deepcopy(fixture);p=m['components'][0]
        for f in p['fields']:
            if f['name'] in ('x','y'):f.update(type='real',unit='meter')
        p['space']=dict(kind='continuous',fields=['x','y'],lower=[0,0],upper=[4,3],bin_width=.5,unit='meter',wrap=True)
        for q in p['queries']:
            if q['source']=='space':q['unit']='meter';q.pop('moore',None)
        p['phases']=[]
        call(m,'lint');result=call(m);assert result[0,'first_near']==5
        # Neighbor sum overflow is detected even when every record is individually exact.
        m=copy.deepcopy(fixture);p=m['components'][0];p['queries']=[dict(id='huge',source='population',op='sum',field='x',include_self=True)];p.pop('space');p['phases']=[]
        for a in p['agents']: a['x']=9007199254740991
        m['outputs']=[dict(id='total',agent=0,query='huge')];call(m,'lint');call(m,valid=False)
        # Same-time asynchronous commands read the preceding command's staged neighbor state.
        m=json.loads((ROOT/'models/typed_abm_async.ir.json').read_text());p=m['components'][0]
        m['time'].update(dt=.5,horizon=2);p['network']=dict(directed=True,edges=[[0,1],[1,0]])
        p['queries']=[dict(id='neighbor_work',source='network',op='sum',field='completed')]
        p['chart']['transitions']=[dict(id='visit',source=0,target=0,trigger='message',event='visit',guard='neighbor_work',assign=[dict(field='completed',expr='neighbor_work+1')])]
        p['chart']['initial']=0
        for i,a in enumerate(p['agents']):a['completed']=i+1
        p['messages']=[dict(time=1,agent=i,sequence=0,event='visit') for i in [1,0]]
        m['outputs']=[dict(id=f'work{i}',agent=i,field='completed') for i in [0,1]]+[dict(id='neighbor',agent=0,query='neighbor_work')]
        rows=call(m)
        assert rows[0,'work0']==1 and rows[0,'work1']==2
        assert rows[1,'work0']==3 and rows[1,'work1']==4 and rows[1,'neighbor']==4
        dense=copy.deepcopy(m);dense['time']['dt']=.25;dr=call(dense)
        assert all(dr[k]==v for k,v in rows.items())
        p['agents'][1]['completed']=0;rows=call(m)
        assert rows[1,'work0']==1 and rows[1,'work1']==2,'guard did not use neighbor state'
    print('neighborhood hand trajectories, async order, units, permutations and runtime failures passed')
if __name__=='__main__':main()
