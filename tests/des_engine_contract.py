"""The offline oracle must reject corrupted evidence and real disagreements."""
import copy
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests/oracles/des'))
from compare_engines import compare
from engine_plan import load


def main():
    root = ROOT/'tests/oracles/des'
    plan = load()
    reference = json.loads((root/'engine-reference.json').read_text())
    native = {'queue': json.loads((root/'native-report.json').read_text()),
              'jackson': json.loads((root/'jackson-native-report.json').read_text()),
              'branch': json.loads((root/'branch-native-report.json').read_text())}
    assert compare(plan, reference, native)['passed']
    mutations = [
        lambda r, n: r.update(plan_sha256='stale'),
        lambda r, n: r.update(generator_sha256='stale'),
        lambda r, n: r['packages'].update(simpy='0.0.0'),
        lambda r, n: r['engines'].pop('ciw'),
        lambda r, n: r['engines']['ciw'].pop(),
        lambda r, n: r['engines']['ciw'].append(r['engines']['ciw'][0]),
        lambda r, n: r['engines']['ciw'][0].update(replications=1),
        lambda r, n: n['queue'].update(native_seed=0),
        lambda r, n: n['jackson'].update(window=1),
        lambda r, n: n['queue'].update(plan_version=999),
        lambda r, n: n['queue'].update(**{'pass': False}),
        lambda r, n: n['queue']['cases'].pop(),
        lambda r, n: n['queue']['cases'].append(n['queue']['cases'][0]),
        lambda r, n: n['queue']['cases'][0].update(replications=1),
        lambda r, n: n['queue']['cases'][0]['metrics'].pop('wait'),
        lambda r, n: n['queue']['cases'][0]['metrics']['wait'].update(tolerance=100),
        lambda r, n: n['queue']['cases'][0]['metrics']['wait'].update(target=100),
        lambda r, n: n['queue']['cases'][0]['metrics']['wait'].update(mean=float('nan')),
        lambda r, n: n['queue']['cases'][0]['metrics']['wait'].update(standard_error=-1),
        lambda r, n: n['queue']['cases'][0]['metrics']['wait'].update(ci95=[0, 100]),
        lambda r, n: r['engines']['ciw'][0]['metrics']['wait'].update(mean=float('inf')),
        lambda r, n: n.pop('branch'),
        lambda r, n: n['branch']['cases'][0]['metrics'].pop('match_fraction'),
        lambda r, n: n['branch'].update(native_seed=0),
    ]
    for mutate in mutations:
        r, n = copy.deepcopy(reference), copy.deepcopy(native)
        mutate(r, n)
        try:
            compare(plan, r, n)
        except (ValueError, KeyError):
            continue
        raise AssertionError('corrupted evidence was accepted')
    # Both estimates still pass their individual analytical tolerances, but
    # opposing biases must fail the independently declared pair tolerance.
    r, n = copy.deepcopy(reference), copy.deepcopy(native)
    gate = plan['cases'][0]['metrics']['wait']
    for metric, sign in [(n['queue']['cases'][0]['metrics']['wait'], 1),
                         (r['engines']['ciw'][0]['metrics']['wait'], -1)]:
        metric['mean'] = gate['target']+sign*.99*gate['analytic_tolerance']
        mean, se = metric['mean'], metric['standard_error']
        metric['ci95'] = [mean-1.959963984540054*se, mean+1.959963984540054*se]
    result = compare(plan, r, n)
    assert not result['passed']
    assert not next(c for c in result['comparisons'] if c['name'] == 'md1' and c['engine'] == 'ciw')['metrics']['wait']['passed']
    print(f'Engine contract: {len(mutations)} evidence corruptions rejected; opposing bias fails the pair gate')


if __name__ == '__main__':
    main()
