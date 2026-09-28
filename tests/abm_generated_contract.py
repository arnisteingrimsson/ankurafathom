"""Generated publication CLI against independent phase/event recurrences."""
import copy,csv,heapq,io,json,math,subprocess,sys,tempfile
from pathlib import Path
from abm_ir_contract import word
ROOT=Path(__file__).resolve().parents[1]

def main():
    exe=str(Path(sys.argv[1]).resolve())
    sync=json.loads((ROOT/'models/typed_abm_phase_publish.ir.json').read_text())
    asynchronous=json.loads((ROOT/'models/typed_abm_transition_publish.ir.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory)/'model.json'
        def call(model,valid=True,mode='run'):
            path.write_text(json.dumps(model));r=subprocess.run([exe,mode,str(path)],capture_output=True,text=True)
            if not valid:
                assert r.returncode!=0,'invalid emission accepted';d=json.loads(r.stderr)['diagnostics'][0];assert d['code'].startswith('IR_') and d['pointer'].startswith('/'),d;return d
            assert r.returncode==0,r.stderr
            if mode=='lint':return
            return {(float(x['time']),x['output_id']):float(x['value']) for x in csv.DictReader(io.StringIO(r.stdout))}
        observations=0
        for case in range(12):
            m=copy.deepcopy(sync);p=m['components'][0];count=1+case%4;gain=case%3+1;factor=1+case%2
            p['agents']=[dict(value=i+1) for i in range(count)]
            p['topics'][0]['capacity']=100
            p['phases'][0]['assign'][0]['expr']=f'value+{gain}'
            p['phases'][1]['assign'][0]['expr']=f'value*{factor}'
            target='broadcast' if case%2 else 'self'
            for phase in p['phases']:phase['publish'][0]['receiver']=target
            m['outputs']=[dict(id=f'agent{i}',agent=i,field='value') for i in range(count)]
            actual=call(m);state=list(range(1,count+1));expected={}
            for tick in range(4):
                for i,v in enumerate(state):expected[tick,f'agent{i}']=v
                first=list(state);second=[v+gain for v in state];state=[v*factor for v in second]
                if target=='broadcast':state=[v+sum(first)+sum(second) for v in state]
                else:state=[v+a+b for v,a,b in zip(state,first,second)]
            assert actual==expected,(case,actual,expected);observations+=len(expected)
        # Exact timeout/message calendar, including external topic input with sequence zero.
        m=copy.deepcopy(asynchronous);p=m['components'][0]
        p['publications']=[dict(time=.75,topic='credit',sender=0,receiver=0,sequence=0,value=100)]
        rows=call(m);state=[0,0];count=[0,0];credit=[0,0];due=[];expected={};ordinal=0
        for tick in range(10):
            time=tick/4;pending=[]
            while due and due[0][0]==time:
                _,agent,_=heapq.heappop(due);pending.append((agent,count[agent]));count[agent]+=1;state[agent]=0
            for msg in sorted([v for v in p['messages'] if v['time']==time],key=lambda v:(v['agent'],v['sequence'])):
                i=msg['agent']
                if state[i]==0:
                    pending.append((i,count[i]+1));count[i]+=1;state[i]=1
                    heapq.heappush(due,(time+.5,i,ordinal));ordinal+=1
            if time==.75:credit[0]+=100
            for i,value in pending:credit[i]+=value
            for i in range(2):
                for name,values in [('state',state),('count',count),('credit',credit)]:expected[time,f'{name}{i}']=values[i]
        assert rows==expected;observations+=len(expected)
        p['messages'].reverse();p['chart']['transitions'].reverse();assert call(m)==expected
        dense=copy.deepcopy(m);dense['time']['dt']=.125;dr=call(dense);assert all(dr[k]==v for k,v in expected.items())
        # A disabled guard and a losing transition do not emit or evaluate their invalid expressions.
        m=copy.deepcopy(asynchronous);p=m['components'][0];loser=copy.deepcopy(p['chart']['transitions'][0]);loser.update(id='loser',priority=10);loser['publish'][0]['value']='1/0';p['chart']['transitions'].append(loser)
        assert call(m)==call(asynchronous)
        p['chart']['transitions'][0]['guard']='0';p['chart']['transitions'][2]['guard']='0';rows=call(m)
        assert all(v==0 for v in rows.values()),'guarded transition emitted'
        # Failed publication evaluation/capacity are runtime errors after successful lint.
        for fixture,select in [(sync,lambda p:p['phases'][0]),(asynchronous,lambda p:p['chart']['transitions'][0])]:
            m=copy.deepcopy(fixture);select(m['components'][0])['publish'][0]['value']='1/0';call(m,mode='lint');call(m,valid=False)
        m=copy.deepcopy(sync);m['components'][0]['topics'][0]['capacity']=3;call(m,mode='lint');call(m,valid=False)
        # Stale timeout after message re-entry produces no extra publication.
        m=copy.deepcopy(asynchronous);p=m['components'][0];refresh=copy.deepcopy(p['chart']['transitions'][0]);refresh.update(id='refresh',source=1)
        p['chart']['transitions'].append(refresh);p['messages']=[dict(time=0,agent=0,sequence=0,event='kick'),dict(time=.25,agent=0,sequence=0,event='kick')]
        rows=call(m);assert rows[.5,'count0']==2 and rows[.5,'credit0']==3 and rows[.75,'count0']==3 and rows[.75,'credit0']==5
        # Payload dimensions are checked even when the topic handler is empty.
        m=copy.deepcopy(sync);m['components'][0]['topics'][0].update(unit='day',assign=[]);call(m,mode='lint',valid=False)
        # Rate-driven transitions use an independent Philox/renewal calendar.
        rate=json.loads((ROOT/'models/typed_abm_rates.ir.json').read_text());p=rate['components'][0]
        p['fields'].append(dict(name='credit',type='integer',unit='1'))
        for agent in p['agents']:agent['credit']=0
        p['topics']=[dict(id='credit',capacity=20,unit='1',assign=[dict(field='credit',expr='credit+message_value')])]
        p['chart']['transitions'][0]['publish']=[dict(topic='credit',receiver='self',value='events+1')]
        rate['outputs']=[dict(id=f'credit{i}',agent=i,field='credit') for i in range(2)]
        actual=call(rate);hazard=rate['parameters'][0]['value'];horizon=rate['time']['horizon'];dt=rate['time']['dt']
        for agent in range(2):
            events=[];time=0;generation=0
            while True:
                u=(word(0,0,0,agent,generation,71)+.5)/2**32;time+=-math.log(u)/hazard
                if time>horizon:break
                events.append(time);generation+=1
            for k in range(round(horizon/dt)+1):
                t=k*dt;n=sum(e<=t for e in events);assert actual[t,f'credit{agent}']==n*(n+1)//2,(t,agent,actual)
                observations+=1
        experiment=dict(seed=7319,replications=3,scenarios=[dict(id=0,parameters={}),dict(id=4,parameters=dict(hazard=0)),dict(id=9,parameters=dict(hazard=2.5))])
        experiment_path=Path(directory)/'experiment.json';experiment_path.write_text(json.dumps(experiment));path.write_text(json.dumps(rate))
        command=[exe,'run',str(path),'--experiment',str(experiment_path)]
        run=subprocess.run(command,capture_output=True,text=True);assert run.returncode==0,run.stderr
        assert subprocess.run(command,capture_output=True,text=True).stdout==run.stdout,'publication experiment replay differs'
        actual={(int(r['scenario']),int(r['replication']),float(r['time']),r['output_id']):float(r['value']) for r in csv.DictReader(io.StringIO(run.stdout))}
        for scenario,hazard in [(0,.7),(4,0),(9,2.5)]:
            for replication in range(3):
                for agent in range(2):
                    events=[];time=0;generation=0
                    while hazard:
                        time+=-math.log((word(7319,scenario,replication,agent,generation,71)+.5)/2**32)/hazard
                        if time>horizon:break
                        events.append(time);generation+=1
                    for k in range(round(horizon/dt)+1):
                        t=k*dt;n=sum(e<=t for e in events)
                        assert actual[scenario,replication,t,f'credit{agent}']==n*(n+1)//2
                        observations+=1
        print(f'generated publication recurrences: {observations} independent observations plus guard/stale/error checks passed')
if __name__=='__main__':main()
