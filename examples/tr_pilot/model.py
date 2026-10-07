"""Compile monthly T&R accounting/policy into unit-checked native SD IR.

No forward trajectories are calculated here. Every next-state expression is
evaluated and committed by AnkuraFathom's CPU Euler runtime.
"""
import json
import math
from pathlib import Path
import pyarrow as pa
import pyarrow.parquet as pq
from config import validate


class Builder:
    def __init__(self, months):
        self.doc=dict(ir_version='0.1',name='T&R staged monthly model v1',mode='sd',
                      time=dict(unit='month',dt=1,horizon=months),parameters=[],components=[],outputs=[],checks=[],data=[])
        self.units={};self.states={};self.params={}

    def p(self,name,value,unit='1'):
        self.doc['parameters'].append(dict(id=name,value=float(value),unit=unit));self.units[name]=unit
        self.params[name]=float(value);return name

    def aux(self,name,expr,unit):
        self.doc['components'].append(dict(id=name,kind='aux',expr=expr,unit=unit));self.units[name]=unit
        return name

    def stock(self,name,initial,unit,nonnegative=True,output=True):
        self.doc['components'].append(dict(id=name,kind='stock',init=float(initial),unit=unit,non_negative=nonnegative))
        self.units[name]=unit;self.states[name]=float(initial)
        if output:self.doc['outputs'].append(dict(id=name,stock=name))
        return name

    def update(self,name,expr):
        self.doc['components'].append(dict(id='step_'+name,kind='flow',source=None,destination=name,
            expr=f'(({expr}) - {name}) / dt',unit=self.units[name]+'/month',non_negative=False))

    def queue(self,name,expr,lag,unit):
        if lag==0:return expr,[]
        names=[self.stock(f'{name}_{i}',0,unit,output=False) for i in range(lag)]
        for i,n in enumerate(names):self.update(n,names[i+1] if i+1<lag else expr)
        return names[0],names

    def metric(self,name,expr,unit,nonnegative=True,cumulative=False):
        self.stock('m_'+name,0,unit,nonnegative);self.update('m_'+name,expr)
        if cumulative:
            self.stock('cum_'+name,0,unit,nonnegative);self.update('cum_'+name,'cum_'+name+' + '+expr)

    def check(self,name,expr):
        self.doc['checks'].append(dict(id=name,kind='assert',expr=expr,absolute_tolerance=2e-6,relative_tolerance=2e-10))


def total(names,zero):return ' + '.join(names) if names else zero


def build(c, directory, calendar):
    validate(c)
    if set(calendar)!=set(('time','adoption','initial','market','season')):
        raise ValueError('unknown/missing calendar series')
    if any(len(v)!=c['months']+1 for v in calendar.values()):raise ValueError('calendar horizon mismatch')
    if calendar['time']!=list(range(c['months']+1)):raise ValueError('monthly calendar grid required')
    if any(not isinstance(v,(int,float)) or not math.isfinite(v) or v<0 for values in calendar.values() for v in values):
        raise ValueError('invalid calendar number')
    if any(v>1 for v in calendar['adoption']) or any(v<=0 for v in calendar['market']):
        raise ValueError('invalid adoption or market index')
    if calendar['initial']!=[1.]+[0.]*c['months']:raise ValueError('initial training pulse required exactly once')
    directory=Path(directory);directory.mkdir(parents=True,exist_ok=False)
    b=Builder(c['months'])
    for name,unit in [('h','hours'),('f','people'),('j','jobs'),('u','USD')]:
        b.p('zero_'+name,0,unit);b.p('one_'+name,1,unit)
    b.p('one_month',1,'month')
    scalar_units=dict(paid_hours='hours/people',fixed_overhead='USD',variable_delivery_cost='USD/hours',
        prospects='jobs',hiring_cost='USD/people',ai_license='USD/people',ai_training_cost='USD/people',
        ai_training_hours='hours/people',bd_jobs_per_hour='jobs/hours',bd_max_jobs='jobs',
        retainer_fee='USD/jobs',success_fee='USD/jobs',recruitment_cap='people')
    omitted={'version','months','levels','hire_lag','bd_lag','approval_lag','collection_lag','holdback_lag','macro_lag'}
    for k,v in c.items():
        if k not in omitted:b.p(k,v,scalar_units.get(k,'1'))
    for name,value in dict(ai=0,erosion=0,bd=0,macro=0,fixed_shift=0,demand_scale=1,success_gate=1,
                           policy_hold=1,policy_responsive=0).items():b.p(name,value)
    # All calendar functions are file-bound and captured by the native manifest.
    pq.write_table(pa.table({k:pa.array(v,type=pa.float64()) for k,v in calendar.items()}),directory/'calendar.parquet')
    for name in sorted(calendar.keys()-{'time'}):
        b.doc['data'].append(dict(id=name,source='calendar.parquet',schema=dict(key_column='time',
            columns=[dict(name=k,type='f64',unit='month' if k=='time' else '1') for k in calendar]),
            use=dict(kind='exogenous_series',time_column='time',value_column=name,interpolate='hold',extrapolate='hold')))
    b.aux('adopt','ai * adoption(t)','1')
    b.aux('ai_enabled','IF_POSITIVE(ai,1,0)','1')
    macro_due,_=b.queue('macro_delay','market(t)',c['macro_lag'],'1')
    # Zero-initialized delay falls back to the neutral index until the first input arrives.
    b.aux('market_factor',f'MAX(0,1 + macro * macro_elasticity * (IF_POSITIVE({macro_due},{macro_due},1) - 1))','1')
    bd_due,bd_queue=b.queue('bd_delay','bd_generated',c['bd_lag'],'jobs')
    b.aux('new_prospects',f'prospects * demand_scale * season(t) * market_factor + ({bd_due})','jobs')
    b.aux('won','new_prospects * win_rate','jobs')
    b.aux('share_fixed','fixed_share + fixed_shift','1')
    b.aux('share_tm','1 - share_fixed - retainer_share - success_share','1')
    b.check('eligible_fee_mix','share_fixed <= fixed_eligible_share')
    b.check('nonnegative_hourly_mix','share_tm >= 0')
    # Separate contract backlogs and locked prices. New pricing never rewrites old value.
    fees=('tm','fixed','retainer','success')
    for fee in fees:
        share={'tm':'share_tm','fixed':'share_fixed','retainer':'retainer_share','success':'success_share'}[fee]
        init=c['initial_backlog']*{'tm':1-c['fixed_share']-c['retainer_share']-c['success_share'],
            'fixed':c['fixed_share'],'retainer':c['retainer_share'],'success':c['success_share']}[fee]
        b.stock('backlog_'+fee,init,'jobs')
        b.aux('new_'+fee,'won * '+share,'jobs')
        b.aux('available_'+fee,'backlog_'+fee+' + new_'+fee,'jobs')
    b.aux('work_available',total(['available_'+f for f in fees],'zero_j'),'jobs')
    capacities=[];level_data=[];rates=[];base_rates=[];costs=[];payroll=[];support=[];loaded=[]
    paid=[];admin=[];bd_hours=[];current=[];starts=[];exits=[];promotions=[];requests=[];training=[]
    for i,l in enumerate(c['levels']):
        prefix=f'l{i}'
        for key,unit in dict(rate='USD/hours',annual_pay='USD/people',hours_per_job='hours/jobs',
                             admin_share='1',exposure='1',promotion='1',bd_eligible='1',count='people').items():
            b.p(prefix+'_'+key,l[key],unit)
        b.p(prefix+'_exit_q',1-(1-l['attrition'])**(1/12))
        b.stock(prefix+'_fte',l['count'],'people')
        due,q=b.queue(prefix+'_hire','request_'+prefix,c['hire_lag'],'people')
        prom_in=f'promote_l{i-1}' if i else 'zero_f'
        b.aux('exit_'+prefix,f'{prefix}_fte * {prefix}_exit_q','people')
        b.aux('promote_'+prefix,f'({prefix}_fte - exit_{prefix}) * {prefix}_promotion','people')
        b.aux('starts_'+prefix,f'({due}) + policy_hold * exit_{prefix}','people')
        b.aux('current_'+prefix,f'{prefix}_fte - exit_{prefix} - promote_{prefix} + {prom_in} + starts_{prefix}','people')
        b.update(prefix+'_fte','current_'+prefix)
        b.aux('paid_'+prefix,f'current_{prefix} * paid_hours','hours')
        b.aux('admin_'+prefix,f'paid_{prefix} * {prefix}_admin_share','hours')
        b.aux('training_'+prefix,f'MIN(paid_{prefix} - admin_{prefix}, ai_enabled * ai_training_hours * ({prefix}_count * initial(t) + starts_{prefix}))','hours')
        b.aux('onboard_'+prefix,f'MIN(paid_{prefix} - admin_{prefix} - training_{prefix}, ({due}) * paid_hours * (1-onboard_productivity))','hours')
        b.aux('free_'+prefix,f'MAX(zero_h, paid_{prefix} - admin_{prefix} - training_{prefix} - onboard_{prefix})','hours')
        b.aux('labor_'+prefix,f'1 - adopt * {prefix}_exposure + adopt * {prefix}_exposure * ai_rework','1')
        if l['hours_per_job']:
            b.aux('task_'+prefix,f'{prefix}_hours_per_job * labor_{prefix}','hours/jobs')
            # BD can use only surplus relative to required current work. It competes for capacity.
            b.aux('bd_'+prefix,f'bd * {prefix}_bd_eligible * bd_fraction * MAX(zero_h, free_{prefix} - work_available * task_{prefix})','hours')
            b.aux('capacity_'+prefix,f'IF_POSITIVE(task_{prefix} / (one_h / one_j),(free_{prefix} - bd_{prefix}) / task_{prefix},work_available)','jobs')
            capacities.append('capacity_'+prefix)
            b.aux('desired_'+prefix,f'(won + (work_available - won) / backlog_clear_months) * task_{prefix} / (paid_hours * MIN(target_utilization,1-{prefix}_admin_share))','people')
            rates.append(f'task_{prefix} * {prefix}_rate * realization')
            base_rates.append(f'{prefix}_hours_per_job * {prefix}_rate * realization')
        else:
            b.aux('bd_'+prefix,'zero_h','hours');b.aux('desired_'+prefix,prefix+'_count','people')
        pending=total(q[1:],'zero_f')
        b.aux('request_'+prefix,f'policy_responsive * MIN(recruitment_cap, MAX(zero_f, desired_{prefix} - current_{prefix} - ({pending})))','people')
        b.aux('basepay_'+prefix,f'current_{prefix} * {prefix}_annual_pay / 12','USD')
        b.aux('loaded_'+prefix,f'basepay_{prefix} * (1+employer_load+bonus_share)','USD')
        b.metric('fte_'+prefix,'current_'+prefix,'people')
        for k in ('paid','admin','training','onboard','bd'):
            b.metric(k+'_'+prefix,k+'_'+prefix,'hours')
        for k in ('exit','starts','request','promote'):
            b.metric(k+'_'+prefix,k+'_'+prefix,'people',cumulative=True)
        b.metric('basepay_'+prefix,'basepay_'+prefix,'USD')
        b.metric('loaded_'+prefix,'loaded_'+prefix,'USD')
        paid.append('paid_'+prefix);admin.append('admin_'+prefix);bd_hours.append('bd_'+prefix)
        current.append('current_'+prefix);starts.append('starts_'+prefix);exits.append('exit_'+prefix)
        promotions.append('promote_'+prefix);requests.append('request_'+prefix)
        payroll.append('basepay_'+prefix);loaded.append('loaded_'+prefix)
        if not l['hours_per_job']:support.append('loaded_'+prefix)
        level_data.append((i,l))
    cap=capacities[0]
    for x in capacities[1:]:cap=f'MIN({cap},{x})'
    b.aux('delivery',f'MIN(work_available,{cap})','jobs')
    b.aux('fraction_done','IF_POSITIVE(work_available / one_j,delivery / work_available,0)','1')
    b.aux('hourly_value',total(rates,'zero_u / one_j'),'USD/jobs')
    b.aux('baseline_value',total(base_rates,'zero_u / one_j'),'USD/jobs')
    b.aux('saving','1 - hourly_value / baseline_value','1')
    b.aux('new_fixed_price','baseline_value * (1-erosion * saving)','USD/jobs')
    b.aux('bd_generated',f'MIN(bd_max_jobs, ({total(bd_hours,"zero_h")}) * bd_jobs_per_hour)','jobs')
    b.metric('prospects','new_prospects','jobs',cumulative=True);b.metric('won','won','jobs',cumulative=True)
    b.metric('delivery','delivery','jobs',cumulative=True);b.metric('bd_prospects','bd_generated','jobs',cumulative=True)
    b.metric('paid',total(paid,'zero_h'),'hours',cumulative=True)
    b.metric('fte',total(current,'zero_f'),'people')
    b.metric('bd_hours',total(bd_hours,'zero_h'),'hours',cumulative=True)
    bill_hours=[];bill_gross=[];all_hours=[]
    for i,l in level_data:
        p=f'l{i}'
        expr=f'delivery * task_{p}' if l['hours_per_job'] else 'zero_h'
        b.aux('delivery_'+p,expr,'hours')
        b.aux('idle_'+p,f'MAX(zero_h,free_{p} - bd_{p} - delivery_{p})','hours')
        b.metric('delivery_'+p,'delivery_'+p,'hours',cumulative=True)
        b.metric('idle_'+p,'idle_'+p,'hours')
        b.check('hours_'+p,f'm_paid_{p} == m_admin_{p} + m_training_{p} + m_onboard_{p} + m_bd_{p} + m_delivery_{p} + m_idle_{p}')
        b.p(p+'_initial',l['count'],'people')
        prom_in=f'cum_promote_l{i-1}' if i else 'zero_f'
        b.check('workforce_'+p,f'{p}_fte == {p}_initial + cum_starts_{p} - cum_exit_{p} + {prom_in} - cum_promote_{p}')
        all_hours.append('delivery_'+p)
        if l['hours_per_job']:
            bill_hours.append(f'done_tm * task_{p}');bill_gross.append(f'done_tm * task_{p} * {p}_rate')
    b.aux('delivery_hours',total(all_hours,'zero_h'),'hours')
    b.metric('delivery_hours','delivery_hours','hours',cumulative=True)
    b.metric('hourly_hours',total(bill_hours,'zero_h'),'hours',cumulative=True)
    b.metric('hourly_standard',total(bill_gross,'zero_u'),'USD',cumulative=True)
    asset_queues=[];disallowed=[];cash=[];earned=[];deposits=[];deposit_used=[]
    baseline_value=sum(l['hours_per_job']*l['rate']*c['realization'] for l in c['levels'])
    for fee in fees:
        b.aux('done_'+fee,'available_'+fee+' * fraction_done','jobs')
        b.update('backlog_'+fee,f'MAX(zero_j,available_{fee} - done_{fee})')
        if fee in ('fixed','retainer','success'):
            price={'fixed':'new_fixed_price','retainer':'retainer_fee',
                   'success':'success_fee * success_probability * success_gate'}[fee]
            oldprice={'fixed':baseline_value,'retainer':c['retainer_fee'],
                      'success':c['success_fee']*c['success_probability']}[fee]
            init=b.states['backlog_'+fee]*oldprice
            b.stock('value_'+fee,init,'USD')
            b.aux('value_available_'+fee,f'value_{fee} + new_{fee} * ({price})','USD')
            b.aux('earned_'+fee,f'value_available_{fee} * fraction_done','USD')
            b.update('value_'+fee,f'MAX(zero_u,value_available_{fee} - earned_{fee})')
        else:b.aux('earned_tm','done_tm * hourly_value','USD')
        deposit='new_fixed * new_fixed_price * fixed_deposit_share' if fee=='fixed' else 'zero_u'
        used='earned_fixed * fixed_deposit_share' if fee=='fixed' else 'zero_u'
        deposits.append('('+deposit+')');deposit_used.append('('+used+')')
        bill=f'earned_{fee} - ({used})'
        approval,aq=b.queue('approval_'+fee,bill,c['approval_lag'],'USD');asset_queues+=aq
        dis=f'({approval}) * hourly_disallowance' if fee=='tm' else 'zero_u'
        b.aux('disallowed_'+fee,dis,'USD');disallowed.append('disallowed_'+fee)
        hold='hourly_holdback' if fee=='tm' else '0'
        b.aux('approved_'+fee,f'({approval}) - disallowed_{fee}','USD')
        retained,hq=b.queue('hold_'+fee,f'approved_{fee} * {hold}',c['holdback_lag'],'USD');asset_queues+=hq
        collection,cq=b.queue('collect_'+fee,f'approved_{fee} * (1-{hold}) + ({retained})',c['collection_lag'],'USD');asset_queues+=cq
        b.aux('cash_'+fee,collection,'USD');cash.append('cash_'+fee);earned.append('earned_'+fee)
        for metric in ('earned','disallowed','cash'):
            b.metric(metric+'_'+fee,metric+'_'+fee,'USD',cumulative=True)
    # Initial fixed backlog is assumed funded with the specified advance deposit.
    initdeposit=b.states['value_fixed']*c['fixed_deposit_share']
    b.stock('deposit_liability',initdeposit,'USD')
    b.p('opening_deposit',initdeposit,'USD')
    b.aux('new_deposits',total(deposits,'zero_u'),'USD')
    b.aux('used_deposits',total(deposit_used,'zero_u'),'USD')
    b.update('deposit_liability','MAX(zero_u,deposit_liability + new_deposits - used_deposits)')
    b.aux('earned',total(earned,'zero_u'),'USD')
    b.aux('disallowed',total(disallowed,'zero_u'),'USD')
    b.aux('revenue','earned - disallowed','USD')
    b.aux('collections',total(cash,'zero_u')+' + new_deposits','USD')
    b.aux('base_comp',total(payroll,'zero_u'),'USD')
    b.aux('loaded_comp',total(loaded,'zero_u'),'USD')
    b.aux('support_comp',total(support,'zero_u'),'USD')
    b.aux('ai_cost',f'ai_enabled * (({total(current,"zero_f")}) * ai_license + ai_training_cost * (initial(t) * ({sum(l["count"] for l in c["levels"])} * one_f) + ({total(starts,"zero_f")})))','USD')
    b.aux('recruiting_cost',f'({total(starts,"zero_f")}) * hiring_cost','USD')
    b.aux('overhead','fixed_overhead + variable_overhead * MAX(zero_u,revenue)','USD')
    b.aux('variable_cost','delivery_hours * variable_delivery_cost','USD')
    b.aux('cost','loaded_comp + ai_cost + recruiting_cost + overhead + variable_cost','USD')
    b.aux('ebitda','revenue - cost','USD')
    b.aux('cash_flow','collections - cost','USD')
    for k in ('earned','disallowed','revenue','collections','base_comp','loaded_comp','support_comp',
              'ai_cost','recruiting_cost','overhead','variable_cost','cost','ebitda','cash_flow','new_deposits','used_deposits'):
        b.metric(k,k,'USD',nonnegative=k not in ('revenue','ebitda','cash_flow'),cumulative=True)
    b.check('ebitda_identity','cum_ebitda == cum_revenue - cum_cost')
    b.check('revenue_identity','cum_revenue == cum_earned - cum_disallowed')
    b.check('cash_identity','cum_cash_flow == cum_collections - cum_cost')
    b.check('cash_assets',f'cum_revenue - cum_collections == ({total(asset_queues,"zero_u")}) - deposit_liability + opening_deposit')
    b.check('backlog_identity',f'({total(["backlog_"+f for f in fees],"zero_j")}) == initial_backlog * one_j + cum_won - cum_delivery')
    b.doc['outputs'].append(dict(id='receivables',expr=total(asset_queues,'zero_u'),unit='USD'))
    # Bind all model parameters to a typed, one-row Parquet table. Scenario overrides retain precedence.
    pq.write_table(pa.table({'key':pa.array(['practice']),**{k:pa.array([v],type=pa.float64()) for k,v in b.params.items()}}),directory/'parameters.parquet')
    b.doc['data'].append(dict(id='practice_parameters',source='parameters.parquet',schema=dict(key_column='key',
        columns=[dict(name='key',type='string',unit='')]+[dict(name=k,type='f64',unit=b.units[k]) for k in b.params]),
        use=dict(kind='parameter_table',key='practice',parameters=[dict(parameter=k,column=k) for k in b.params])))
    (directory/'model.json').write_text(json.dumps(b.doc,indent=2)+'\n')
    return b.doc


def experiment(rows):
    return dict(seed=20260930,replications=1,scenarios=[dict(id=i,parameters={
        **{k:float(s[k]) for k in ('ai','erosion','bd','macro','fixed_shift','demand_scale','success_gate')},
        'policy_hold':float(s['policy']=='hold'),'policy_responsive':float(s['policy']=='responsive')}) for i,s in enumerate(rows)])
