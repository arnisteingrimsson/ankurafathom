"""Topic CLI against a separate integer-clock request/ack scheduler and hand cases."""
import copy,csv,io,json,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def reference(count,publications):
    # Integer quarter-day clock. Two explicit protocol handlers, no native expressions.
    values=[0]*count;senders=[0]*count;rows={};tick=2
    for time in range(9):
        pending=[(p['topic'],p['sender'],p['sequence'],p.get('receiver'),p['value']) for p in publications if round(p['time']*4)==time]
        sequences=[0]*count
        while pending:
            replies=[]
            for topic,sender,sequence,receiver,value in sorted(pending,key=lambda x:x[:3]):
                for agent in range(count) if receiver is None else [receiver]:
                    values[agent]+=value;senders[agent]=sender
                    if topic=='request':
                        replies.append(('ack',agent,sequences[agent],sender,2*value));sequences[agent]+=1
            pending=replies
        if time and time%tick==0:values=[2*v for v in values]
        if time%2==0:
            for agent in range(count):rows[time/4,f'agent{agent}']=values[agent];rows[time/4,f'sender{agent}']=senders[agent]
    return rows

def main():
    exe=str(Path(sys.argv[1]).resolve());fixture=json.loads((ROOT/'models/typed_abm_topics.ir.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory)/'model.json'
        def call(model,mode='run',valid=True):
            path.write_text(json.dumps(model));r=subprocess.run([exe,mode,str(path)],capture_output=True,text=True)
            if not valid:
                assert r.returncode!=0,'invalid topic model accepted';d=json.loads(r.stderr)['diagnostics'][0];assert d['code'].startswith('IR_') and d['pointer'].startswith('/'),d;return d
            assert r.returncode==0,r.stderr
            if mode=='lint':return
            return {(float(x['time']),x['output_id']):float(x['value']) for x in csv.DictReader(io.StringIO(r.stdout))}
        rows=call(fixture)
        assert [rows[t,'first'] for t in [0,.5,1,1.5,2]]==[0,15,28,38,48]
        assert [rows[t,'second'] for t in [0,.5,1,1.5,2]]==[0,11,27,37,47]
        observations=0
        for case in range(24):
            m=copy.deepcopy(fixture);p=m['components'][0];count=2+case%4
            p['agents']=[dict(received=0,last_sender=0) for _ in range(count)]
            p['phases'][0]['assign'][0]['expr']='received*2'
            for topic in p['topics']:topic['capacity']=1000
            p['publications']=[]
            for i in range(12):
                v=dict(time=((i*3+case)%9)/4,topic='ack' if i%4==0 else 'request',sender=(i+case)%count,sequence=i,value=1+i%3)
                if (i+case)%3:v['receiver']=(i*2+case)%count
                p['publications'].append(v)
            m['outputs']=[o for i in range(count) for o in [dict(id=f'agent{i}',agent=i,field='received'),dict(id=f'sender{i}',agent=i,field='last_sender')]]
            expected=reference(count,p['publications']);actual=call(m);assert actual==expected,(case,actual,expected);observations+=len(expected)
            p['publications'].reverse();p['topics'].reverse();assert call(m)==expected,'bag/topic declaration order differs'
        # Replies and assignments both read the pre-handler record.
        m=copy.deepcopy(fixture);p=m['components'][0];p['phases']=[];p['publications']=p['publications'][:1];p['publications'][0]['receiver']=1
        p['agents'][1]['received']=7;p['topics'][0]['publish'][0]['value']='received'
        rows=call(m);assert rows[.5,'first']==7 and rows[.5,'second']==8
        p['topics'][0]['guard']='0';rows=call(m);assert rows[2,'first']==0 and rows[2,'second']==7
        for target,expected in [('self',(0,3)),('sender',(2,1)),('broadcast',(2,3)),(0,(2,1)),(1,(0,3))]:
            m=copy.deepcopy(fixture);p=m['components'][0];p['phases']=[];p['publications']=p['publications'][:1];p['publications'][0]['receiver']=1
            p['topics'][0]['publish'][0]['receiver']=target
            result=call(m);assert (result[2,'first'],result[2,'second'])==expected,target
        # Semantic failures happen at lint; capacity and bounded cascades fail at runtime.
        for mutate in [lambda p:p['topics'][1].update(capacity=1),lambda p:p.update(delivery_budget=3)]:
            m=copy.deepcopy(fixture);mutate(m['components'][0]);call(m,'lint');assert call(m,valid=False)['code']=='IR_ABM_RUNTIME'
        m=copy.deepcopy(fixture);p=m['components'][0];p['topics'][1]['publish']=[dict(topic='ack',receiver='self',value='message_value')];p['delivery_budget']=20
        call(m,'lint');call(m,valid=False)
        # Async topic handlers change ordinary fields; statechart timers retain their order and ownership.
        m=json.loads((ROOT/'models/typed_abm_async.ir.json').read_text());p=m['components'][0]
        p['topics']=[dict(id='credit',capacity=4,unit='1',assign=[dict(field='completed',expr='completed+message_value')])]
        p['publications']=[dict(time=1.5,topic='credit',sender=1,receiver=0,sequence=0,value=10)]
        rows=call(m);assert rows[1.5,'completed']==11 and rows[2.5,'completed']==12
        dense=copy.deepcopy(m);dense['time']['dt']=.25;dr=call(dense);assert all(dr[k]==v for k,v in rows.items())
        p['topics'][0]['assign'][0]['field']='generation';call(m,'lint',False)
        # Units propagate from topic payload to assignment and reply expressions.
        m=copy.deepcopy(fixture);p=m['components'][0];p['fields'][0]['unit']='USD';p['phases']=[]
        for t in p['topics']:t['unit']='USD'
        call(m,'lint');call(m)
        p['topics'][1]['unit']='day';call(m,'lint',False)
    print(f'24 independent topic schedules, {observations} observations plus permutations, async/unit/failure checks passed')
if __name__=='__main__':main()
