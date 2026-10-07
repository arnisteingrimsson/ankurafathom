"""Run the synthetic T&R AI decision case through the existing native SD runtime."""
import argparse
import copy
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

import pyarrow as pa
import pyarrow.parquet as pq

from data import calibrate, generate, save
from oracle import compare, trajectory

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'examples/ankura_pilot'))
from generate import generate as generate_pilot, UNITS

ASSUMPTIONS = dict(variable_cost_per_hour=10., ai_license=40., ai_training=300.,
    acquisition_fte_share=.15, acquisition_pipeline_share=.20,
    acquisition_cost=2000000., integration_cost=20000., sales_cost_per_pipeline_hour=2.)


def invoke(exe, *args):
    result = subprocess.run([str(exe), *map(str,args)],capture_output=True,text=True)
    if result.returncode:
        raise RuntimeError(f'{args}: {result.stderr or result.stdout}')
    return result.stdout


def calibrated_model(root, c, assumptions):
    row = dict(name=c['practice'],fte=c['fte'],paid_hours_per_fte_month=c['paid_hours'],
        delivery_share=c['delivery_share'],realized_rate=c['tm_rate'],monthly_pipeline_hours=c['pipeline_hours'],
        win_rate=c['win_rate'],fixed_fee_share=c['fixed_share'],annual_pay_per_fte=c['annual_pay'],
        variable_cost_per_hour=assumptions['variable_cost_per_hour'],annual_demand_growth=0.,ai_task_share=1.,
        tool_task_share=0.,automation_task_share=0.,initial_tm_backlog_hours=0.,initial_fixed_backlog_hours=0.)
    # Intermediate CSV is the existing pilot generator's input contract; runtime inputs are Parquet.
    with (root/'calibrated-practice.csv').open('w',newline='') as stream:
        writer=csv.DictWriter(stream,fieldnames=['name',*UNITS]);writer.writeheader();writer.writerow(row)
    inputs = root/'inputs'
    generate_pilot(inputs,root/'calibrated-practice.csv')
    model = json.loads((inputs/'practice-0.ir.json').read_text())
    params = {p['id']:p for p in model['parameters']}
    for name in ('acquisition_fte_share','acquisition_pipeline_share','acquisition_cost','integration_cost'):
        params[name]['value'] = assumptions[name]
    params['copilot_license']['value'] = assumptions['ai_license']
    params['copilot_training']['value'] = assumptions['ai_training']
    model['parameters'] += [dict(id='fixed_contract_rate',value=c['fixed_rate'],unit='USD/hours'),
                            dict(id='pipeline_uplift',value=0.,unit='1'),
                            dict(id='sales_unit_cost',value=assumptions['sales_cost_per_pipeline_hour'],unit='USD/hours')]
    # The fee-specific estimate is file-bound with the rest of the calibrated parameters.
    table = pq.read_table(inputs/'practices.parquet')
    table = table.append_column('fixed_contract_rate',pa.array([c['fixed_rate']],type=pa.float64()))
    pq.write_table(table,inputs/'practices.parquet')
    binding = model['data'][0]
    binding['schema']['columns'].append(dict(name='fixed_contract_rate',type='f64',unit='USD/hours'))
    binding['use']['parameters'].append(dict(parameter='fixed_contract_rate',column='fixed_contract_rate'))
    components = {r['id']:r for r in model['components']}
    components['won_rate']['expr'] += ' * (1 + pipeline_uplift)'
    components['revenue_rate']['expr'] = 'tm_delivered_rate * labor_factor * realized_rate + fixed_delivered_rate * fixed_contract_rate'
    components['cost_rate']['expr'] += ' + monthly_pipeline_hours * growth(t) * pipeline_uplift * sales_unit_cost'
    next(r for r in model['checks'] if r['id']=='delivery_capacity')['expr'] = 'actual_delivery_rate <= paid_rate * delivery_share'
    calendar = pq.read_table(inputs/'calendar-0.parquet').to_pydict()
    calendar['growth'] = [c['seasonality'][m%12] for m in range(61)]
    pq.write_table(pa.table(calendar),inputs/'calendar-0.parquet')
    save(inputs/'practice-0.ir.json',model)
    # Remove ambiguity: the standard preview runner cannot summarize this custom scenario design.
    save(inputs/'pilot.json',dict(scope='AI decision custom model; use examples/ai_decision/run.py',
                                  calibration='../calibration.json',model='practice-0.ir.json'))
    return inputs, model


def scenarios(c):
    rows = []
    def add(name,reduction=0.,uplift=0.,fee=None,acquisition=False,kind='comparison'):
        rows.append(dict(id=len(rows),name=name,ai_reduction=reduction,pipeline_uplift=uplift,
                         fixed_share=c['fixed_share'] if fee is None else fee,acquisition=acquisition,kind=kind))
    add('baseline')
    add('acquisition',acquisition=True)
    for reduction in (.1,.2,.3):
        add(f'AI {reduction:.0%}',reduction)
        add(f'AI {reduction:.0%} + acquisition',reduction,acquisition=True)
        for fee in sorted(set((round(c['fixed_share'],10),.4,.6,1.))):
            for pct in range(61):
                add(f'AI {reduction:.0%}, fixed {fee:.0%}, pipeline +{pct}%',
                    reduction,pct/100,fee,kind='frontier')
        for pct in range(math.ceil(c['fixed_share']*100-1e-8),101):
            add(f'AI {reduction:.0%}, fee-mix sweep {pct}%',reduction,fee=pct/100,kind='fee_frontier')
    return rows


def experiment(rows):
    return dict(seed=0,replications=1,scenarios=[dict(id=r['id'],parameters=dict(
        enable_copilot=float(r['ai_reduction']>0),copilot_effect=r['ai_reduction'],
        enable_acquisition=float(r['acquisition']),pipeline_uplift=r['pipeline_uplift'],
        fixed_fee_share=r['fixed_share'])) for r in rows])


def native_run(exe, model, exp, output, threads):
    validation_path = output.with_suffix('.validation.json')
    invoke(exe,'check',model,'--experiment',exp,'--threads',threads,'--out',validation_path)
    validation=json.loads(validation_path.read_text())
    if validation['verdict'] != 'pass':
        raise AssertionError('native declared checks failed')
    invoke(exe,'run',model,'--experiment',exp,'--threads',threads,'--require-check','--out',output)
    manifest=json.loads(Path(str(output)+'.manifest.json').read_text())
    if validation['inputs'] != manifest['inputs'] or validation['result'] != manifest['result']:
        raise AssertionError('native validation/run identity mismatch')
    invoke(exe,'verify-results',output,'--embedded')
    actual={}
    for r in pq.read_table(output).select(['scenario','replication','time','output_id','value']).to_pylist():
        if r['replication'] != 0 or r['time'] != int(r['time']):
            raise AssertionError('unexpected replication or time')
        values=actual.setdefault((r['scenario'],int(r['time'])),{})
        if r['output_id'] in values:
            raise AssertionError('duplicate native output')
        values[r['output_id']]=r['value']
    return actual, dict(manifest_id=manifest['id'],rules=sum(x.get('samples',0) for x in validation['checks']))


def summarize(actual, rows):
    annual, totals = [], []
    for s in rows:
        sid=s['id']
        for year in range(1,6):
            before, after=actual[(sid,(year-1)*12)],actual[(sid,year*12)]
            values={k.removeprefix('cumulative_'):after[k]-before[k] for k in after if k.startswith('cumulative_')}
            annual.append(dict(scenario=sid,name=s['name'],year=year,revenue=values['revenue'],cost=values['cost'],
                contribution=values['profit'],margin=values['profit']/values['revenue'],
                utilization=values['actual_delivery']/values['paid_hours'],headcount=after['headcount'],
                backlog=after['tm_backlog']+after['fixed_backlog']))
        differences=[]
        for m in range(1,61):
            difference=(actual[(sid,m)]['cumulative_profit']-actual[(sid,m-1)]['cumulative_profit']
                        -actual[(0,m)]['cumulative_profit']+actual[(0,m-1)]['cumulative_profit'])
            differences.append(difference)
        balance=0.;deficit=False;balances=[];deficits=[]
        for delta in differences:
            balance+=delta;deficit|=balance<0;balances.append(balance);deficits.append(deficit)
        payback=next((i+1 for i,x in enumerate(balances) if deficits[i] and x>=0 and min(balances[i:])>=0),None)
        totals.append(dict(scenario=sid,name=s['name'],incremental_npv=sum(d/(1.1**((i+1)/12)) for i,d in enumerate(differences)),
                           sustained_payback_month=payback))
    return annual,totals


def frontier(annual, rows):
    by_key={(r['scenario'],r['year']):r for r in annual}
    groups={ (s['ai_reduction'],s['fixed_share']) for s in rows if s['kind']=='frontier'}
    result=[]
    for year in (1,2,5):
        base=by_key[(0,year)]
        for reduction,fee in sorted(groups):
            candidates=sorted((s for s in rows if s['kind']=='frontier' and
                               s['ai_reduction']==reduction and s['fixed_share']==fee),key=lambda s:s['pipeline_uplift'])
            passing=[s for s in candidates if by_key[(s['id'],year)]['revenue']>=base['revenue']-1e-6
                     and by_key[(s['id'],year)]['margin']>=base['margin']-1e-12]
            first=passing[0] if passing else None
            result.append(dict(year=year,ai_reduction=reduction,fixed_share=fee,
                minimum_tested_pipeline_uplift=first['pipeline_uplift'] if first else None,
                scenario=first['id'] if first else None,status='pass' if first else 'not_found_through_60_percent',
                grid_step=.01))
    return result


def fee_frontier(annual,rows):
    by_key={(r['scenario'],r['year']):r for r in annual}
    result=[]
    for year in (1,2,5):
        base=by_key[(0,year)]
        for reduction in (.1,.2,.3):
            candidates=sorted((s for s in rows if s['kind']=='fee_frontier' and s['ai_reduction']==reduction),
                              key=lambda s:s['fixed_share'])
            passing=[s for s in candidates if by_key[(s['id'],year)]['revenue']>=base['revenue']-1e-6
                     and by_key[(s['id'],year)]['margin']>=base['margin']-1e-12]
            first=passing[0] if passing else None
            result.append(dict(year=year,ai_reduction=reduction,pipeline_uplift=0.,
                minimum_tested_fixed_share=first['fixed_share'] if first else None,
                scenario=first['id'] if first else None,grid_step=.01,
                status='pass' if first else 'not_found_through_100_percent'))
    return result


def run(destination, executable, config=None, threads=8, assumption_overrides=None):
    assumptions = dict(ASSUMPTIONS, **(assumption_overrides or {}))
    if set(assumptions) != set(ASSUMPTIONS) or any(
            not isinstance(v,(int,float)) or not math.isfinite(v) or v < 0 for v in assumptions.values()):
        raise ValueError('scenario assumptions require known fields and finite nonnegative numbers')
    root=Path(destination).resolve();root.mkdir(parents=True,exist_ok=False)
    exe=Path(executable).resolve()
    print('Generating operational exports...',flush=True)
    generate(root/'data',config)
    c=calibrate(root/'data');save(root/'calibration.json',c)
    truth=json.loads((root/'data/truth.json').read_text())
    cfg=truth['config']
    targets=[(c['count_win_rates'],cfg['win_rates']),(c['seasonality'],cfg['seasonality']),
             ([c['realization']['TM'],c['realization']['Fixed']],[cfg['tm_realization'],cfg['fixed_realization']]),
             ([c['fixed_share'],c['pipeline_hours']],[cfg['fixed_share'],cfg['pipeline_hours']]),
             ([r['utilization'] for r in c['monthly']],[r['utilization'] for r in truth['monthly']])]
    for observed,expected in targets:
        if not all(math.isclose(x,y,rel_tol=1e-10,abs_tol=1e-10) for x,y in zip(observed,expected)):
            raise AssertionError('planted calibration recovery failed')
    save(root/'assumptions.json',dict(estimated='calibration.json',scenario_assumptions=assumptions,
        scope='Synthetic time-based T&R work only; not a representation of Ankura practice economics',
        conventions=dict(pipeline_uplift='step increase in baseline-hour opportunity flow, not annual growth',
            fixed_share='share of won baseline work, not revenue; contract values held fixed after AI',
            ai='full effect reached month 7; no automatic sales feedback or payroll reduction',
            acquisition='month 13; additions apply to workforce and opportunity flow; 18 integration months',
            margin='operating contribution / revenue; not GAAP margin',
            npv='operating-contribution proxy, 10% annual discount; no terminal value, financing, tax or cash collections'),
        not_modeled=['hiring/attrition policies','stochastic pipeline','court approvals and fee collections',
                     'success fees/retainers','CRO/interim leadership constraints','skill matching','AI quality or rework']))
    inputs, model=calibrated_model(root,c,assumptions)
    rows=scenarios(c);save(root/'scenario-catalog.json',rows)
    save(inputs/'scenarios.json',experiment(rows))
    print(f'Running {len(rows)} five-year native scenarios with declared checks...',flush=True)
    actual,receipt=native_run(exe,inputs/'practice-0.ir.json',inputs/'scenarios.json',root/'forecast.parquet',threads)
    proof=compare(actual,c,rows,assumptions)
    annual,totals=summarize(actual,rows)
    pq.write_table(pa.Table.from_pylist(annual),root/'annual.parquet')
    save(root/'summary.json',totals)
    thresholds=frontier(annual,rows);save(root/'thresholds.json',thresholds)
    fee_thresholds=fee_frontier(annual,rows);save(root/'fee-thresholds.json',fee_thresholds)
    # Historical reconstruction uses observed monthly won work, explicitly not an out-of-sample backtest.
    historical=copy.deepcopy(model);historical['time']['horizon']=36
    historical['name'] += ' historical reconstruction'
    calendar=pq.read_table(inputs/'calendar-0.parquet').to_pydict()
    calendar={k:v[:37] for k,v in calendar.items()}
    calendar['growth']=[r['won_hours']/(c['pipeline_hours']*c['win_rate']) for r in c['monthly']]
    calendar['growth'].append(calendar['growth'][-1])
    pq.write_table(pa.table(calendar),inputs/'historical-calendar.parquet')
    for b in historical['data']:
        if b['source']=='calendar-0.parquet':b['source']='historical-calendar.parquet'
    save(inputs/'historical.ir.json',historical);save(inputs/'historical-experiment.json',experiment(rows[:1]))
    print('Checking historical reconstruction and independent ledger...',flush=True)
    observed,hreceipt=native_run(exe,inputs/'historical.ir.json',inputs/'historical-experiment.json',root/'history.parquet',threads)
    history_proof=compare(observed,c,rows[:1],assumptions,36,True)
    for m,r in enumerate(c['monthly'],1):
        for metric,key in [('cumulative_revenue','revenue'),('cumulative_actual_delivery','billable_hours')]:
            delta=observed[(0,m)][metric]-observed[(0,m-1)][metric]
            if not math.isclose(delta,r[key],rel_tol=1e-10,abs_tol=2e-6):
                raise AssertionError('history/timesheet ledger mismatch')
    # Negative control: accounting identities can pass while T&M pricing is wrong.
    broken=copy.deepcopy(model)
    next(x for x in broken['components'] if x['id']=='revenue_rate')['expr']='tm_delivered_rate * realized_rate + fixed_delivered_rate * fixed_contract_rate'
    save(inputs/'wrong-tm.ir.json',broken);save(inputs/'control-experiment.json',experiment([rows[2]]))
    bad,_=native_run(exe,inputs/'wrong-tm.ir.json',inputs/'control-experiment.json',root/'wrong-tm.parquet',threads)
    mismatches=sum(not math.isclose(bad[(rows[2]['id'],m)]['cumulative_revenue'],e['cumulative_revenue'],abs_tol=2e-6)
                   for m,e in trajectory(c,rows[2],assumptions))
    if not mismatches:raise AssertionError('wrong T&M formula was not detected')
    print('Checking replay at one thread and preserving input bundle...',flush=True)
    replay=json.loads(invoke(exe,'replay',str(root/'forecast.parquet')+'.manifest.json','--threads',1))
    invoke(exe,'bundle',str(root/'forecast.parquet')+'.manifest.json','--out',root/'replay-bundle')
    bundled=json.loads(invoke(exe,'replay',root/'replay-bundle','--threads',threads))
    validation=dict(verdict='pass',synthetic=True,calibration_recovery_values=sum(len(x) for x,y in targets),
        forecast=proof,historical=history_proof,historical_transaction_comparisons=72,
        historical_scope='in-sample ledger reconstruction with observed monthly won work, not predictive validation',
        native_forecast=receipt,native_history=hreceipt,thread_replay=replay,bundle_replay=bundled,
        negative_control=dict(native_accounting='pass',independent_oracle='reject',wrong_tm_revenue_mismatches=mismatches),
        scripts={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in Path(__file__).parent.glob('*.py')})
    save(root/'validation.json',validation)
    # Human-readable plain-text result; data artifacts remain the authoritative numeric outputs.
    lines=[f'{c["practice"]}: synthetic AI decision test',
        'All figures are synthetic. Historical reconstruction is not an out-of-sample backtest.',
        f'Native scenarios: {len(rows)}. Independent forecast checks: {proof["comparisons"]:,}.',
        f'Estimated FTE {c["fte"]}; T&M realized rate ${c["tm_rate"]:.2f}; fixed baseline-hour value ${c["fixed_rate"]:.2f}.',
        f'Latest win rate {c["win_rate"]:.1%}; fixed work share {c["fixed_share"]:.1%}.',
        '', 'YEAR 1: scenario | revenue | operating contribution | margin | utilization']
    for r in annual:
        if r['year']==1 and rows[r['scenario']]['kind']=='comparison':
            lines.append(f'{r["name"]} | ${r["revenue"]:,.0f} | ${r["contribution"]:,.0f} | {r["margin"]:.2%} | {r["utilization"]:.2%}')
    lines += ['', 'MINIMUM TESTED PIPELINE UPLIFT preserving annual revenue AND margin',
              '1 percentage point grid, 0..60%; constant uplift, not annual compound growth.']
    for r in thresholds:
        value=r['minimum_tested_pipeline_uplift']
        lines.append(f'Year {r["year"]}, AI {r["ai_reduction"]:.0%}, fixed {r["fixed_share"]:.0%}: '+
                     (f'{value:.0%}' if value is not None else 'no passing tested point'))
    lines += ['', 'MINIMUM TESTED FIXED-FEE WORK SHARE with no pipeline uplift (1 point grid)']
    for r in fee_thresholds:
        value=r['minimum_tested_fixed_share']
        lines.append(f'Year {r["year"]}, AI {r["ai_reduction"]:.0%}: '+
                     (f'{value:.0%}' if value is not None else 'no passing tested point'))
    lines += ['', 'FIVE-YEAR OPERATING-CONTRIBUTION NPV PROXY vs baseline (10%, no terminal value)']
    for r in totals:
        if rows[r['scenario']]['kind']=='comparison':
            lines.append(f'{r["name"]}: ${r["incremental_npv"]:,.0f}; sustained payback month {r["sustained_payback_month"]}')
    (root/'results.txt').write_text('\n'.join(lines)+'\n')
    print(json.dumps(dict(output=str(root),verdict='pass',scenarios=len(rows),comparisons=proof['comparisons']),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination',type=Path)
    parser.add_argument('--executable',type=Path,default=ROOT/'build-arrow/fathom')
    parser.add_argument('--config',type=Path)
    parser.add_argument('--assumptions',type=Path,help='JSON overrides for synthetic intervention and acquisition costs')
    parser.add_argument('--threads',type=int,default=8)
    args=parser.parse_args()
    if args.threads<1:parser.error('threads must be positive')
    run(args.destination,args.executable,json.loads(args.config.read_text()) if args.config else None,args.threads,
        json.loads(args.assumptions.read_text()) if args.assumptions else None)
