"""Predeclared deterministic interaction cases; no native simulator dependencies."""
import copy
import json
from pathlib import Path
HERE=Path(__file__).resolve().parent

def encoded(value): return json.dumps(value,indent=2,sort_keys=True,allow_nan=False)+'\n'

def make_plan():
    cases=[]
    settings=[('grid',False,False,1),('grid',False,True,1),('grid',True,False,1),('grid',True,True,2),('grid',True,True,0),('grid',True,True,8),
              ('continuous',False,False,.5),('continuous',False,True,2),('continuous',True,False,.5),('continuous',True,True,1.5),
              ('network',False,False,0),('network',False,True,0),('population',False,False,0)]
    for index,(kind,wrap,flag,radius) in enumerate(settings):
        grid=kind=='grid'
        points=[(0,0),(1,0),(3,0),(2,1)] if grid else [(-.25,0),(-.25,0),(.25,0),(1.75,1)]
        agents=[dict(x=x,y=y,value=float(2*i+1),vx=(1 if grid else (.25 if i%2 else -.5)) if wrap else 0,group='a' if i<2 else 'b') for i,(x,y) in enumerate(points)]
        fields=[dict(name=n,type=t,unit='1') for n,t in [('x','integer' if grid else 'real'),('y','integer' if grid else 'real'),('value','real'),('vx','integer' if grid else 'real'),('group','string')]]
        source='space' if kind in ('grid','continuous') else kind
        q=[dict(id='near_mean',source=source,op='mean',field='value'),dict(id='near_sum',source=source,op='sum',field='value'),dict(id='degree',source=source,op='count'),dict(id='same',source=source,op='count_same',field='group')]
        for query in q:
            query['include_self']=flag
            if source=='space': query.update(radius=radius,unit='1')
            if grid: query['moore']=flag
        q+=[dict(id='linked_sum',source='network',op='sum',field='value'),dict(id='all_count',source='population',op='count',include_self=True)]
        component=dict(id='people',kind='population',execution='sync',fields=fields,agents=agents,queries=q,
                       network=dict(directed=flag,edges=[[0,1],[0,2],[1,3],[2,3]]),
                       phases=[{'assign':[dict(field='value',expr='(value+near_mean)/2')]},
                               {'assign':[dict(field='x',expr='x+vx')]},
                               {'assign':[dict(field='value',expr='value+linked_sum/8')]}])
        if grid: component['space']=dict(kind='grid',x='x',y='y',width=4,height=3,wrap=wrap)
        elif kind=='continuous': component['space']=dict(kind='continuous',fields=['x','y'],lower=[-1,-1],upper=[3,3],bin_width=.6,wrap=wrap,unit='1')
        outputs=[]
        for i in range(4):
            outputs += [dict(id=f'a{i}_{f}',agent=i,field=f) for f in ['x','y','value']]
            outputs += [dict(id=f'a{i}_{query["id"]}',agent=i,query=query['id']) for query in q]
        model=dict(ir_version='0.1',name=f'interaction_{index}_{kind}',mode='abm',time=dict(unit='day',dt=1,horizon=8),parameters=[],components=[component],outputs=outputs)
        cases.append(dict(id=model['name'],model=model))
    return dict(version=1,cases=cases)

if __name__=='__main__':
    import sys
    path=HERE/'interaction-plan.json'; content=encoded(make_plan())
    if sys.argv[1:] == ['--generate']: path.write_text(content)
    elif sys.argv[1:] == ['--verify']:
        if path.read_text()!=content: raise ValueError('stale interaction plan')
        print(f'interaction plan: {len(make_plan()["cases"])} cases')
    else: raise SystemExit('use --generate or --verify')
