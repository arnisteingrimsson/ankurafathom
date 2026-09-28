"""Reject stale, incomplete, reordered, or numerically incorrect queue traces."""
import copy
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests/oracles/des'))
from discipline_oracles import compare


def main():
    reference = json.loads((ROOT/'tests/oracles/des/discipline-reference.json').read_text())
    native = json.loads(Path(sys.argv[1]).read_text())
    assert compare(reference, native) == 2304
    subset = dict(version=1, cases=[copy.deepcopy(case) for case in native['cases'] if case['spec']['queue_capacity'] is None])
    assert compare(reference, subset, unbounded_only=True) == 576
    try: compare(reference, subset)
    except ValueError: pass
    else: raise AssertionError('partial evidence selected its own subset')
    missing = copy.deepcopy(subset)
    missing['cases'].pop()
    try: compare(reference, missing, unbounded_only=True)
    except ValueError: pass
    else: raise AssertionError('incomplete declared subset accepted')
    mutations = [
        lambda r,n: r.update(adapter_sha256='stale'),
        lambda r,n: r.update(plan_sha256='stale'),
        lambda r,n: r['packages'].update(ciw='0'),
        lambda r,n: n.update(version=2),
        lambda r,n: n['cases'].pop(),
        lambda r,n: n['cases'].reverse(),
        lambda r,n: n['cases'][0]['spec'].update(discipline='lifo'),
        lambda r,n: n['cases'][0]['records'].pop(),
        lambda r,n: n['cases'][0]['records'][0].__setitem__(0,999),
        lambda r,n: n['cases'][0]['records'][0].__setitem__(3,99),
        lambda r,n: n['cases'][0]['records'][0].__setitem__(4,float('nan')),
        lambda r,n: n['cases'][0]['rejected'].pop(),
        lambda r,n: n['cases'][0]['rejected'][0].__setitem__(1,99),
    ]
    for mutate in mutations:
        r, n = copy.deepcopy(reference), copy.deepcopy(native)
        mutate(r,n)
        try: compare(r,n)
        except (ValueError, KeyError, TypeError): continue
        raise AssertionError('corrupt discipline evidence accepted')
    print(f'Discipline evidence contract: {len(mutations)} corruptions and 2 incomplete/subset substitutions rejected')


if __name__ == '__main__':
    main()
