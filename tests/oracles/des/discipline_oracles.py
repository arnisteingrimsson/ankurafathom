"""Pinned exact FIFO/LIFO/priority queue traces; no native input to generation."""
import argparse
from importlib.metadata import version
import json
from pathlib import Path

from discipline_plan import ROOT, generate as planned
from engine_plan import digest
from engine_oracles import simpy_run, ciw_run, compare_traces, equivalent, require


def load_plan():
    plan = json.loads((ROOT/'discipline-plan.json').read_text())
    require(plan == planned(), 'discipline workloads differ from frozen plan')
    return plan


def metadata():
    packages = dict(line.split('==') for line in (ROOT/'requirements.txt').read_text().splitlines() if line)
    return dict(version=1, plan_sha256=digest(ROOT/'discipline-plan.json'),
                generator_sha256=digest(ROOT/'discipline_oracles.py'), adapter_sha256=digest(ROOT/'engine_oracles.py'),
                packages=packages)


def generate():
    report = metadata()
    require(all(version(k) == v for k, v in report['packages'].items()), 'oracle dependencies differ from pins')
    report['cases'] = []
    for spec in load_plan()['cases']:
        arrivals = [j['arrival'] for j in spec['jobs']]
        durations = [[j['service'] for j in spec['jobs']]]
        priorities = [j['priority'] for j in spec['jobs']]
        options = dict(discipline=spec['discipline'], priorities=priorities)
        a = simpy_run(arrivals, durations, spec['capacity'], spec['queue_capacity'], **options)
        b = ciw_run(arrivals, durations, spec['capacity'], spec['queue_capacity'], 20260925, **options)
        compare_traces(a, b)
        # Complete one-node histories must partition source identities.
        served, lost = {r[0] for r in a[0]}, {r[0] for r in a[1]}
        require(len(served) == len(a[0]) and len(lost) == len(a[1]) and not served & lost and
                served | lost == set(range(len(arrivals))), 'oracle flow/identity conservation failed')
        report['cases'].append(dict(spec=spec, records=[list(row) for row in a[0]], rejected=[list(row) for row in a[1]]))
    return report


def compare(reference, native, unbounded_only=False):
    for key, value in metadata().items(): require(reference[key] == value, 'stale discipline reference metadata')
    require(native['version'] == 1, 'native trace version differs')
    plan = load_plan()
    require(len(reference['cases']) == len(plan['cases']), 'reference discipline case set differs')
    # Selection is an explicit test configuration, never inferred from which
    # native cases happen to be present. The whole reference remains validated.
    for spec, expected in zip(plan['cases'], reference['cases'], strict=True):
        require(expected['spec'] == spec, 'reference input/order differs')
    selected = [(spec, expected) for spec, expected in zip(plan['cases'], reference['cases'], strict=True)
                if not unbounded_only or spec['queue_capacity'] is None]
    require(len(native['cases']) == len(selected), 'discipline case set differs')
    count = 0
    for (spec, expected), actual in zip(selected, native['cases'], strict=True):
        require(actual['spec'] == expected['spec'] == spec, 'discipline input/order differs')
        compare_traces((actual['records'], actual['rejected']), (expected['records'], expected['rejected']))
        count += len(actual['records'])+len(actual['rejected'])
    return count


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--native', type=Path)
    parser.add_argument('--unbounded-only', action='store_true')
    args = parser.parse_args()
    path = ROOT/'discipline-reference.json'
    if args.native:
        count = compare(json.loads(path.read_text()), json.loads(args.native.read_text()), args.unbounded_only)
        print(f'Discipline reference comparison: {count} exact service/rejection outcomes passed')
    else:
        require(not args.unbounded_only, 'subset selection is only available for an explicit native comparison')
        report = generate()
        if args.verify: equivalent(report, json.loads(path.read_text()))
        else: path.write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
        print(f"SimPy/Ciw discipline traces: {len(report['cases'])} cases agree")
