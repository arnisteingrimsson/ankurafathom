"""Scheduled lifecycles against independent recurrence and renewal calendars."""
import copy,csv,io,json,math,subprocess,sys,tempfile
from pathlib import Path
from abm_ir_contract import word
ROOT=Path(__file__).resolve().parents[1]

def main():
    exe=str(Path(sys.argv[1]).resolve());observations=0
    fixture=json.loads((ROOT/'models/typed_abm_lifecycle.ir.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory)/'model.json'
        def call(model,mode='run',valid=True,experiment=None):
            path.write_text(json.dumps(model));args=[exe,mode,str(path)]
            if experiment:
                ep=Path(directory)/'experiment.json';ep.write_text(json.dumps(experiment));args+=['--experiment',str(ep)]
            r=subprocess.run(args,capture_output=True,text=True)
            if not valid:
                assert r.returncode!=0,'invalid lifecycle accepted'
                d=json.loads(r.stderr)['diagnostics'][0];assert d['code'].startswith('IR_') and d['pointer'].startswith('/'),d
                return d
            assert r.returncode==0,r.stderr
            if mode=='lint':return
            rows=list(csv.DictReader(io.StringIO(r.stdout)))
            if experiment:return {(int(x['scenario']),int(x['replication']),float(x['time']),x['output_id']):float(x['value']) for x in rows}
            return {(float(x['time']),x['output_id']):float(x['value']) for x in rows}
        # Independently computed full output trajectories, including initial-time and horizon births.
        for birth_time in [0,.25,.5,1,1.75,2]:
            m=copy.deepcopy(fixture);p=m['components'][0]
            for change in p['lifecycle']:change['time']=birth_time
            actual=call(m);state={0:1,1:3};expected={};replaced=False
            for tick in range(5):
                t=tick/2
                if not replaced and birth_time<=t:
                    del state[0];state[2]=10;replaced=True
                if tick:state={i:2*(v+2) for i,v in state.items()}
                expected.update({(t,'active'):2,(t,'total'):sum(state.values()),(t,'old_alive'):int(0 in state),(t,'new_alive'):int(2 in state),(t,'new_work'):state.get(2,-1)})
            assert actual==expected,(birth_time,actual,expected);observations+=len(expected)
            p['lifecycle'].reverse();assert call(m)==expected
        # Chronological IDs despite declaration order; empty initial population.
        m=copy.deepcopy(fixture);p=m['components'][0];p['agents']=[]
        p['lifecycle']=[dict(time=1,sequence=5,retire=[],births=[dict(work=4,ready=True,role='later')]),dict(time=0,sequence=3,retire=[],births=[dict(work=2,ready=False,role='first')])]
        m['outputs']=[dict(id='first',agent=0,field='work'),dict(id='later',agent=1,field='work',inactive_value=-1)]
        rows=call(m);assert rows[0,'first']==2 and rows[0,'later']==-1 and rows[1,'later']==12
        # Spatial replacement and live graph/population query membership.
        m=copy.deepcopy(fixture);p=m['components'][0]
        p['fields']=[dict(name=n,type='integer',unit='1') for n in ['x','y','work']]
        p['agents']=[dict(x=0,y=0,work=1),dict(x=1,y=0,work=2)]
        p['phases']=[dict(assign=[dict(field='work',expr='work+near+edges+others')])]
        p['space']=dict(kind='grid',x='x',y='y',width=3,height=1,wrap=False)
        p['network']=dict(directed=False,edges=[[0,1]])
        p['queries']=[dict(id='near',source='space',op='count',unit='1',radius=1),dict(id='edges',source='network',op='count'),dict(id='others',source='population',op='count')]
        p['lifecycle'][1]['births']=[dict(x=0,y=0,work=10)]
        m['outputs']=[dict(id='new',agent=2,field='work',inactive_value=-1),dict(id='edge',agent=1,query='edges'),dict(id='newedge',agent=2,query='edges',inactive_value=-1),dict(id='old',agent=0,query='near',inactive_value=-1)]
        rows=call(m);assert rows[.5,'edge']==1 and rows[1,'edge']==0 and rows[1,'newedge']==0 and rows[1,'old']==-1 and rows[1,'new']==12
        bad=copy.deepcopy(m);bad['components'][0]['lifecycle'][0]['retire']=[];call(bad,mode='lint');call(bad,valid=False)
        bad=copy.deepcopy(fixture);bad['outputs'][-1].pop('inactive_value');call(bad,mode='lint');call(bad,valid=False)
        # Async newborn gets a fresh chart epoch and timers at birth; messages can arrive at birth.
        m=json.loads((ROOT/'models/typed_abm_rates.ir.json').read_text());p=m['components'][0];p['agents']=p['agents'][:1]
        birth=copy.deepcopy(p['agents'][0]);p['lifecycle']=[dict(time=1,sequence=0,retire=[0],births=[birth])]
        p['chart']['transitions']=[dict(id='timeout',source=0,target=0,trigger='timeout',duration='duration',assign=[dict(field='events',expr='events+1')]),dict(id='kick',source=0,target=0,trigger='message',event='kick',assign=[dict(field='events',expr='events+10')])]
        m['parameters']=[dict(id='duration',value=.5,unit='day')];p['messages']=[dict(time=1,agent=1,sequence=0,event='kick')]
        m['outputs']=[dict(id='events',agent=1,field='events',inactive_value=-1),dict(id='generation',agent=1,field='generation',inactive_value=-1),dict(id='entered',agent=1,field='entered',inactive_value=-1)]
        rows=call(m)
        for tick in range(11):
            t=tick/2;assert rows[t,'events']==(-1 if t<1 else 10+tick-2)
            assert rows[t,'generation']==(-1 if t<1 else 1+tick-2)
            assert rows[t,'entered']==(-1 if t<1 else t)
            observations+=3
        for time,agent in [(.5,1),(1,0)]:
            bad=copy.deepcopy(m);bad['components'][0]['messages'][0].update(time=time,agent=agent);call(bad,mode='lint',valid=False)
        # Lifecycle precedes explicit topic delivery; newborns are broadcast recipients.
        tmodel=copy.deepcopy(fixture);p=tmodel['components'][0];p['phases']=[]
        p['topics']=[dict(id='add',capacity=4,unit='1',assign=[dict(field='work',expr='work+message_value')])]
        p['publications']=[dict(time=1,topic='add',sender=2,sequence=0,value=3)]
        rows=call(tmodel);assert rows[1,'total']==19 and rows[1,'new_work']==13
        bad=copy.deepcopy(tmodel);bad['components'][0]['publications'][0]['sender']=0;call(bad,mode='lint',valid=False)
        # Addressed newborn rates across seed/scenario/replication contexts, with death censoring.
        rate=json.loads((ROOT/'models/typed_abm_rates.ir.json').read_text());p=rate['components'][0];birth=copy.deepcopy(p['agents'][0]);p['agents']=p['agents'][:1]
        p['lifecycle']=[dict(time=1.25,sequence=0,retire=[0],births=[birth]),dict(time=4,sequence=0,retire=[1],births=[birth])]
        rate['outputs']=[dict(id=f'events{i}',agent=i,field='events',inactive_value=-1) for i in range(3)]
        experiment=dict(seed=7319,replications=3,scenarios=[dict(id=0,parameters={}),dict(id=4,parameters=dict(hazard=0)),dict(id=9,parameters=dict(hazard=2.5))])
        actual=call(rate,experiment=experiment);assert call(rate,experiment=experiment)==actual
        for scenario,hazard in [(0,.7),(4,0),(9,2.5)]:
            for replication in range(3):
                for agent,(start,stop) in enumerate([(0,1.25),(1.25,4),(4,math.inf)]):
                    events=[];time=start;generation=0
                    while hazard:
                        time+=-math.log((word(7319,scenario,replication,agent,generation,71)+.5)/2**32)/hazard
                        if time>5 or time>=stop:break
                        events.append(time);generation+=1
                    for tick in range(11):
                        t=tick/2;expected=-1 if not start<=t<stop else sum(e<=t for e in events)
                        assert actual[scenario,replication,t,f'events{agent}']==expected,(scenario,replication,agent,t)
                        observations+=1
        dense=copy.deepcopy(rate);dense['time']['dt']=.25;d=call(dense,experiment=experiment);assert all(d[k]==v for k,v in actual.items())
        print(f'lifecycle calendars: {observations} independent observations plus liveness, spatial, message, replay and failure checks passed')
if __name__=='__main__':main()
