"""Independent decimal ledger; does not read IR expressions or native results."""
from decimal import Decimal as D
import math


def decimal(value):
    # Calibration aggregation creates harmless binary noise; preserve 12 significant figures.
    return D(format(value, '.12g'))


def trajectory(calibration, scenario, assumptions, months=60, historical=False, wrong_tm=False):
    c, s, a = calibration, scenario, assumptions
    fte, paid, wage = map(decimal, (c['fte'], c['paid_hours'], c['annual_pay']))
    rate_tm, rate_fixed = map(decimal, (c['tm_rate'], c['fixed_rate']))
    fee = decimal(s['fixed_share'])
    tm, fixed = D(0), D(0)
    totals = dict.fromkeys(('revenue','cost','profit','actual_delivery','paid_hours','tm_won',
                          'fixed_won','tm_delivered','fixed_delivered'), D(0))
    yield 0, dict(headcount=float(fte), tm_backlog=0., fixed_backlog=0.,
                  **{'cumulative_'+k:float(v) for k,v in totals.items()})
    for m in range(1,months+1):
        acquired = s['acquisition'] and m >= 13
        workforce = fte * (1 + decimal(a['acquisition_fte_share']) if acquired else 1)
        seasonal = decimal(c['seasonality'][(m-1)%12])
        if historical:
            demand = decimal(c['monthly'][m-1]['won_hours'])
        else:
            demand = decimal(c['pipeline_hours'])*decimal(c['win_rate'])*seasonal*(1+decimal(s['pipeline_uplift']))
            if acquired:
                demand *= 1+decimal(a['acquisition_pipeline_share'])
        labor = 1-decimal(s['ai_reduction'])*D(min(m-1,6))/6
        incoming_fixed, incoming_tm = demand*fee, demand*(1-fee)
        tm += incoming_tm; fixed += incoming_fixed
        # Allocate baseline throughput directly, not the IR's share-delivered expression.
        total_work = tm + fixed
        delivered = min(total_work, workforce*paid*decimal(c['delivery_share'])/labor)
        fixed_done = delivered*fixed/total_work if total_work else D(0)
        tm_done = delivered-fixed_done
        fixed -= fixed_done; tm -= tm_done
        hours = delivered*labor
        revenue = tm_done*(1 if wrong_tm else labor)*rate_tm+fixed_done*rate_fixed
        expense = workforce*wage/12 + hours*decimal(a['variable_cost_per_hour'])
        if s['ai_reduction']:
            expense += workforce*decimal(a['ai_license'])
            if m == 1:
                expense += fte*decimal(a['ai_training'])
        if acquired:
            if m == 13:
                expense += decimal(a['acquisition_cost'])
            if m < 31:
                expense += decimal(a['integration_cost'])
        expense += decimal(c['pipeline_hours'])*seasonal*decimal(s['pipeline_uplift'])*decimal(a['sales_cost_per_pipeline_hour'])
        values = dict(revenue=revenue,cost=expense,profit=revenue-expense,actual_delivery=hours,
                      paid_hours=workforce*paid,tm_won=incoming_tm,fixed_won=incoming_fixed,
                      tm_delivered=tm_done,fixed_delivered=fixed_done)
        for key, value in values.items():
            totals[key] += value
        yield m, dict(headcount=float(workforce),tm_backlog=float(tm),fixed_backlog=float(fixed),
                      **{'cumulative_'+k:float(v) for k,v in totals.items()})


def compare(actual, c, scenarios, assumptions, months=60, historical=False):
    comparisons, worst = 0, 0.
    for s in scenarios:
        for m, expected in trajectory(c,s,assumptions,months,historical):
            observed = actual[(s['id'],m)]
            if observed.keys() != expected.keys():
                raise AssertionError('oracle/native output names differ')
            for key, target in expected.items():
                value = observed[key]
                worst = max(worst, abs(value-target))
                if not math.isclose(value,target,rel_tol=2e-10,abs_tol=2e-6):
                    raise AssertionError((s['id'],m,key,value,target))
                comparisons += 1
    if len(actual) != len(scenarios)*(months+1):
        raise AssertionError('unexpected scenario/time observations')
    return dict(verdict='pass',comparisons=comparisons,max_absolute_gap=worst,
                absolute_tolerance=2e-6,relative_tolerance=2e-10)
