"""Deterministic, non-simultaneous queue workloads for cross-engine traces."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def generate():
    cases = []
    for discipline in ('fifo', 'lifo', 'priority'):
        for capacity in (1, 2):
            for limit in (0, 1, 3, None):
                for pattern in range(4):
                    jobs = [dict(id=i, arrival=round(.07+i*(.09+.03*pattern)+.0001*i*i, 9),
                                 service=round(.31+.041*((i*7+pattern)%11)+.000013*i, 9),
                                 priority=(i*5+pattern)%7-3) for i in range(24)]
                    cases.append(dict(name=f'{discipline}_{capacity}_{limit}_{pattern}',
                                      capacity=capacity, queue_capacity=limit, discipline=discipline, jobs=jobs))
    return dict(version=1, numerical_tolerance=2e-9, cases=cases)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    path = ROOT/'discipline-plan.json'
    plan = generate()
    if args.verify:
        assert json.loads(path.read_text()) == plan, 'discipline trace workloads changed'
    else:
        path.write_text(json.dumps(plan, indent=2)+'\n')
    print(f"Discipline plan: {len(plan['cases'])} exact trace cases")
