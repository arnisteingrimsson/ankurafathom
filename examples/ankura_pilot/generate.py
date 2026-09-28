"""Generate a synthetic, data-bound SD translation of the Phase A economics reference."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FLAGS = ('copilot', 'tool', 'automation', 'acquisition')
UNITS = dict(fte='people', paid_hours_per_fte_month='hours/people/month', delivery_share='1',
    realized_rate='USD/hours', monthly_pipeline_hours='hours/month', win_rate='1', fixed_fee_share='1',
    annual_pay_per_fte='USD/people/year', variable_cost_per_hour='USD/hours', annual_demand_growth='1',
    ai_task_share='1', tool_task_share='1', automation_task_share='1',
    initial_tm_backlog_hours='hours', initial_fixed_backlog_hours='hours')

def scenario_name(flags):
    return '+'.join(name for i,name in enumerate(FLAGS) if flags & (1 << i)) or 'baseline'

def generate(destination, source=ROOT/'models/synthetic_practices.csv', scenarios=16):
    import pyarrow as pa
    import pyarrow.parquet as pq
    if not 16 <= scenarios <= 65536: raise ValueError('scenario count must be 16..65536')
    source=Path(source)
    with source.open(newline='') as file:
        reader=csv.DictReader(file)
        if reader.fieldnames != ['name',*UNITS]: raise ValueError('unexpected practice columns')
        records=[{'name':r['name'],**{k:float(r[k]) for k in UNITS}} for r in reader]
    if not records or len({r['name'] for r in records})!=len(records) or any(not r['name'] for r in records):
        raise ValueError('practices need unique nonempty names')
    fractions=('delivery_share','win_rate','fixed_fee_share','ai_task_share','tool_task_share','automation_task_share')
    for r in records:
        if any(not math.isfinite(r[k]) for k in UNITS): raise ValueError('nonfinite practice input')
        if any(r[k]<0 for k in UNITS if k!='annual_demand_growth'): raise ValueError('negative practice input')
        if any(not 0<=r[k]<=1 for k in fractions) or r['annual_demand_growth']<=-1: raise ValueError('invalid fraction/growth')
    destination=Path(destination);destination.mkdir(parents=True,exist_ok=False)
    table=pa.table({'name':pa.array([r['name'] for r in records],type=pa.string()),
                    **{k:pa.array([r[k] for r in records],type=pa.float64()) for k in UNITS}})
    pq.write_table(table,destination/'practices.parquet')
    # Source bytes are retained separately so every derived assumption is reviewable.
    (destination/'synthetic_practices.csv').write_bytes(source.read_bytes())
    experiment=dict(seed=0,replications=1,scenarios=[dict(id=i,parameters={
        'enable_'+name:float(bool((i%16)&(1<<bit))) for bit,name in enumerate(FLAGS)}) for i in range(scenarios)])
    (destination/'scenarios.json').write_text(json.dumps(experiment,indent=2)+'\n')
    models=[]
    for index,p in enumerate(records):
        calendar={'time':[float(t) for t in range(61)]}
        calendar.update({name:[] for name in ('growth','copilot_adoption','tool_adoption','automation_adoption',
            'acquired','initial','tool_operating','automation_operating','acquisition_payment','integration')})
        for t in range(61):
            month=t+1
            calendar['growth'].append((1+p['annual_demand_growth'])**(t/12))
            for name,first in (('copilot',2),('tool',10),('automation',7)):
                calendar[name+'_adoption'].append(max(0.,min(1.,(month-first+1)/6)))
            for name,condition in [('acquired',month>=13),('initial',month==1),('tool_operating',month>=10),
                    ('automation_operating',month>=7),('acquisition_payment',month==13),('integration',13<=month<31)]:
                calendar[name].append(float(condition))
        calendar_file=f'calendar-{index}.parquet';pq.write_table(pa.table(calendar),destination/calendar_file)
        parameters=[dict(id=k,value=p[k],unit=u) for k,u in UNITS.items()]
        parameters += [dict(id='enable_'+name,value=0,unit='1') for name in FLAGS]
        constants={
            'one_hour':(1,'hours'),'months_per_year':(12,'month/year'),
            'copilot_effect':(.08,'1'),'tool_effect':(.15,'1'),'automation_effect':(.30*.85,'1'),
            'acquisition_fte_share':(.15,'1'),'acquisition_pipeline_share':(.20,'1'),
            'copilot_license':(40,'USD/people/month'),'copilot_training':(300,'USD/people'),
            'tool_build_cost':(750000,'USD'),'tool_operating_cost':(5000,'USD/month'),
            'automation_build_cost':(250000,'USD'),'automation_operating_cost':(3000,'USD/month'),
            'acquisition_cost':(2000000,'USD'),'integration_cost':(20000,'USD/month')}
        parameters += [dict(id=k,value=v,unit=u) for k,(v,u) in constants.items()]
        components=[];outputs=[]
        def stock(name,initial,unit,nonnegative=True):
            components.append(dict(id=name,kind='stock',init=initial,unit=unit,non_negative=nonnegative))
            outputs.append(dict(id=name,stock=name))
        def aux(name,expr,unit): components.append(dict(id=name,kind='aux',expr=expr,unit=unit))
        def flow(name,destination,expr,unit,nonnegative=True):
            components.append(dict(id=name,kind='flow',source=None,destination=destination,
                                   expr=expr,unit=unit,non_negative=nonnegative))
        stock('tm_backlog',p['initial_tm_backlog_hours'],'hours')
        stock('fixed_backlog',p['initial_fixed_backlog_hours'],'hours')
        stock('headcount',p['fte'],'people')
        for name,unit,nonnegative in [('revenue','USD',True),('cost','USD',True),('profit','USD',False),
                ('actual_delivery','hours',True),('paid_hours','hours',True),('tm_won','hours',True),
                ('fixed_won','hours',True),('tm_delivered','hours',True),('fixed_delivered','hours',True)]:
            stock('cumulative_'+name,0,unit,nonnegative)
        aux('current_fte','fte * (1 + enable_acquisition * acquisition_fte_share * acquired(t))','people')
        aux('paid_rate','current_fte * paid_hours_per_fte_month','hours/month')
        aux('won_rate','monthly_pipeline_hours * growth(t) * (1 + enable_acquisition * acquisition_pipeline_share * acquired(t)) * win_rate','hours/month')
        aux('fixed_won_rate','won_rate * fixed_fee_share','hours/month')
        aux('tm_won_rate','won_rate - fixed_won_rate','hours/month')
        aux('available_tm','tm_backlog + tm_won_rate * dt','hours')
        aux('available_fixed','fixed_backlog + fixed_won_rate * dt','hours')
        aux('labor_factor','MAX(0.5,(1-enable_copilot*copilot_effect*ai_task_share*copilot_adoption(t)) * (1-enable_tool*tool_effect*tool_task_share*tool_adoption(t)) * (1-enable_automation*automation_effect*automation_task_share*automation_adoption(t)))','1')
        aux('actual_needed','(available_tm + available_fixed) * labor_factor','hours')
        aux('capacity','paid_rate * delivery_share * dt','hours')
        aux('share_delivered','IF_POSITIVE(actual_needed / one_hour, MIN(1,capacity / actual_needed), 0)','1')
        aux('tm_delivered_rate','available_tm * share_delivered / dt','hours/month')
        aux('fixed_delivered_rate','available_fixed * share_delivered / dt','hours/month')
        aux('actual_delivery_rate','(tm_delivered_rate + fixed_delivered_rate) * labor_factor','hours/month')
        aux('revenue_rate','(tm_delivered_rate * labor_factor + fixed_delivered_rate) * realized_rate','USD/month')
        aux('copilot_cost','enable_copilot * (copilot_license * current_fte + copilot_training * fte * initial(t) / dt)','USD/month')
        aux('tool_cost','enable_tool * (tool_build_cost * initial(t) / dt + tool_operating_cost * tool_operating(t))','USD/month')
        aux('automation_cost','enable_automation * (automation_build_cost * initial(t) / dt + automation_operating_cost * automation_operating(t))','USD/month')
        aux('acquire_cost','enable_acquisition * (acquisition_cost * acquisition_payment(t) / dt + integration_cost * integration(t))','USD/month')
        aux('cost_rate','current_fte * annual_pay_per_fte / months_per_year + actual_delivery_rate * variable_cost_per_hour + (copilot_cost + tool_cost + automation_cost + acquire_cost)','USD/month')
        aux('profit_rate','revenue_rate - cost_rate','USD/month')
        flow('tm_change','tm_backlog','(NONNEGATIVE(available_tm - tm_delivered_rate * dt) - tm_backlog) / dt','hours/month',False)
        flow('fixed_change','fixed_backlog','(NONNEGATIVE(available_fixed - fixed_delivered_rate * dt) - fixed_backlog) / dt','hours/month',False)
        flow('headcount_change','headcount','(current_fte - headcount) / dt','people/month',False)
        for name,expr,unit,nonnegative in [('revenue','revenue_rate','USD',True),('cost','cost_rate','USD',True),
                ('profit','profit_rate','USD',False),('actual_delivery','actual_delivery_rate','hours',True),
                ('paid_hours','paid_rate','hours',True),('tm_won','tm_won_rate','hours',True),
                ('fixed_won','fixed_won_rate','hours',True),('tm_delivered','tm_delivered_rate','hours',True),
                ('fixed_delivered','fixed_delivered_rate','hours',True)]:
            flow(name+'_accumulation','cumulative_'+name,expr,unit+'/month',nonnegative)
        bindings=[dict(id='practice_inputs',source='practices.parquet',
            schema=dict(key_column='name',columns=[dict(name='name',type='string',unit='')]+
                [dict(name=k,type='f64',unit=u) for k,u in UNITS.items()]),
            use=dict(kind='parameter_table',key=p['name'],parameters=[dict(parameter=k,column=k) for k in UNITS]))]
        for name in calendar:
            if name=='time':continue
            bindings.append(dict(id=name,source=calendar_file,
                schema=dict(key_column='time',columns=[dict(name=k,type='f64',unit='month' if k=='time' else '1') for k in calendar]),
                use=dict(kind='exogenous_series',time_column='time',value_column=name,interpolate='hold',extrapolate='hold')))
        checks=[dict(id='profit_identity',kind='assert',expr='cumulative_profit == cumulative_revenue - cumulative_cost',absolute_tolerance=2e-6,relative_tolerance=1e-10),
            dict(id='tm_balance',kind='assert',expr='tm_backlog + cumulative_tm_delivered == initial_tm_backlog_hours + cumulative_tm_won',absolute_tolerance=2e-6,relative_tolerance=1e-10),
            dict(id='fixed_balance',kind='assert',expr='fixed_backlog + cumulative_fixed_delivered == initial_fixed_backlog_hours + cumulative_fixed_won',absolute_tolerance=2e-6,relative_tolerance=1e-10),
            dict(id='delivery_capacity',kind='assert',expr='actual_delivery_rate <= paid_rate',absolute_tolerance=1e-8,relative_tolerance=1e-12)]
        checks += [dict(id=name+'_nonnegative',kind='bounds',output=name,min=0,absolute_tolerance=1e-8)
                   for name in ('tm_backlog','fixed_backlog','headcount')]
        checks += [dict(id=name+'_increasing',kind='monotone',output='cumulative_'+name,direction='increasing',absolute_tolerance=1e-8)
                   for name in ('revenue','cost','actual_delivery','paid_hours','tm_delivered','fixed_delivered')]
        model=dict(ir_version='0.1',name=p['name']+' synthetic economics',mode='sd',
                   time=dict(unit='month',dt=1,horizon=60),parameters=parameters,components=components,outputs=outputs,data=bindings,checks=checks)
        filename=f'practice-{index}.ir.json';(destination/filename).write_text(json.dumps(model,indent=2)+'\n')
        models.append(dict(practice=p['name'],model=filename))
    plan=dict(version='0.1',scope='synthetic economics preview; M7/M8 acceptance pending',
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),models=models,experiment='scenarios.json',
        scenario_names={str(i):scenario_name(i%16) for i in range(scenarios)},
        assumptions='Phase A coefficients, ramps and costs are synthetic assumptions, not estimated causal effects. Initial stocks are generated from source rows; regenerate after changing source inputs.')
    (destination/'pilot.json').write_text(json.dumps(plan,indent=2)+'\n')
    return plan

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('destination',type=Path)
    p.add_argument('--practices',type=Path,default=ROOT/'models/synthetic_practices.csv')
    p.add_argument('--scenarios',type=int,default=16);a=p.parse_args()
    print(json.dumps(generate(a.destination,a.practices,a.scenarios),indent=2))
