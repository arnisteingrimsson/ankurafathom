"""Offline native/SimPy/Ciw comparison. No third-party Python dependencies."""
import argparse
import json
import math
from pathlib import Path

from engine_plan import ROOT, digest, load


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def indexed(cases):
    result = {case['name']: case for case in cases}
    require(len(result) == len(cases), 'duplicate report case')
    return result


def validate_metrics(case, expected, plan, native):
    require(case['replications'] == expected['replications'], 'replication count differs from frozen plan')
    require(case['metrics'].keys() == expected['metrics'].keys(), 'metric set differs from frozen plan')
    for key, gate in expected['metrics'].items():
        metric = case['metrics'][key]
        mean, se, ci = metric['mean'], metric['standard_error'], metric['ci95']
        require(finite(mean) and finite(se) and se >= 0, 'invalid mean or standard error')
        require(len(ci) == 2 and all(finite(x) for x in ci), 'invalid confidence interval')
        require(all(math.isclose(a, b, rel_tol=1e-10, abs_tol=1e-12) for a, b in zip(ci,
                    [mean-1.959963984540054*se, mean+1.959963984540054*se], strict=True)), 'inconsistent confidence interval')
        tolerance = gate['analytic_tolerance'] or plan['numerical_tolerance']
        require(abs(mean-gate['target']) <= tolerance, f"{case['name']}/{key} fails analytical gate")
        if native:
            require(metric['target'] == gate['target'] and metric['tolerance'] == gate['analytic_tolerance'], 'native targets/tolerances changed')
            require(metric['pass'] is True, 'native metric reports failure')
    probabilities = [m['mean'] for key, m in case['metrics'].items() if key.startswith('p')]
    if probabilities:
        require(all(-2e-9 <= p <= 1+2e-9 for p in probabilities) and math.isclose(sum(probabilities), 1, abs_tol=2e-9),
                'invalid occupancy distribution')


def compare(plan, reference, native_reports):
    require(reference['version'] == 1 and reference['plan_sha256'] == digest(ROOT/'engine-plan.json'), 'stale engine reference plan')
    require(reference['generator_sha256'] == digest(ROOT/'engine_oracles.py'), 'stale reference generator')
    packages = dict(line.split('==') for line in (ROOT/'requirements.txt').read_text().splitlines() if line)
    require(reference['packages'] == packages, 'reference dependency pins differ')
    require(reference['engines'].keys() == plan['engines'].keys(), 'missing/unknown reference engine')
    expected_names = {case['name'] for case in plan['cases']}
    references = {name: indexed(cases) for name, cases in reference['engines'].items()}
    require(all(cases.keys() == expected_names for cases in references.values()), 'missing/unknown reference case')
    native = {}
    require(native_reports.keys() == plan['plans'].keys(), 'missing/unknown native report group')
    for group, report in native_reports.items():
        metadata = plan['plans'][group]
        require(report['pass'] is True, 'native report failed')
        for key in ('native_seed', 'warmup', 'window'):
            require(report[key] == metadata[key], f'native {key} differs from plan')
        require(report['plan_version'] == metadata['version'], 'native plan version differs')
        cases = indexed(report['cases'])
        require(cases.keys() == {c['name'] for c in plan['cases'] if c['group'] == group}, 'native case set differs')
        native.update(cases)
    results = []
    passed = True
    for expected in plan['cases']:
        actual = native[expected['name']]
        validate_metrics(actual, expected, plan, native=True)
        for engine, cases in references.items():
            other = cases[expected['name']]
            require(other['group'] == expected['group'], 'reference group differs')
            validate_metrics(other, expected, plan, native=False)
            metrics = {}
            for key, gate in expected['metrics'].items():
                a, b = actual['metrics'][key], other['metrics'][key]
                difference = a['mean']-b['mean']
                tolerance = gate['comparison_tolerance'] or plan['numerical_tolerance']
                valid = abs(difference) <= tolerance
                passed &= valid
                se = math.hypot(a['standard_error'], b['standard_error'])
                metrics[key] = dict(native_mean=a['mean'], reference_mean=b['mean'], difference=difference,
                    tolerance=tolerance, difference_standard_error=se,
                    difference_ci95=[difference-1.959963984540054*se, difference+1.959963984540054*se], passed=valid)
            probability_keys = sorted(key for key in metrics if key.startswith('p'))
            diagnostics = {}
            if probability_keys:
                diagnostics['occupancy_total_variation'] = sum(abs(metrics[k]['difference']) for k in probability_keys)/2
                if expected['group'] == 'queue':
                    cumulative, maximum = 0.0, 0.0
                    for key in sorted(probability_keys, key=lambda k: int(k[1:])):
                        cumulative += metrics[key]['difference']
                        maximum = max(maximum, abs(cumulative))
                    diagnostics['occupancy_cdf_sup_distance'] = maximum
            results.append(dict(name=expected['name'], engine=engine, replications=expected['replications'],
                                metrics=metrics, diagnostics=diagnostics))
    return dict(version=1, plan_sha256=digest(ROOT/'engine-plan.json'),
                reference_sha256=digest(ROOT/'engine-reference.json'),
                comparisons=results, metric_count=sum(len(c['metrics']) for c in results), passed=passed)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--queue', type=Path, required=True)
    parser.add_argument('--jackson', type=Path, required=True)
    parser.add_argument('--branch', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    plan = load()
    report = compare(plan, json.loads((ROOT/'engine-reference.json').read_text()),
                     {group: json.loads(path.read_text()) for group, path in
                      [('queue', args.queue), ('jackson', args.jackson), ('branch', args.branch)]})
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
    failures = [(c['name'], c['engine'], key) for c in report['comparisons'] for key, m in c['metrics'].items() if not m['passed']]
    require(report['passed'], f'cross-engine gates failed without retuning: {failures}')
    print(f"Cross-engine comparison: {report['metric_count']} metric gates passed")


if __name__ == '__main__':
    main()
