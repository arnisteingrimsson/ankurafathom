"""Mutable network CLI checked against a separate edge-set/event recurrence."""
import copy,csv,io,json,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def main():
    exe=str(Path(sys.argv[1]).resolve());observations=0
    fixture=json.loads((ROOT/'models/typed_abm_network_updates.ir.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory)/'model.json'
        def call(model,mode='run',valid=True):
            path.write_text(json.dumps(model));r=subprocess.run([exe,mode,str(path)],capture_output=True,text=True)
            if not valid:
                assert r.returncode!=0,'invalid graph accepted';d=json.loads(r.stderr)['diagnostics'][0];assert d['code'].startswith('IR_') and d['pointer'].startswith('/'),d;return
            assert r.returncode==0,r.stderr
            if mode=='lint':return
            return {(float(x['time']),x['output_id']):float(x['value']) for x in csv.DictReader(io.StringIO(r.stdout))}
        for n in [3,4,5]:
            for directed in [False,True]:
                for gain in [.125,.5]:
                    m=copy.deepcopy(fixture);m['time']['dt']=.5;p=m['components'][0]
                    p['agents']=[dict(value=i+1) for i in range(n)];p['network']=dict(directed=directed,edges=[[i,i+1] for i in range(n-1)])
                    p['phases']=[dict(assign=[dict(field='value',expr=f'value+neighbor_sum*{gain}')]),dict(assign=[dict(field='value',expr='value+degree')])]
                    p['lifecycle']=[dict(time=1.5,sequence=0,retire=[1],births=[dict(value=10)])]
                    p['network_updates']=[dict(time=.75,sequence=0,add=[[0,n-1]],remove=[[0,1]]),dict(time=1.5,sequence=1,add=[[0,n]],remove=[]),dict(time=1.5,sequence=0,add=[[n,n-1]],remove=[]),dict(time=2.25,sequence=0,add=([[n-1,0]] if directed else [[0,2]] if n>3 else []),remove=[[0,n-1]])]
                    m['outputs']=[dict(id=f'value{i}',agent=i,field='value',inactive_value=-1) for i in range(n+1)]+[dict(id=f'degree{i}',agent=i,query='degree',inactive_value=-1) for i in range(n+1)]
                    actual=call(m);state={i:float(i+1) for i in range(n)}
                    canonical=lambda a,b:(a,b) if directed else tuple(sorted((a,b)))
                    edges={canonical(*edge) for edge in p['network']['edges']};expected={}
                    def neighbors(i):return sorted({b for a,b in edges if a==i}|({a for a,b in edges if b==i} if not directed else set()))
                    for time in sorted({i/2 for i in range(7)}|{.75,1.5,2.25}):
                        if time==1.5:
                            del state[1];state[n]=10.;edges={e for e in edges if 1 not in e}
                        for change in sorted([c for c in p['network_updates'] if c['time']==time],key=lambda c:c['sequence']):
                            for a,b in change['remove']:edges.remove(canonical(a,b))
                            for a,b in change['add']:
                                edge=canonical(a,b);assert edge not in edges;edges.add(edge)
                        if time and time*2==int(time*2):
                            state={i:value+gain*sum(state[j] for j in neighbors(i)) for i,value in state.items()}
                            state={i:value+len(neighbors(i)) for i,value in state.items()}
                        if time*2==int(time*2):
                            for i in range(n+1):expected[time,f'value{i}']=state.get(i,-1);expected[time,f'degree{i}']=len(neighbors(i)) if i in state else -1
                    assert actual==expected,(n,directed,gain,actual,expected);observations+=len(expected)
                    p['network_updates'].reverse();p['network']['edges'].reverse();assert call(m)==expected
                    if not directed:
                        for c in p['network_updates']:
                            c['add']=[e[::-1] for e in c['add']];c['remove']=[e[::-1] for e in c['remove']]
                        assert call(m)==expected
        # Due timers see old topology, then the edge edit, direct messages and topic handlers see new topology.
        m=json.loads((ROOT/'models/typed_abm_rates.ir.json').read_text());p=m['components'][0];p['agents'].append(copy.deepcopy(p['agents'][0]));p['network']=dict(directed=False,edges=[[0,1]])
        p['queries']=[dict(id='degree',source='network',op='count')];m['parameters']=[dict(id='duration',value=1,unit='day')]
        p['chart']['transitions']=[dict(id='tick',source=0,target=0,trigger='timeout',duration='duration',assign=[dict(field='events',expr='events+degree')]),dict(id='read',source=0,target=0,trigger='message',event='read',guard='degree',assign=[dict(field='events',expr='events+10*degree')])]
        p['network_updates']=[dict(time=1,sequence=0,add=[[0,2]],remove=[[0,1]])];p['messages']=[dict(time=1,agent=0,sequence=0,event='read')]
        p['topics']=[dict(id='read_degree',capacity=5,unit='1',assign=[dict(field='events',expr='events+100*degree')])];p['publications']=[dict(time=1,topic='read_degree',sender=0,sequence=0,value=0)]
        m['outputs']=[dict(id=f'events{i}',agent=i,field='events') for i in range(3)]
        rows=call(m);assert rows[1,'events0']==111 and rows[1,'events1']==1 and rows[1,'events2']==100
        for tick in range(11):
            t=tick/2
            expected=[0,0,0] if t<1 else [111+int(t)-1,1,100+int(t)-1]
            for i,v in enumerate(expected):assert rows[t,f'events{i}']==v;observations+=1
        dense=copy.deepcopy(m);dense['time']['dt']=.25;d=call(dense);assert all(d[k]==v for k,v in rows.items())
        # Phase-driven retirement prunes edges before the next phase; the child remains isolated.
        m=copy.deepcopy(fixture);p=m['components'][0];p.pop('lifecycle');p['network_updates']=[];p['network']['edges']=[[0,1],[1,2]]
        p['fields'].append(dict(name='replace',type='boolean',unit='1'))
        for i,a in enumerate(p['agents']):a['replace']=i==1
        p['phases']=[dict(assign=[dict(field='value',expr='degree')],lifecycle=dict(retire='replace',births=[dict(guard='replace',record=dict(value=7,replace=False))])),dict(assign=[dict(field='value',expr='value+degree*10')])]
        m['outputs']=[dict(id='total',metric='sum',field='value'),dict(id='degree0',agent=0,query='degree')]
        rows=call(m);assert rows[1,'total']==9 and rows[1,'degree0']==0
        # Behavior can invalidate a statically valid scheduled edge endpoint.
        bad=copy.deepcopy(m);bad['components'][0]['network_updates']=[dict(time=2,sequence=0,add=[[0,1]],remove=[])];call(bad,mode='lint');call(bad,valid=False)
        # Runtime edge existence/duplicate/overlap checks across the whole same-time bag.
        for edits in [
            [dict(time=0,sequence=0,add=[[0,1]],remove=[])],
            [dict(time=0,sequence=0,add=[],remove=[[0,2]])],
            [dict(time=0,sequence=0,add=[[0,2]],remove=[]),dict(time=0,sequence=1,add=[[2,0]],remove=[])],
            [dict(time=0,sequence=0,add=[[0,1]],remove=[[0,1]])],
            [dict(time=0,sequence=0,add=[],remove=[[0,1]]),dict(time=0,sequence=1,add=[],remove=[[1,0]])],
        ]:
            bad=copy.deepcopy(fixture);bad['components'][0]['network_updates']=edits;call(bad,mode='lint');call(bad,valid=False)
        # No-op edits are legal; edits at time zero precede the first observation.
        m=copy.deepcopy(fixture);p=m['components'][0];p.pop('lifecycle');p['network_updates']=[dict(time=0,sequence=0,add=[[0,2]],remove=[]),dict(time=1,sequence=0,add=[],remove=[])]
        m['outputs']=[dict(id='degree0',agent=0,query='degree')];assert call(m)[0,'degree0']==2
        print(f'mutable network recurrences: {observations} independent observations across 12 graphs plus async ordering, lifecycle, failure and replay checks passed')
if __name__=='__main__':main()
