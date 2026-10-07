"""Versioned, explicitly synthetic T&R assumptions; peer data is a comparison only."""
import copy
import math


def default_config():
    counts = [18, 27, 30, 24, 18, 12, 6, 15]
    names = ['analyst', 'consultant', 'senior_consultant', 'manager', 'director',
             'senior_director', 'managing_director', 'support']
    rates = [250, 350, 450, 600, 800, 1100, 1400, 0]
    pay = [85000, 110000, 145000, 185000, 240000, 320000, 450000, 80000]
    hours = [30, 60, 60, 45, 30, 20, 8, 0]
    admin = [.20, .20, .20, .25, .30, .35, .40, 1.]
    exposure = [.8, .7, .6, .4, .2, .1, 0., 0.]
    return dict(version=1, months=60, levels=[dict(id=n, count=counts[i], rate=rates[i],
        annual_pay=pay[i], hours_per_job=hours[i], admin_share=admin[i], exposure=exposure[i],
        attrition=.10, promotion=0., bd_eligible=float(i in (5, 6))) for i,n in enumerate(names)],
        paid_hours=160., employer_load=.25, bonus_share=.20, fixed_overhead=500000.,
        variable_overhead=.05, variable_delivery_cost=10., realization=.90,
        prospects=100., win_rate=.50, initial_backlog=0., hire_lag=2, onboard_productivity=.5,
        hiring_cost=10000., target_utilization=.70, backlog_clear_months=6.,
        ai_license=80., ai_training_cost=500., ai_training_hours=8., ai_rework=.05,
        bd_fraction=.25, bd_jobs_per_hour=.01, bd_lag=3, bd_max_jobs=15.,
        fixed_eligible_share=.25, fixed_share=.15, retainer_share=.15, success_share=.05,
        retainer_fee=110000., success_fee=200000., success_probability=.50,
        fixed_deposit_share=.20, hourly_disallowance=.02, hourly_holdback=.20,
        approval_lag=1, collection_lag=1, holdback_lag=3,
        macro_elasticity=.5, macro_lag=1, recruitment_cap=3.)


def validate(c):
    known = default_config()
    if set(c) != set(known) or c['version'] != 1:
        raise ValueError('unknown/missing configuration fields or version')
    for key,value in c.items():
        if key == 'levels': continue
        if not isinstance(value,(int,float)) or isinstance(value,bool) or not math.isfinite(value) or value < 0:
            raise ValueError(f'invalid numeric configuration: {key}')
    for key in ('months','hire_lag','bd_lag','approval_lag','collection_lag','holdback_lag','macro_lag'):
        if not isinstance(c[key],int) or not 0 <= c[key] <= (120 if key=='months' else 12):
            raise ValueError(f'invalid integer horizon/lag: {key}')
    if c['months']<1 or c['hire_lag']<1 or c['bd_lag']<1:
        raise ValueError('positive horizon, hiring lag and BD lag required')
    for key in ('paid_hours','backlog_clear_months','target_utilization'):
        if c[key] <= 0: raise ValueError(f'{key} must be positive')
    fractions=('employer_load','bonus_share','variable_overhead','realization','win_rate',
               'onboard_productivity','target_utilization','ai_rework','bd_fraction','fixed_eligible_share',
               'fixed_share','retainer_share','success_share','success_probability','fixed_deposit_share',
               'hourly_disallowance','hourly_holdback')
    if any(c[k]>1 for k in fractions): raise ValueError('fraction exceeds one')
    if c['fixed_share']>c['fixed_eligible_share'] or sum(c[k] for k in ('fixed_share','retainer_share','success_share'))>1:
        raise ValueError('infeasible contract mix')
    if not c['levels'] or len(c['levels'])>16:
        raise ValueError('one to sixteen levels required')
    ids=set()
    for i,l in enumerate(c['levels']):
        if set(l)!=set(known['levels'][0]): raise ValueError('unknown/missing level fields')
        name=l['id']
        if not isinstance(name,str) or not name.isidentifier() or not name.isascii() or name in ids:
            raise ValueError('invalid/duplicate level id')
        ids.add(name)
        if any(not isinstance(v,(int,float)) or isinstance(v,bool) or not math.isfinite(v) or v<0 for k,v in l.items() if k!='id'):
            raise ValueError('invalid level number')
        if not isinstance(l['count'],int): raise ValueError('initial employee count must be integer')
        if l['attrition']>=1 or any(l[k]>1 for k in ('admin_share','exposure','promotion','bd_eligible')):
            raise ValueError('invalid level fraction')
        if l['promotion'] and (i==len(c['levels'])-1 or l['hours_per_job']==0 or c['levels'][i+1]['hours_per_job']==0):
            raise ValueError('promotions require an adjacent billable destination')
        if l['hours_per_job'] and l['admin_share']>=1:
            raise ValueError('required skill has no possible delivery time')
        if not l['hours_per_job'] and (l['rate'] or l['exposure'] or l['bd_eligible']):
            raise ValueError('support roles cannot bill or supply BD in this subset')
    if not any(l['hours_per_job'] for l in c['levels']): raise ValueError('no delivery levels')
    if sum(l['hours_per_job']*l['rate']*c['realization'] for l in c['levels'])<=0:
        raise ValueError('positive standard engagement value required')
    return c


def scenario(name='baseline', **overrides):
    s=dict(name=name,ai=0.,policy='hold',erosion=0.,bd=0.,macro=0.,fixed_shift=0.,
           demand_scale=1.,success_gate=1.)
    if set(overrides)-set(s): raise ValueError('unknown scenario field')
    s.update(overrides)
    if s['policy'] not in ('hold','freeze','responsive'): raise ValueError('unknown workforce policy')
    if any(not isinstance(s[k],(int,float)) or not math.isfinite(s[k]) or not 0<=s[k]<=1
           for k in ('ai','erosion','bd','macro','fixed_shift','success_gate')):
        raise ValueError('scenario fraction outside [0,1]')
    if not math.isfinite(s['demand_scale']) or s['demand_scale']<0:
        raise ValueError('invalid demand scale')
    return s


def scenarios():
    rows=[scenario()]
    for ai in (.2,.4):
        for policy in ('hold','freeze','responsive'):
            for erosion in (0.,.5,1.):
                rows.append(scenario(f'ai{ai:.0%}_{policy}_erosion{erosion:.0%}',ai=ai,policy=policy,erosion=erosion))
        rows += [scenario(f'ai{ai:.0%}_responsive_bd',ai=ai,policy='responsive',erosion=.5,bd=1.),
                 scenario(f'ai{ai:.0%}_responsive_bd_macro',ai=ai,policy='responsive',erosion=.5,bd=1.,macro=1.),
                 scenario(f'ai{ai:.0%}_eligible_fixed',ai=ai,policy='responsive',erosion=.5,fixed_shift=.10)]
    rows += [scenario('baseline_macro',macro=1.), scenario('demand_down',demand_scale=.75),
             scenario('ai20_demand_down',ai=.2,policy='responsive',demand_scale=.75,erosion=.5),
             scenario('no_success_fee',success_gate=0.)]
    rows += [scenario('baseline_responsive',policy='responsive'),
             scenario('baseline_freeze',policy='freeze'),
             scenario('baseline_responsive_bd',policy='responsive',bd=1.),
             scenario('ai20_pipeline20',ai=.2,policy='responsive',erosion=.5,demand_scale=1.2),
             scenario('ai20_pipeline40',ai=.2,policy='responsive',erosion=.5,demand_scale=1.4)]
    return rows


def merged_config(overrides=None):
    return validate(dict(default_config(), **copy.deepcopy(overrides or {})))
