"""Freeze cross-engine gates from the previously frozen analytical plans."""
import argparse
import hashlib
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PLAN_NAMES = {'queue': 'queue-plan.json', 'jackson': 'jackson-plan.json', 'branch': 'branch-plan.json'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generate():
    plan = dict(version=1, engines={'simpy': '4.1.2', 'ciw': '3.2.7'},
                requirements_sha256=digest(ROOT/'requirements.txt'),
                numerical_tolerance=2e-9, pair_tolerance_factor=math.sqrt(2),
                reference_seed_base=921001, case_seed_stride=1000,
                service_seed_offset=9000000, station_seed_stride=100000, routing_seed_offset=8000000,
                plans={}, cases=[])
    for group, filename in PLAN_NAMES.items():
        original = json.loads((ROOT/filename).read_text())
        plan['plans'][group] = dict(sha256=digest(ROOT/filename), version=original['version'],
                                    native_seed=original['native_seed'], warmup=original['warmup'], window=original['window'])
        for spec in original['cases']:
            plan['cases'].append(dict(name=spec['name'], group=group,
                seed_base=921001+1000*len(plan['cases']), replications=spec['replications'],
                metrics={key: dict(target=metric['target'], analytic_tolerance=metric['tolerance'],
                    comparison_tolerance=math.sqrt(2)*metric['tolerance']) for key, metric in spec['metrics'].items()}))
    return plan


def load():
    plan = json.loads((ROOT/'engine-plan.json').read_text())
    if plan != generate():
        raise ValueError('cross-engine plan or source plans changed; investigate before updating gates')
    return plan


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    if args.verify:
        plan = load()
    else:
        plan = generate()
        (ROOT/'engine-plan.json').write_text(json.dumps(plan, indent=2)+'\n')
    print(f"Engine plan: {len(plan['cases'])} cases, {sum(c['replications'] for c in plan['cases'])} replications per engine")
