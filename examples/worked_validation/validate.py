"""Worked examples checked against exact arithmetic, without diagrams/reporting.

The pricing cases use the production pilot generator. Expected values use known
binding/slack regimes and Fraction arithmetic, never the generator's expressions
or the separate C++ economics implementation. Staffing expectations come from an
explicit hand-scheduled event ledger.
"""
import argparse
import copy
import csv
from fractions import Fraction as F
import json
import math
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
ABS_TOL = 2e-8
REL_TOL = 1e-12


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def invoke(executable, *args):
    p = subprocess.run([str(executable), *map(str, args)], capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f'{args}: {p.stderr or p.stdout}')
    return p.stdout


def observations(path):
    rows = {}
    with path.open(newline='') as stream:
        for row in csv.DictReader(stream):
            key = (int(row.get('scenario', 0)), F(row['time']), row['output_id'])
            if key in rows:
                raise AssertionError(f'duplicate observation: {key}')
            rows[key] = float(row['value'])
    return rows


def compare(actual, expected):
    if actual.keys() != expected.keys():
        raise AssertionError('observation addresses differ from the independent oracle')
    differences = []
    worst = 0.
    for key, exact in expected.items():
        value = actual[key]; target = float(exact); gap = abs(value - target)
        worst = max(worst, gap)
        if not math.isclose(value, target, rel_tol=REL_TOL, abs_tol=ABS_TOL):
            differences.append(dict(scenario=key[0], time=str(key[1]), output=key[2],
                                    actual=value, expected=target, expected_rational=str(exact)))
    return dict(comparisons=len(expected), max_absolute_gap=worst, failures=differences)


def pricing_expected(fixed, saturated):
    expected = {}; monthly = {}
    for scenario in (0, 1):
        sums = {name: F(0) for name in ('revenue', 'cost', 'profit', 'actual_delivery',
                'paid_hours', 'tm_won', 'fixed_won', 'tm_delivered', 'fixed_delivered')}
        for month in range(13):
            if month:
                # Month 1 has no adoption, months 2..7 ramp by 1/6 to an 8% saving.
                adoption = F(min(month - 1, 6), 6)
                labor = 1 - scenario * F(2, 25) * adoption
                # These examples stay strictly in their known regime all year.
                work = F(160) / labor if saturated else F(100)
                hours = F(160) if saturated else F(100) * labor
                assert (work < 200 if saturated else hours <= 160)
                revenue = 200 * (work if fixed else hours)
                cost = 10000 + 10 * hours + scenario * (40 + (300 if month == 1 else 0))
                won = F(200 if saturated else 100)
                values = dict(revenue=revenue, cost=cost, profit=revenue-cost,
                    actual_delivery=hours, paid_hours=F(160), tm_won=0 if fixed else won,
                    fixed_won=won if fixed else 0, tm_delivered=0 if fixed else work,
                    fixed_delivered=work if fixed else 0)
                monthly[(scenario, month)] = values
                for name, value in values.items():
                    sums[name] += value
            for name, value in sums.items():
                expected[(scenario, F(month), 'cumulative_' + name)] = value
            expected[(scenario, F(month), 'headcount')] = F(1)
            for fee in ('tm', 'fixed'):
                expected[(scenario, F(month), fee + '_backlog')] = sums[fee+'_won'] - sums[fee+'_delivered']
    return expected, monthly


def pricing(executable, directory):
    sys.path.insert(0, str(ROOT/'examples/ankura_pilot'))
    from generate import generate, UNITS
    directory.mkdir()
    cases = [('tm_spare', False, False), ('tm_full', False, True),
             ('fixed_spare', True, False), ('fixed_full', True, True)]
    source = directory/'practices.csv'
    with source.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=['name', *UNITS]); writer.writeheader()
        for name, fixed, saturated in cases:
            writer.writerow(dict(name=name, fte=1, paid_hours_per_fte_month=160,
                delivery_share=1, realized_rate=200, monthly_pipeline_hours=200 if saturated else 100,
                win_rate=1, fixed_fee_share=int(fixed), annual_pay_per_fte=120000,
                variable_cost_per_hour=10, annual_demand_growth=0, ai_task_share=1,
                tool_task_share=0, automation_task_share=0,
                initial_tm_backlog_hours=0, initial_fixed_backlog_hours=0))
    inputs = directory/'inputs'; plan = generate(inputs, source)
    experiment = json.loads((inputs/'scenarios.json').read_text())
    experiment['scenarios'] = experiment['scenarios'][:2]
    save(inputs/'scenarios.json', experiment)
    # This is a controlled 12-month specialization of the production equations.
    plan['scenario_names'] = {'0': 'baseline', '1': 'copilot'}
    plan['scope'] = 'worked one-year pricing examples; synthetic hand-calculation inputs'
    save(inputs/'pilot.json', plan)
    results = []; oracle_detection = []
    for index, (name, fixed, saturated) in enumerate(cases):
        path = inputs/plan['models'][index]['model']
        model = json.loads(path.read_text()); model['time']['horizon'] = 12; save(path, model)
        args = ['--experiment', inputs/'scenarios.json', '--threads', '2']
        validation = directory/(name+'.validation.json')
        invoke(executable, 'check', path, *args, '--out', validation)
        check = json.loads(validation.read_text()); assert check['verdict'] == 'pass'
        output = directory/(name+'.csv')
        invoke(executable, 'run', path, *args, '--out', output)
        manifest = json.loads(Path(str(output)+'.manifest.json').read_text())
        assert check['inputs'] == manifest['inputs'] and check['result'] == manifest['result']
        actual = observations(output); expected, monthly = pricing_expected(fixed, saturated)
        verdict = compare(actual, expected); assert not verdict['failures'], (name, verdict)
        samples = []
        for scenario in (0, 1):
            snapshot = {}
            for metric in ('revenue', 'profit', 'actual_delivery'):
                native = actual[(scenario, F(7), 'cumulative_'+metric)] - actual[(scenario, F(6), 'cumulative_'+metric)]
                snapshot[metric] = dict(expected=float(monthly[(scenario, 7)][metric]), actual=native)
            snapshot['year_profit'] = dict(expected=float(expected[(scenario, F(12), 'cumulative_profit')]),
                                           actual=actual[(scenario, F(12), 'cumulative_profit')])
            samples.append(dict(scenario='copilot' if scenario else 'baseline', **snapshot))
        results.append(dict(example=name, **verdict, month_7=samples, manifest_id=manifest['id'],
                            declared_check_samples=sum(c.get('samples', 0) for c in check['checks'])))
        if name == 'tm_spare':
            # A plausible economic defect preserves all model accounting identities.
            broken = copy.deepcopy(model)
            next(c for c in broken['components'] if c['id']=='revenue_rate')['expr'] = '(tm_delivered_rate + fixed_delivered_rate) * realized_rate'
            bug = inputs/'wrong-tm-billing.ir.json'; save(bug, broken)
            passed = json.loads(invoke(executable, 'check', bug, *args))
            assert passed['verdict'] == 'pass'
            wrong = directory/'wrong-tm-billing.csv'; invoke(executable, 'run', bug, *args, '--out', wrong)
            caught = compare(observations(wrong), expected)
            assert caught['failures'], 'independent oracle failed to detect wrong T&M economics'
            oracle_detection.append(dict(defect='Bill T&M at baseline work hours after AI savings',
                declared_checks='pass', independent_oracle='fail as expected',
                mismatch_count=len(caught['failures']), first_failure=caught['failures'][0]))
    return dict(cases=results, negative_controls=oracle_detection)


# request_id, arrival, start, finish, required slots: hand-scheduled under FIFO.
JOBS = [(10, F(0), F(0), F(1), 1), (11, F(0), F(0), F(2), 1),
        (12, F(0), F('1.5'), F(2), 1), (13, F(0), F(2), F(3), 2),
        (14, F(2), F(3), F('3.25'), 1)]
CAPACITY = [(F(0), F(1), 2), (F(1), F('1.5'), 1),
            (F('1.5'), F(3), 2), (F(3), F(4), 1)]


def staffing_expected(dt):
    expected = {}
    for step in range(int(4/dt)+1):
        t = step*dt
        done = [j for j in JOBS if j[3] <= t]
        active = [j for j in JOBS if j[2] <= t < j[3]]
        capacity = next((c for start, end, c in CAPACITY if start <= t < end), 1)
        capacity_area = sum(max(F(0), min(t, end)-start)*c for start, end, c in CAPACITY)
        occupied_area = sum(max(F(0), min(t, end)-start)*units for _, _, start, end, units in JOBS)
        values = dict(revenue=100*len(done), cost=20*t, capacity=capacity,
            allocated=sum(j[4] for j in active), accepted=sum(j[1]<=t for j in JOBS),
            started=sum(j[2]<=t for j in JOBS), completed=len(done), in_service=len(active),
            waiting=sum(j[1]<=t<j[2] for j in JOBS),
            wait_total=sum(j[2]-j[1] for j in JOBS if j[2]<=t),
            cycle_total=sum(j[3]-j[1] for j in done),
            final_running=int(F(3)<=t<F('3.25')), final_completed=int(t>=F('3.25')),
            # This metric records ever granted, not current resource ownership.
            final_granted=int(t>=F(3)),
            revenue_time=sum(100*(t-j[3]) for j in done),
            utilization=occupied_area/capacity_area if capacity_area else F(0))
        for metric, value in values.items(): expected[(0, t, metric)] = F(value)
    return expected


def staffing(executable, directory):
    directory.mkdir(); results = []; grids = []
    for label, dt in [('quarter', F(1,4)), ('half', F(1,2)), ('day', F(1))]:
        model = json.loads((ROOT/'models/agent_pool_sd.ir.json').read_text())
        model['time']['dt'] = float(dt)
        path = directory/(label+'.ir.json'); save(path, model)
        output = directory/(label+'.csv'); invoke(executable, 'run', path, '--out', output)
        actual = observations(output); expected = staffing_expected(dt)
        verdict = compare(actual, expected); assert not verdict['failures'], (label, verdict)
        manifest = json.loads(Path(str(output)+'.manifest.json').read_text())
        results.append(dict(grid_days=str(dt), **verdict, manifest_id=manifest['id']))
        grids.append(actual)
    assert all(all(rows[key] == grids[0][key] for key in rows) for rows in grids[1:])
    # Same terminal revenue can hide incorrect off-grid timing.
    delayed = json.loads((directory/'half.ir.json').read_text())
    delayed['agent_pool']['schedule'][3]['engagements'][0]['duration'] = .5
    delayed_path = directory/'delayed-completion.ir.json'; save(delayed_path, delayed)
    delayed_result = directory/'delayed-completion.csv'
    invoke(executable, 'run', delayed_path, '--out', delayed_result)
    wrong = observations(delayed_result)
    assert wrong[(0, F(4), 'revenue')] == grids[1][(0, F(4), 'revenue')] == 500
    assert wrong[(0, F(4), 'revenue_time')] == 850
    caught = compare(wrong, staffing_expected(F(1,2)))
    assert any(f['output']=='revenue_time' for f in caught['failures'])
    return dict(grids=results, common_samples='bit-identical',
        job_ledger=[dict(request_id=j[0], arrival=str(j[1]), start=str(j[2]), finish=str(j[3]), slots=j[4]) for j in JOBS],
        day_4=dict(revenue=500, cost=80, operating_profit=420, revenue_time=875,
                   occupied_slot_days='23/4', capacity_slot_days='13/2', utilization='23/26',
                   wait_total='9/2', cycle_total='37/4'),
        negative_control=dict(defect='Round final completion from day 3.25 to 3.5 while keeping final revenue',
                              independent_oracle='fail as expected', mismatch_count=len(caught['failures']),
                              timing_failure=next(f for f in caught['failures'] if f['output']=='revenue_time')))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path); parser.add_argument('destination', type=Path)
    parser.add_argument('--staffing-only', action='store_true', help='No Arrow input dependency; pricing explicitly not run')
    args = parser.parse_args(); args.destination.mkdir(parents=True, exist_ok=False)
    evidence = dict(scope='deterministic worked examples; synthetic inputs, no empirical Ankura validation',
                    absolute_tolerance=ABS_TOL, relative_tolerance=REL_TOL)
    if not args.staffing_only:
        evidence['pricing'] = pricing(args.executable.resolve(), args.destination/'pricing')
    else: evidence['pricing'] = 'not run: staffing-only selection'
    evidence['staffing'] = staffing(args.executable.resolve(), args.destination/'staffing')
    evidence['verdict'] = 'pass'
    save(args.destination/'validation.json', evidence)
    print(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    main()
