"""Frozen SciPy Sobol points and independent addressed Latin hypercube reference."""
import argparse
import copy
import gzip
import hashlib
import itertools
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PLAN = HERE/'design-plan.json'
REFERENCE = HERE/'design-reference.json.gz'


def require(ok, why):
    if not ok:
        raise ValueError(why)


def make_plan():
    cases = []
    for i, axes in enumerate(({'z': [10, 20, 30], 'a': [1, 2]}, {'x': [-1, 0, 1], 'y': [2, 2, 3]},
                              {'p02': [3, 4], 'p00': [0, 1], 'p01': [1, 2]}, {'x': [1]})):
        cases.append(dict(id=f'grid_{i}', kind='grid', axes=axes, first_id=65535 if i==3 else 17))
    def bounds(d):
        return {f'p{i:02}': [-1, 2] if i%3==0 else [0, 1] for i in range(d)}
    for d in (1, 3, 32):
        for n in (1, 7, 64, 1000):
            for seed in (0, 2**64-1):
                cases.append(dict(id=f'lhs_{d}_{n}_{seed}', kind='lhs', bounds=bounds(d), count=n, design_seed=seed, first_id=17))
    cases.append(dict(id='lhs_max_count', kind='lhs', bounds={'p00': [0, 1]}, count=65536, design_seed=77, first_id=0))
    for d in (1, 2, 8, 32):
        for n in (1, 8, 64, 1024):
            cases.append(dict(id=f'sobol_{d}_{n}', kind='sobol', bounds=bounds(d), count=n, first_id=17))
    cases.append(dict(id='sobol_max_count', kind='sobol', bounds={'p00': [0, 1]}, count=65536, first_id=0))
    return dict(version=1, cases=cases, axis_orders=['forward', 'reverse'], tolerance=2e-13)


def word(seed, index, dimension, attempt, purpose):
    # Independent arbitrary-precision integer implementation of the declared raw
    # counter; no native address packer or production code is used.
    mask = 2**32-1
    counter = [index, dimension, attempt, purpose]
    key = [seed & mask, seed >> 32]
    for round_index in range(10):
        a = counter[0]*0xd2511f53;b = counter[2]*0xcd9e8d57
        counter = [(b>>32)^counter[1]^key[0], b & mask, (a>>32)^counter[3]^key[1], a & mask]
        if round_index!=9:
            key = [(key[0]+0x9e3779b9) & mask, (key[1]+0xbb67ae85) & mask]
    return counter[0]


def reference(case):
    first = case['first_id']
    if case['kind']=='grid':
        names = sorted(case['axes'])
        return [[first+i, list(values)] for i, values in enumerate(itertools.product(*(case['axes'][name] for name in names)))]
    names = sorted(case['bounds']);n = case['count'];d = len(names)
    if case['kind']=='sobol':
        from scipy.stats import qmc
        unit = qmc.Sobol(d, scramble=False, bits=32).random_base2(int(math.log2(n))).tolist()
    else:
        seed = case['design_seed'];unit = [[0.]*d for _ in range(n)]
        for dimension in range(d):
            permutation = list(range(n))
            for index in range(n-1, 0, -1):
                attempt = 0;bound = index+1;threshold = (2**32)%bound
                while True:
                    value = word(seed, index, dimension, attempt, 0x4c485350);attempt += 1
                    if value>=threshold:
                        break
                chosen = value%bound;permutation[index], permutation[chosen] = permutation[chosen], permutation[index]
            for index in range(n):
                jitter = (word(seed, index, dimension, 0, 0x4c48534a)+.5)/2**32
                unit[index][dimension] = (permutation[index]+jitter)/n
            require(sorted(int(row[dimension]*n) for row in unit)==list(range(n)), 'reference Latin strata')
    return [[first+i, [case['bounds'][name][0]+(case['bounds'][name][1]-case['bounds'][name][0])*unit[i][j]
                      for j, name in enumerate(names)]] for i in range(n)]


def generated(plan):
    import scipy
    import scipy.stats
    import numpy as np
    require(scipy.__version__=='1.18.1', 'reference regeneration requires SciPy 1.18.1')
    source = Path(scipy.stats.__file__).parent/'_sobol_direction_numbers.npz'
    provenance = json.loads((ROOT/'third_party/sobol/PROVENANCE.json').read_text())
    require(hashlib.sha256(source.read_bytes()).hexdigest()==provenance['dataset_sha256'], 'Sobol source dataset changed')
    table = np.load(source)
    subset = [dict(dimension=i+1, polynomial=int(table['poly'][i]), initial=[int(x) for x in table['vinit'][i][:7]]) for i in range(32)]
    stored = ROOT/'third_party/sobol/joe-kuo-6-first32.json'
    require(subset==json.loads(stored.read_text()) and hashlib.sha256(stored.read_bytes()).hexdigest()==provenance['subset_sha256'], 'Sobol numeric subset changed')
    return {case['id']: reference(case) for case in plan['cases']}


def score(rows, plan, references):
    cases = {c['id']: c for c in plan['cases']}
    require(len(rows)==len(cases)*len(plan['axis_orders']), 'design record count')
    seen = set();checks = 0;gap = 0.;strata_gates = 0
    for row in rows:
        require(set(row)=={'case', 'axis_order', 'scenarios'}, 'design report fields')
        key = row['case'], row['axis_order']
        require(key[0] in cases and key[1] in plan['axis_orders'] and key not in seen, 'unknown/duplicate design case')
        seen.add(key);case = cases[key[0]];expected = references[key[0]]
        require(len(row['scenarios'])==len(expected), 'scenario count')
        for actual, target in zip(row['scenarios'], expected):
            require(isinstance(actual, list) and len(actual)==2 and type(actual[0]) is int and actual[0]==target[0], 'scenario identity')
            require(isinstance(actual[1], list) and len(actual[1])==len(target[1]), 'coordinate width')
            for a, b in zip(actual[1], target[1]):
                require(type(a) in (int, float) and math.isfinite(a), 'coordinate type/finite')
                error = abs(a-b);require(error<=plan['tolerance'], 'coordinate gap');gap = max(gap, error);checks += 1
        if case['kind']=='lhs':
            n = len(expected)
            for j, name in enumerate(sorted(case['bounds'])):
                low, high = case['bounds'][name]
                unit = [(r[1][j]-low)/(high-low) for r in row['scenarios']]
                require(sorted(math.floor(x*n) for x in unit)==list(range(n)), 'Latin stratum missing/repeated')
                require(abs(sum(unit)/n-.5)<=(.5/n)+1e-14, 'Latin mean bound')
                strata_gates += 2
    return dict(passed=True, configurations=len(rows), coordinate_comparisons=checks,
                strata_and_mean_gates=strata_gates, max_absolute_gap=gap)


def contract(plan, references):
    selected = [plan['cases'][0], next(c for c in plan['cases'] if c['id']=='lhs_3_7_0'), next(c for c in plan['cases'] if c['id']=='sobol_2_8')]
    small = dict(plan, cases=selected, axis_orders=['forward'])
    good = [dict(case=c['id'], axis_order='forward', scenarios=copy.deepcopy(references[c['id']])) for c in selected]
    score(good, small, references);mutations = []
    def corrupt(fn):
        bad = copy.deepcopy(good);fn(bad);mutations.append(bad)
    corrupt(lambda a: a.pop());corrupt(lambda a: a.append(copy.deepcopy(a[0])))
    corrupt(lambda a: a[0].update(case='unknown'));corrupt(lambda a: a[0].update(axis_order='bad'))
    corrupt(lambda a: a[0]['scenarios'][0].__setitem__(0, True));corrupt(lambda a: a[0]['scenarios'][0].__setitem__(0, 1))
    corrupt(lambda a: a[1]['scenarios'][0][1].__setitem__(0, math.nan));corrupt(lambda a: a[1]['scenarios'][0][1].__setitem__(0, True))
    corrupt(lambda a: a[1]['scenarios'][0][1].__setitem__(0, a[1]['scenarios'][1][1][0]))
    corrupt(lambda a: a[2]['scenarios'].reverse());corrupt(lambda a: a[2]['scenarios'][0][1].__setitem__(0, .25))
    corrupt(lambda a: a[0].update(extra=0))
    for bad in mutations:
        try:
            score(bad, small, references)
        except (ValueError, TypeError):
            continue
        raise ValueError('corrupted scenario design accepted')
    return len(mutations)


def main():
    parser = argparse.ArgumentParser();parser.add_argument('--write', action='store_true');parser.add_argument('--verify', action='store_true')
    parser.add_argument('--contract', action='store_true');parser.add_argument('--native', type=Path);parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.write:
        PLAN.write_text(json.dumps(make_plan(), indent=2)+'\n')
        values = generated(make_plan())
        data = json.dumps(values, separators=(',', ':'), sort_keys=True).encode()
        REFERENCE.write_bytes(gzip.compress(data, mtime=0))
        metadata = dict(source_engine='SciPy 1.18.1 unscrambled Sobol(bits=32); independent integer Philox/Fisher-Yates LHS; itertools grid',
                        compressed_sha256=hashlib.sha256(REFERENCE.read_bytes()).hexdigest(), decoded_sha256=hashlib.sha256(data).hexdigest(),
                        cases=len(values), scenarios=sum(len(rows) for rows in values.values()),
                        coordinates=sum(len(r[1]) for rows in values.values() for r in rows))
        REFERENCE.with_name('design-reference-metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
    plan = json.loads(PLAN.read_text());require(plan==make_plan(), 'frozen design plan changed')
    compressed = REFERENCE.read_bytes();decoded = gzip.decompress(compressed)
    metadata = json.loads(REFERENCE.with_name('design-reference-metadata.json').read_text())
    require(hashlib.sha256(compressed).hexdigest()==metadata['compressed_sha256'] and hashlib.sha256(decoded).hexdigest()==metadata['decoded_sha256'], 'scenario reference hash mismatch')
    references = json.loads(decoded)
    if args.verify:
        require(generated(plan)==references, 'pinned scenario reference did not regenerate')
        print('pinned SciPy/independent LHS designs regenerate exactly')
    if args.contract:
        print(f'{contract(plan, references)} scenario design corruption controls passed')
    if args.native:
        data = args.native.read_bytes();report = score([json.loads(line) for line in data.splitlines()], plan, references)
        report['negative_controls'] = contract(plan, references);report['trajectory_sha256'] = hashlib.sha256(data).hexdigest()
        if args.report:
            args.report.write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report))

if __name__=='__main__':
    main()
