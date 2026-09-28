"""Behavior lifecycle CLI versus independent phase and event calendars."""
import copy,csv,heapq,io,json,math,subprocess,sys,tempfile
from pathlib import Path
from abm_ir_contract import word
ROOT=Path(__file__).resolve().parents[1]

def main():
    exe=str(Path(sys.argv[1]).resolve());observations=0
    fixture=json.loads((ROOT/'models/typed_abm_behavior.ir.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory)/'model.json'
        def call(model,mode='run',valid=True,experiment=None):
            path.write_text(json.dumps(model));args=[exe,mode,str(path)]
            if experiment:
                ep=Path(directory)/'experiment.json';ep.write_text(json.dumps(experiment));args+=['--experiment',str(ep)]
            r=subprocess.run(args,capture_output=True,text=True)
            if not valid:
                assert r.returncode!=0,'invalid behavior accepted'
                d=json.loads(r.stderr)['diagnostics'][0];assert d['code'].startswith('IR_') and d['pointer'].startswith('/'),d
                return d
            assert r.returncode==0,r.stderr
            if mode=='lint':return
            rows=list(csv.DictReader(io.StringIO(r.stdout)))
            if experiment:return {(int(x['scenario']),int(x['replication']),float(x['time']),x['output_id']):float(x['value']) for x in rows}
            return {(float(x['time']),x['output_id']):float(x['value']) for x in rows}
        # One phase snapshot, deterministic parent/list ID ordering, next-phase newborn updates.
        for count in [1,2,3]:
            for retire in [False,True]:
                for multiplicity in [1,2]:
                    m=copy.deepcopy(fixture);p=m['components'][0];p['agent_limit']=1000
                    p['agents']=[dict(age=i%2,lineage=i*10,work=i+1) for i in range(count)]
                    life=p['phases'][0]['lifecycle'];life['retire']='age' if retire else '0';life['births']*=multiplicity
                    actual=call(m);state={i:dict(a) for i,a in enumerate(p['agents'])};next_id=count;expected={}
                    for tick in range(5):
                        expected.update({(tick,'active'):len(state),(tick,'total'):sum(a['work'] for a in state.values()),(tick,'lineages'):sum(a['lineage'] for a in state.values()),(tick,'initial_alive'):int(0 in state)})
                        survivors={};children=[]
                        for i,a in sorted(state.items()):
                            if a['age']:
                                children.extend([dict(age=0,lineage=a['lineage']+1,work=a['work']*2) for _ in range(multiplicity)])
                            if not (retire and a['age']):survivors[i]=dict(age=a['age']+1,lineage=a['lineage'],work=a['work']+1)
                        for a in children:survivors[next_id]=a;next_id+=1
                        state={i:dict(a,work=a['work']+10) for i,a in survivors.items()}
                    assert actual==expected,(count,retire,multiplicity,actual,expected);observations+=len(expected)
        # Guarded births do not evaluate invalid assignments; retirement suppresses parent actions/publications.
        m=copy.deepcopy(fixture);p=m['components'][0];life=p['phases'][0]['lifecycle'];life['retire']='1';life['births'][0]['guard']='0';life['births'][0]['assign'][0]['expr']='1/0'
        p['phases'][0]['assign'][0]['expr']='1/0';p['topics']=[dict(id='unused',unit='1',capacity=1)]
        p['phases'][0]['publish']=[dict(topic='unused',receiver='self',value='1/0')]
        rows=call(m);assert rows[1,'active']==0 and rows[4,'total']==0
        # Literal string/boolean templates and exact parent numeric assignments.
        m=copy.deepcopy(fixture);p=m['components'][0];p['fields'] += [dict(name='role',type='string',unit='1'),dict(name='ready',type='boolean',unit='1')]
        for a in p['agents']:a.update(role='parent',ready=True)
        b=p['phases'][0]['lifecycle']['births'][0];b['record'].update(role='child',ready=False);b['assign'].append(dict(field='ready',expr='1-ready'))
        call(m)
        # A phase can free/reoccupy grid cells, with new graph vertices remaining isolated.
        m=copy.deepcopy(fixture);p=m['components'][0]
        p['fields'] += [dict(name=n,type='integer',unit='1') for n in ['x','y']]
        for i,a in enumerate(p['agents']):a.update(x=i,y=0)
        b=p['phases'][0]['lifecycle']['births'][0];b['record'].update(x=0,y=0);b['assign'] += [dict(field='x',expr='x')]
        p['space']=dict(kind='grid',x='x',y='y',width=4,height=1);p['network']=dict(edges=[[0,1]],directed=False)
        p['queries']=[dict(id='peers',source='network',op='count')];p['phases'][1]['assign'][0]['expr']='work+10+peers'
        rows=call(m);assert rows[1,'active']==2
        bad=copy.deepcopy(m);bad['components'][0]['phases'][0]['lifecycle']['retire']='0';call(bad,mode='lint');call(bad,valid=False)
        # A surviving parent's broadcast is delivered after birth, so it reaches the newborn too.
        m=copy.deepcopy(fixture);m['time']['horizon']=1;p=m['components'][0];p['agents']=[dict(age=1,lineage=0,work=2)]
        p['phases']=p['phases'][:1];p['phases'][0]['lifecycle']['retire']='0'
        p['topics']=[dict(id='credit',unit='1',capacity=10,assign=[dict(field='work',expr='work+message_value')])]
        p['phases'][0]['publish']=[dict(topic='credit',receiver='broadcast',value='work')]
        rows=call(m);assert rows[1,'total']==11 and rows[1,'active']==2
        # A later phase retiring a queued sender rolls back instead of dropping its publication.
        bad=copy.deepcopy(m);bad['components'][0]['phases'].append(dict(assign=[],lifecycle=dict(retire='1')));call(bad,mode='lint');call(bad,valid=False)
        # Limit counts all allocated IDs, including retired ones, not just live agents.
        bad=copy.deepcopy(fixture);bad['components'][0]['agent_limit']=3;call(bad,mode='lint');call(bad,valid=False)
        bad=copy.deepcopy(fixture);bad['components'][0]['lifecycle']=[dict(time=1,sequence=0,retire=[],births=[copy.deepcopy(bad['components'][0]['agents'][0])])];call(bad,mode='lint',valid=False)
        # Due timeout replacement precedes same-time direct commands; dead references fail at runtime.
        rate=json.loads((ROOT/'models/typed_abm_rates.ir.json').read_text());p=rate['components'][0];template=copy.deepcopy(p['agents'][0]);p['agent_limit']=1000
        transition=p['chart']['transitions'][0];transition.pop('assign');transition['lifecycle']=dict(retire='1',births=[dict(record=template,assign=[dict(field='events',expr='events+1')])])
        rate['outputs']=[dict(id='total',metric='sum',field='events'),dict(id='active',metric='active'),dict(id='alive0',agent=0,metric='alive'),dict(id='alive1',agent=1,metric='alive')]
        timeout=copy.deepcopy(rate);tp=timeout['components'][0];t=tp['chart']['transitions'][0];t.update(trigger='timeout',duration='duration');t.pop('rate');t.pop('stream');timeout['parameters']=[dict(id='duration',value=.5,unit='day')]
        rows=call(timeout)
        for tick in range(11):
            assert rows[tick/2,'total']==2*tick and rows[tick/2,'active']==2
            assert rows[tick/2,'alive0']==int(tick==0);observations+=3
        # Selected message transition also initializes children; losing/guarded transitions do not evaluate lifecycle.
        message=copy.deepcopy(timeout);p=message['components'][0];p['chart']['transitions'][0].update(trigger='message',event='replace');p['chart']['transitions'][0].pop('duration');p['messages']=[dict(time=.5,agent=0,sequence=0,event='replace')]
        loser=copy.deepcopy(p['chart']['transitions'][0]);loser.update(id='loser',priority=10);loser['lifecycle']['births'][0]['assign'][0]['expr']='1/0';p['chart']['transitions'].append(loser)
        rows=call(message);assert rows[.5,'total']==1 and rows[.5,'alive0']==0 and rows[.5,'active']==2
        guarded=copy.deepcopy(message)
        for t in guarded['components'][0]['chart']['transitions']:t['guard']='0';t['lifecycle']['retire']='1/0'
        assert call(guarded)[.5,'total']==0
        bad=copy.deepcopy(message);bad['components'][0]['messages'].append(dict(time=1,agent=0,sequence=0,event='replace'));call(bad,mode='lint');call(bad,valid=False)
        # Stale timeout after re-entry cannot execute replacement twice.
        stale=copy.deepcopy(timeout);p=stale['components'][0];p['chart']['transitions'].append(dict(id='refresh',source=0,target=0,trigger='message',event='refresh'));p['messages']=[dict(time=.25,agent=0,sequence=0,event='refresh')];stale['time']['dt']=.25
        rows=call(stale);assert rows[.5,'total']==1 and rows[.75,'total']==2
        # Reproduction retains parents while each child starts its own chart clock and epoch.
        reproduce=copy.deepcopy(timeout);reproduce['time']['horizon']=2;reproduce['components'][0]['chart']['transitions'][0]['lifecycle']['retire']='0';rows=call(reproduce)
        for tick in range(5):assert rows[tick/2,'active']==2**(tick+1)
        # Immediate feedback is stopped transactionally by the explicit allocation limit.
        bad=copy.deepcopy(timeout);bad['parameters'][0]['value']=0;bad['components'][0]['agent_limit']=5;call(bad,mode='lint');call(bad,valid=False)
        # Independent global event calendar assigns child IDs in event order, then draws by child ID.
        experiment=dict(seed=7319,replications=3,scenarios=[dict(id=0,parameters={}),dict(id=4,parameters=dict(hazard=0)),dict(id=9,parameters=dict(hazard=2.5))])
        actual=call(rate,experiment=experiment);assert call(rate,experiment=experiment)==actual
        for scenario,hazard in [(0,.7),(4,0),(9,2.5)]:
            for replication in range(3):
                state={0:0,1:0};next_id=2;calendar=[]
                def schedule(agent,time):
                    if hazard:heapq.heappush(calendar,(time-math.log((word(7319,scenario,replication,agent,0,71)+.5)/2**32)/hazard,agent))
                schedule(0,0);schedule(1,0)
                for tick in range(11):
                    t=tick/2
                    while calendar and calendar[0][0]<=t:
                        event,parent=heapq.heappop(calendar);value=state.pop(parent)+1;state[next_id]=value;schedule(next_id,event);next_id+=1
                    for name,value in dict(total=sum(state.values()),active=len(state),alive0=int(0 in state),alive1=int(1 in state)).items():
                        assert actual[scenario,replication,t,name]==value,(scenario,replication,t,name);observations+=1
        dense=copy.deepcopy(rate);dense['time']['dt']=.25;d=call(dense,experiment=experiment);assert all(d[k]==v for k,v in actual.items())
        print(f'behavior lifecycle: {observations} independent observations plus guard, stale, reproduction, spatial, limit and replay checks passed')
if __name__=='__main__':main()
