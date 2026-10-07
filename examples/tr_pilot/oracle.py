"""Independent monthly ledger using Decimal and explicit queues, never IR expressions."""
from decimal import Decimal, localcontext
import math


def D(value):return Decimal(str(value))


def simulate(c,s,calendar):
    with localcontext() as context:
        context.prec=36
        yield from _simulate(c,s,calendar)


def _simulate(c,s,calendar):
    a={k:D(v) for k,v in c.items() if k!='levels'}
    lv=[{k:(v if k=='id' else D(v)) for k,v in row.items()} for row in c['levels']]
    scenario={k:D(v) for k,v in s.items() if k not in ('name','policy')}
    mix={'tm':1-a['fixed_share']-a['retainer_share']-a['success_share']-scenario['fixed_shift'],
         'fixed':a['fixed_share']+scenario['fixed_shift'],'retainer':a['retainer_share'],'success':a['success_share']}
    if mix['fixed']>a['fixed_eligible_share'] or mix['tm']<0:raise ValueError('ineligible fee mix')
    # Initial contracts retain original mix and original prices.
    initial_mix=dict(mix,tm=mix['tm']+scenario['fixed_shift'],fixed=a['fixed_share'])
    backlog={k:a['initial_backlog']*v for k,v in initial_mix.items()}
    basevalue=sum(l['hours_per_job']*l['rate']*a['realization'] for l in lv)
    values={'fixed':backlog['fixed']*basevalue,'retainer':backlog['retainer']*a['retainer_fee'],
            'success':backlog['success']*a['success_fee']*a['success_probability']}
    hc=[l['count'] for l in lv]
    deposit=values['fixed']*a['fixed_deposit_share']
    queues={};out={};cumulative={}
    def through(name,amount,lag):
        if not lag:return amount
        q=queues.setdefault(name,[D(0)]*lag)
        due=q.pop(0);q.append(amount);return due
    def peek(name,lag):return queues.setdefault(name,[D(0)]*lag)[0]
    def metric(name,value,cum=False):
        out['m_'+name]=value
        if cum:
            cumulative[name]=cumulative.get(name,D(0))+value
            out['cum_'+name]=cumulative[name]
    def snapshot():
        out.update({f'l{i}_fte':v for i,v in enumerate(hc)})
        out.update({'backlog_'+f:v for f,v in backlog.items()})
        out.update({'value_'+f:v for f,v in values.items()})
        out['deposit_liability']=deposit
        out['receivables']=sum(sum(q) for key,q in queues.items() if key.startswith(('approval_','hold_','collect_')))
        return {k:float(v) for k,v in out.items()}
    # Initialize the exact public observation set independently of the generated model.
    for i in range(len(lv)):
        metric(f'fte_l{i}',D(0))
        for key in ('paid','admin','training','onboard','bd','idle','basepay','loaded'):
            metric(f'{key}_l{i}',D(0))
        for key in ('exit','starts','request','promote','delivery'):
            metric(f'{key}_l{i}',D(0),True)
    for key in ('prospects','won','delivery','bd_prospects','paid','bd_hours','delivery_hours','hourly_hours','hourly_standard'):
        metric(key,D(0),True)
    metric('fte',D(0))
    for fee in mix:
        for key in ('earned','disallowed','cash'):metric(key+'_'+fee,D(0),True)
    for key in ('earned','disallowed','revenue','collections','base_comp','loaded_comp','support_comp',
                'ai_cost','recruiting_cost','overhead','variable_cost','cost','ebitda','cash_flow','new_deposits','used_deposits'):
        metric(key,D(0),True)
    yield 0,snapshot()
    for t in range(c['months']):
        adopt=scenario['ai']*D(calendar['adoption'][t]);enabled=int(scenario['ai']>0)
        market=through('macro_delay',D(calendar['market'][t]),c['macro_lag'])
        market=market if market>0 else D(1)
        macro=max(D(0),1+scenario['macro']*a['macro_elasticity']*(market-1))
        bd_due=peek('bd_delay',c['bd_lag'])
        prospects=a['prospects']*scenario['demand_scale']*D(calendar['season'][t])*macro+bd_due
        won=prospects*a['win_rate'];new={f:won*mix[f] for f in mix}
        available={f:backlog[f]+new[f] for f in mix};work=sum(available.values())
        # Compute exits and promotions simultaneously from the prior committed population.
        exits=[hc[i]*(1-(1-l['attrition'])**(D(1)/12)) for i,l in enumerate(lv)]
        promotions=[(hc[i]-exits[i])*l['promotion'] for i,l in enumerate(lv)]
        starts=[];monthly=[];caps=[];tasks=[]
        for i,l in enumerate(lv):
            due=peek(f'l{i}_hire',c['hire_lag'])
            hire=due+(exits[i] if s['policy']=='hold' else 0);starts.append(hire)
            n=hc[i]-exits[i]-promotions[i]+(promotions[i-1] if i else 0)+hire
            paid=n*a['paid_hours'];admin=paid*l['admin_share']
            training=min(paid-admin,enabled*a['ai_training_hours']*(l['count']*D(calendar['initial'][t])+hire))
            onboard=min(paid-admin-training,due*a['paid_hours']*(1-a['onboard_productivity']))
            free=max(D(0),paid-admin-training-onboard)
            task=l['hours_per_job']*(1-adopt*l['exposure']*(1-a['ai_rework']));tasks.append(task)
            bd=scenario['bd']*l['bd_eligible']*a['bd_fraction']*max(D(0),free-work*task) if task else D(0)
            if task:caps.append((free-bd)/task)
            desired=(won+(work-won)/a['backlog_clear_months'])*task/(a['paid_hours']*min(a['target_utilization'],1-l['admin_share'])) if task else l['count']
            pending=sum(queues[f'l{i}_hire'][1:])
            request=min(a['recruitment_cap'],max(D(0),desired-n-pending)) if s['policy']=='responsive' else D(0)
            through(f'l{i}_hire',request,c['hire_lag'])
            base=n*l['annual_pay']/12;loaded=base*(1+a['employer_load']+a['bonus_share'])
            monthly.append(dict(fte=n,paid=paid,admin=admin,training=training,onboard=onboard,bd=bd,
                                free=free,basepay=base,loaded=loaded,request=request))
        delivery=min([work]+caps);fraction=delivery/work if work else D(0)
        done={f:available[f]*fraction for f in mix}
        backlog={f:max(D(0),available[f]-done[f]) for f in mix}
        hourly_value=sum(tasks[i]*l['rate']*a['realization'] for i,l in enumerate(lv))
        saving=1-hourly_value/basevalue
        fixed_price=basevalue*(1-scenario['erosion']*saving)
        price={'fixed':fixed_price,'retainer':a['retainer_fee'],
               'success':a['success_fee']*a['success_probability']*scenario['success_gate']}
        earned={'tm':done['tm']*hourly_value}
        for fee in values:
            value=values[fee]+new[fee]*price[fee]
            earned[fee]=value*fraction;values[fee]=max(D(0),value-earned[fee])
        deposit_in=new['fixed']*fixed_price*a['fixed_deposit_share']
        deposit_used=earned['fixed']*a['fixed_deposit_share']
        deposit=max(D(0),deposit+deposit_in-deposit_used)
        discounts={};cash={}
        for fee in mix:
            submitted=earned[fee]-(deposit_used if fee=='fixed' else 0)
            approved_gross=through('approval_'+fee,submitted,c['approval_lag'])
            discounts[fee]=approved_gross*a['hourly_disallowance'] if fee=='tm' else D(0)
            approved=approved_gross-discounts[fee];hold=a['hourly_holdback'] if fee=='tm' else D(0)
            release=through('hold_'+fee,approved*hold,c['holdback_lag'])
            cash[fee]=through('collect_'+fee,approved*(1-hold)+release,c['collection_lag'])
            for key,value in [('earned',earned[fee]),('disallowed',discounts[fee]),('cash',cash[fee])]:
                metric(key+'_'+fee,value,True)
        bd_hours=sum(x['bd'] for x in monthly)
        bd_generated=min(a['bd_max_jobs'],bd_hours*a['bd_jobs_per_hour'])
        through('bd_delay',bd_generated,c['bd_lag'])
        for i,row in enumerate(monthly):
            delivered=delivery*tasks[i]
            for key in ('fte','paid','admin','training','onboard','bd','basepay','loaded'):
                metric(f'{key}_l{i}',row[key])
            metric(f'delivery_l{i}',delivered,True)
            metric(f'idle_l{i}',max(D(0),row['free']-row['bd']-delivered))
            for key,value in [('exit',exits[i]),('starts',starts[i]),('request',row['request']),('promote',promotions[i])]:
                metric(f'{key}_l{i}',value,True)
        hc=[x['fte'] for x in monthly]
        delivery_hours=delivery*sum(tasks)
        for key,value in dict(prospects=prospects,won=won,delivery=delivery,bd_prospects=bd_generated,
                              paid=sum(x['paid'] for x in monthly),bd_hours=bd_hours,
                              delivery_hours=delivery_hours,hourly_hours=done['tm']*sum(tasks),
                              hourly_standard=done['tm']*sum(tasks[i]*l['rate'] for i,l in enumerate(lv))).items():
            metric(key,value,True)
        metric('fte',sum(hc))
        revenue=sum(earned.values())-sum(discounts.values())
        base=sum(x['basepay'] for x in monthly);loaded=sum(x['loaded'] for x in monthly)
        support=sum(x['loaded'] for i,x in enumerate(monthly) if not lv[i]['hours_per_job'])
        ai_cost=enabled*(sum(hc)*a['ai_license']+a['ai_training_cost']*(D(calendar['initial'][t])*sum(l['count'] for l in lv)+sum(starts)))
        recruiting=sum(starts)*a['hiring_cost']
        overhead=a['fixed_overhead']+a['variable_overhead']*max(D(0),revenue)
        variable=delivery_hours*a['variable_delivery_cost']
        cost=loaded+ai_cost+recruiting+overhead+variable
        collections=sum(cash.values())+deposit_in
        for key,value in dict(earned=sum(earned.values()),disallowed=sum(discounts.values()),revenue=revenue,
            collections=collections,base_comp=base,loaded_comp=loaded,support_comp=support,ai_cost=ai_cost,
            recruiting_cost=recruiting,overhead=overhead,variable_cost=variable,cost=cost,ebitda=revenue-cost,
            cash_flow=collections-cost,new_deposits=deposit_in,used_deposits=deposit_used).items():metric(key,value,True)
        yield t+1,snapshot()


def compare(actual,c,rows,calendar):
    count=0;worst=0.
    for sid,s in enumerate(rows):
        for month,expected in simulate(c,s,calendar):
            observed=actual[(sid,month)]
            if observed.keys()!=expected.keys():raise AssertionError(('output set',observed.keys()^expected.keys()))
            for key,value in expected.items():
                gap=abs(observed[key]-value);worst=max(worst,gap)
                if not math.isclose(observed[key],value,rel_tol=2e-10,abs_tol=2e-6):
                    raise AssertionError((s['name'],month,key,observed[key],value))
                count+=1
    if len(actual)!=(c['months']+1)*len(rows):raise AssertionError('unexpected observations')
    return dict(verdict='pass',comparisons=count,max_absolute_gap=worst,relative_tolerance=2e-10,absolute_tolerance=2e-6)
