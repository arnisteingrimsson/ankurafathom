"""Synthetic operational exports and a calibration adapter that never reads truth.json."""
from collections import Counter, defaultdict
from datetime import date, timedelta
import hashlib
import json
import math
from pathlib import Path
import random

import pyarrow as pa
import pyarrow.parquet as pq


DEFAULTS = dict(seed=20260930, practice='Synthetic T&R', fte=20, pipeline_hours=4000.,
                win_rates=[.30, .40, .50], fixed_share=.20,
                tm_realization=.90, fixed_realization=.98,
                seasonality=[1.05]*6 + [.75, .75] + [1.05]*4)


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')


def write_table(path, rows):
    pq.write_table(pa.Table.from_pylist(rows), path)


def generate(destination, overrides=None):
    """Exact stratified cohorts, randomized identity/order; not Monte Carlo draws."""
    cfg = dict(DEFAULTS, **(overrides or {}))
    if set(cfg) != set(DEFAULTS):
        raise ValueError('unknown generator configuration')
    if not isinstance(cfg['practice'], str) or not cfg['practice'].strip():
        raise ValueError('practice must be a nonempty name')
    if not isinstance(cfg['fte'], int) or cfg['fte'] <= 0 or cfg['fte'] % 2:
        raise ValueError('fte must be positive and even for the two-level fixture')
    if len(cfg['win_rates']) != 3 or len(cfg['seasonality']) != 12:
        raise ValueError('three annual win rates and twelve monthly factors required')
    if not all(math.isfinite(x) and 0 < x <= 1 for x in
               [*cfg['win_rates'], cfg['fixed_share'], cfg['tm_realization'], cfg['fixed_realization']]):
        raise ValueError('fractions must be finite in (0,1]')
    if cfg['fixed_share'] == 1 or not math.isfinite(cfg['pipeline_hours']) or cfg['pipeline_hours'] <= 0:
        raise ValueError('positive pipeline and both fee types required')
    if not all(math.isfinite(x) and x > 0 for x in cfg['seasonality']):
        raise ValueError('positive finite seasonal factors required')
    if not math.isclose(sum(cfg['seasonality']), 12, abs_tol=1e-12):
        raise ValueError('seasonal factors must average one')
    fixed_count = round(100 * cfg['fixed_share'])
    if not math.isclose(fixed_count, 100 * cfg['fixed_share']):
        raise ValueError('fixed share must give an integer count in 100 opportunities')
    for w in cfg['win_rates']:
        if w > .6 or any(not math.isclose(n*w, round(n*w)) for n in (fixed_count, 100-fixed_count)):
            raise ValueError('win rates need exact fee-stratified counts and must be <=60%')
    if cfg['pipeline_hours'] * max(cfg['win_rates']) * max(cfg['seasonality']) > cfg['fte']*160*.8:
        raise ValueError('history fixture requires enough capacity to deliver all won work')
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    rng = random.Random(cfg['seed'])
    roster = [dict(EmployeeId=f'E{i:03}', Practice=cfg['practice'],
                   Level='Consultant' if i < cfg['fte']//2 else 'Director',
                   HireDate='2022-01-01', TerminationDate='',
                   AnnualPay=90000. if i < cfg['fte']//2 else 150000.,
                   BillRate=200. if i < cfg['fte']//2 else 300.) for i in range(cfg['fte'])]
    accounts = [dict(Id=f'A{i:03}', Name=f'Synthetic account {i:03}') for i in range(100)]
    opportunities, history, projects, timesheets, availability = [], [], [], [], []
    truth_monthly = []
    for m in range(36):
        year, month = 2023 + m//12, m%12+1
        start = date(year, month, 1)
        following = date(year+month//12, month%12+1, 1)
        close = start
        # Service-week buckets begin on the 1st, 8th, 15th, 22nd and (if present) 29th.
        # They are month-contained accounting periods, not ISO/calendar weeks.
        weeks = [start+timedelta(days=d) for d in range(0, (following-start).days, 7)]
        hours = cfg['pipeline_hours'] * cfg['seasonality'][m%12] / 100
        labels = [('Fixed', i < round(fixed_count*cfg['win_rates'][m//12])) for i in range(fixed_count)]
        labels += [('TM', i < round((100-fixed_count)*cfg['win_rates'][m//12])) for i in range(100-fixed_count)]
        rng.shuffle(labels)
        # All winners reach proposal; losses exercise early-stage exit paths.
        lost_index = 0
        for j, (fee, won) in enumerate(labels):
            oid = f'O{m:02}-{j:03}'
            terminal = 'Closed Won' if won else 'Closed Lost'
            realization = cfg['tm_realization'] if fee == 'TM' else cfg['fixed_realization']
            opportunities.append(dict(Id=oid, AccountId=accounts[j]['Id'], Practice=cfg['practice'],
                StageName=terminal, Amount=hours*250*realization, CloseDate=close.isoformat(),
                CreatedDate=(start-timedelta(days=30)).isoformat(), FeeType=fee,
                Probability=100. if won else 0., BaselineHours=hours))
            depth = 3 if won else (1 if lost_index < 20 else 2 if lost_index < 40 else 3)
            if not won:
                lost_index += 1
            stages = ['Prospecting', 'Qualified', 'Proposal'][:depth]
            for k, stage in enumerate(stages):
                history.append(dict(Id=f'{oid}-H{k}', OpportunityId=oid, StageName=stage,
                                    ChangedDate=(start-timedelta(days=30-10*k)).isoformat()))
            history.append(dict(Id=f'{oid}-HC', OpportunityId=oid, StageName=terminal, ChangedDate=close.isoformat()))
            if won:
                pid = f'P{oid}'
                projects.append(dict(ProjectId=pid, OpportunityId=oid, FeeType=fee,
                    BaselineHours=hours, ContractValue=hours*250*realization, ServiceMonth=start.isoformat()))
                for e in roster:
                    for week in weeks:
                        delivered = hours / len(roster) / len(weeks)
                        timesheets.append(dict(Id=f'{pid}-{e["EmployeeId"]}-{week}', EmployeeId=e['EmployeeId'],
                            ProjectId=pid, PeriodStart=week.isoformat(), Month=start.isoformat(),
                            Hours=delivered, Billable=True, BillRate=e['BillRate'],
                            CostRate=e['AnnualPay']/1920,
                            RecognizedAmount=delivered*e['BillRate']*realization))
        won_hours = hours * round(100*cfg['win_rates'][m//12])
        for e in roster:
            for week in weeks:
                paid = 160 / len(weeks)
                available = dict(Id=f'{e["EmployeeId"]}-{week}', EmployeeId=e['EmployeeId'],
                    PeriodStart=week.isoformat(), Month=start.isoformat(), PaidHours=paid,
                    DeliveryCapacityHours=.8*paid)
                availability.append(available)
                timesheets.append(dict(Id=f'NB-{available["Id"]}', EmployeeId=e['EmployeeId'], ProjectId='',
                    PeriodStart=week.isoformat(), Month=start.isoformat(), Hours=paid-won_hours/len(roster)/len(weeks),
                    Billable=False, BillRate=e['BillRate'], CostRate=e['AnnualPay']/1920, RecognizedAmount=0.))
        truth_monthly.append(dict(month=start.isoformat(), win_rate=cfg['win_rates'][m//12],
            utilization=won_hours/(cfg['fte']*160), won_hours=won_hours))
    # Open opportunities are censored at the export date, not counted as lost.
    for j in range(20):
        oid = f'OPEN{j:03}'
        opportunities.append(dict(Id=oid, AccountId=accounts[j]['Id'], Practice=cfg['practice'],
            StageName='Qualified', Amount=10000., CloseDate='2026-03-01', CreatedDate='2025-12-01',
            FeeType='TM', Probability=30., BaselineHours=40.))
        for k, stage in enumerate(('Prospecting', 'Qualified')):
            history.append(dict(Id=f'{oid}-H{k}', OpportunityId=oid, StageName=stage,
                                ChangedDate=f'2025-12-{1+k*10:02}'))
    tables = dict(accounts=accounts, roster=roster, opportunities=opportunities,
                  opportunity_history=history, projects=projects, timesheets=timesheets, availability=availability)
    for name, rows in tables.items():
        rng.shuffle(rows)
        write_table(destination/f'{name}.parquet', rows)
    save(destination/'truth.json', dict(config=cfg, monthly=truth_monthly,
        tm_rate=250*cfg['tm_realization'], fixed_rate=250*cfg['fixed_realization']))
    save(destination/'export.json', dict(as_of='2025-12-31', synthetic=True,
        row_counts={k:len(v) for k,v in tables.items()}, seed=cfg['seed']))


def calibrate(source):
    """Aggregate source exports only; fail on broken identities and inconsistent ledgers."""
    source = Path(source)
    keys = dict(accounts='Id', roster='EmployeeId', opportunities='Id', opportunity_history='Id',
                projects='ProjectId', timesheets='Id', availability='Id')
    data = {name:sorted(pq.read_table(source/f'{name}.parquet').to_pylist(), key=lambda r:r[key])
            for name,key in keys.items()}
    for name, rows in data.items():
        if not rows or len({r[keys[name]] for r in rows}) != len(rows):
            raise ValueError(f'empty table or duplicate key: {name}')
        for row in rows:
            if any(v is None or isinstance(v,float) and not math.isfinite(v) for v in row.values()):
                raise ValueError(f'null/nonfinite field: {name}')
    employees = {r['EmployeeId']:r for r in data['roster']}
    accounts = {r['Id'] for r in data['accounts']}
    opps = {r['Id']:r for r in data['opportunities']}
    if len({r['Practice'] for r in [*employees.values(),*opps.values()]}) != 1:
        raise ValueError('this adapter requires one consistent practice')
    projects = {r['ProjectId']:r for r in data['projects']}
    as_of = json.loads((source/'export.json').read_text())['as_of']
    if any(r['HireDate'] > '2023-01-01' or r['TerminationDate'] for r in employees.values()):
        raise ValueError('this calibration fixture requires a stable three-year workforce')
    if any(r['AnnualPay'] <= 0 or r['BillRate'] <= 0 for r in employees.values()):
        raise ValueError('positive pay and rates required')
    closed = [r for r in opps.values() if r['StageName'] in ('Closed Won','Closed Lost') and r['CloseDate'] <= as_of]
    months = sorted({r['CloseDate'][:7]+'-01' for r in closed})
    if months != [f'{2023+i//12}-{i%12+1:02}-01' for i in range(36)]:
        raise ValueError('expected complete 2023-2025 historical window')
    for o in opps.values():
        if o['AccountId'] not in accounts or o['FeeType'] not in ('TM','Fixed') or o['BaselineHours'] <= 0:
            raise ValueError('invalid opportunity account, fee type or baseline hours')
        if not 0 <= o['Probability'] <= 100 or o['Amount'] < 0 or o['CreatedDate'] > o['CloseDate']:
            raise ValueError('invalid opportunity values or dates')
    won_ids = {r['Id'] for r in closed if r['StageName']=='Closed Won'}
    closed_ids = {r['Id'] for r in closed}
    if {r['OpportunityId'] for r in projects.values()} != won_ids or len(projects) != len(won_ids):
        raise ValueError('closed-won opportunities must map one-to-one to projects')
    for p in projects.values():
        o = opps[p['OpportunityId']]
        if p['FeeType'] != o['FeeType'] or p['BaselineHours'] != o['BaselineHours'] or p['ContractValue'] != o['Amount']:
            raise ValueError('opportunity/project contract mismatch')
    histories = defaultdict(list)
    for h in data['opportunity_history']:
        if h['OpportunityId'] not in opps:
            raise ValueError('orphan stage history')
        histories[h['OpportunityId']].append(h)
    transitions = Counter()
    stage_visits = Counter()
    dwell = defaultdict(list)
    for oid, o in opps.items():
        rows = sorted(histories[oid],key=lambda h:h['ChangedDate'])
        if not rows or rows[-1]['StageName'] != o['StageName']:
            raise ValueError('stage history does not reconcile to current stage')
        if rows[0]['ChangedDate'] != o['CreatedDate'] or rows[-1]['ChangedDate'] > as_of:
            raise ValueError('stage history outside export bounds')
        if oid not in closed_ids:
            continue
        if rows[-1]['ChangedDate'] != o['CloseDate']:
            raise ValueError('terminal stage/close date mismatch')
        for a,b in zip(rows,rows[1:]):
            stage_visits[a['StageName']] += 1
            transitions[(a['StageName'],b['StageName'])] += 1
            dwell[a['StageName']].append((date.fromisoformat(b['ChangedDate'])-date.fromisoformat(a['ChangedDate'])).days)
    available = {(r['EmployeeId'],r['PeriodStart']):r for r in data['availability']}
    if len(available) != len(data['availability']):
        raise ValueError('duplicate employee period availability')
    monthly = {m:dict(month=m, paid_hours=0., capacity_hours=0., billable_hours=0., revenue=0.,
                     pipeline_hours=0., won_hours=0., fixed_won_hours=0., closed=0, won=0) for m in months}
    for a in available.values():
        if a['EmployeeId'] not in employees or a['Month'] not in monthly or not 0 <= a['DeliveryCapacityHours'] <= a['PaidHours']:
            raise ValueError('invalid availability')
        monthly[a['Month']]['paid_hours'] += a['PaidHours']
        monthly[a['Month']]['capacity_hours'] += a['DeliveryCapacityHours']
    project_hours, project_revenue, period_hours, period_billable = defaultdict(float), defaultdict(float), defaultdict(float), defaultdict(float)
    fee_hours, fee_gross, fee_revenue = defaultdict(float), defaultdict(float), defaultdict(float)
    for t in data['timesheets']:
        key = (t['EmployeeId'],t['PeriodStart'])
        if key not in available or t['Month'] != available[key]['Month'] or t['Hours'] < 0:
            raise ValueError('invalid timesheet identity, period or hours')
        e = employees[t['EmployeeId']]
        if t['BillRate'] != e['BillRate'] or not math.isclose(t['CostRate'], e['AnnualPay']/1920):
            raise ValueError('employee/time rate mismatch')
        period_hours[key] += t['Hours']
        if t['Billable']:
            if t['ProjectId'] not in projects or t['RecognizedAmount'] < 0:
                raise ValueError('orphan project or negative revenue')
            p = projects[t['ProjectId']]
            if t['Month'] != p['ServiceMonth']:
                raise ValueError('project service-month mismatch')
            fee = p['FeeType']
            project_hours[t['ProjectId']] += t['Hours']
            project_revenue[t['ProjectId']] += t['RecognizedAmount']
            period_billable[key] += t['Hours']
            fee_hours[fee] += t['Hours']; fee_gross[fee] += t['Hours']*t['BillRate']
            fee_revenue[fee] += t['RecognizedAmount']
            monthly[t['Month']]['billable_hours'] += t['Hours']
            monthly[t['Month']]['revenue'] += t['RecognizedAmount']
        elif t['ProjectId'] or t['RecognizedAmount'] != 0:
            raise ValueError('nonbillable entry carries project revenue')
    for key, a in available.items():
        if not math.isclose(period_hours[key],a['PaidHours'],abs_tol=1e-8) or period_billable[key] > a['DeliveryCapacityHours']+1e-8:
            raise ValueError('timesheet/availability hours do not reconcile')
    for pid, p in projects.items():
        if not math.isclose(project_hours[pid],p['BaselineHours'],abs_tol=1e-8) or not math.isclose(project_revenue[pid],p['ContractValue'],abs_tol=1e-7):
            raise ValueError('project delivery/recognized revenue mismatch')
    for o in closed:
        row = monthly[o['CloseDate'][:7]+'-01']
        row['closed'] += 1; row['pipeline_hours'] += o['BaselineHours']
        if o['StageName'] == 'Closed Won':
            row['won'] += 1; row['won_hours'] += o['BaselineHours']
            row['fixed_won_hours'] += o['BaselineHours']*(o['FeeType']=='Fixed')
    for row in monthly.values():
        row['utilization'] = row['billable_hours']/row['paid_hours']
    annual_wins = [sum(r['won'] for m,r in monthly.items() if m.startswith(str(y))) /
                   sum(r['closed'] for m,r in monthly.items() if m.startswith(str(y))) for y in range(2023,2026)]
    recent = list(monthly.values())[-12:]
    avg_pipeline = sum(r['pipeline_hours'] for r in recent)/12
    avg_won = sum(r['won_hours'] for r in recent)/12
    fte = len(employees)
    return dict(scope='synthetic aggregate calibration; no truth input; stable workforce; all historical work completed',
        source_hashes={name:hashlib.sha256((source/f'{name}.parquet').read_bytes()).hexdigest() for name in keys},
        export_sha256=hashlib.sha256((source/'export.json').read_bytes()).hexdigest(),
        practice=next(iter(employees.values()))['Practice'],
        fte=fte, annual_pay=sum(r['AnnualPay'] for r in employees.values())/fte,
        paid_hours=sum(r['paid_hours'] for r in recent)/12/fte,
        delivery_share=sum(r['capacity_hours'] for r in recent)/sum(r['paid_hours'] for r in recent),
        pipeline_hours=avg_pipeline, win_rate=avg_won/avg_pipeline, count_win_rates=annual_wins,
        fixed_share=sum(r['fixed_won_hours'] for r in recent)/sum(r['won_hours'] for r in recent),
        tm_rate=fee_revenue['TM']/fee_hours['TM'], fixed_rate=fee_revenue['Fixed']/fee_hours['Fixed'],
        realization={fee:fee_revenue[fee]/fee_gross[fee] for fee in ('TM','Fixed')},
        seasonality=[r['pipeline_hours']/avg_pipeline for r in recent], monthly=list(monthly.values()),
        open_opportunities_excluded=len(opps)-len(closed),
        stage_transitions=[dict(source=a,destination=b,count=n,probability=n/stage_visits[a])
                           for (a,b),n in sorted(transitions.items())],
        mean_days_to_next_stage={s:sum(v)/len(v) for s,v in dwell.items()})
