"""Run project checks and a synthetic six-strategy acceptance experiment."""
import copy
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT))
from application.fathom_service.contracts import APIError, digest
from service import Workshop
from spec import DEFAULT, PRESETS
from verify_record import verify


def main():
    output = ROOT / 'artifacts/equinix-ai-acceptance'
    output.mkdir(parents=True, exist_ok=True)
    tests = []
    for name, sanitize in [('normal', '0'), ('asan-ubsan', '1')]:
        result = subprocess.run([sys.executable, str(HERE / 'test_project.py')],
                                env={**os.environ, 'EQUINIX_SANITIZE': sanitize},
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (output / (name + '.log')).write_text(result.stdout)
        print(name, result.stdout, flush=True)
        result.check_returncode()
        tests.append(dict(build=name, passed=True, log=name + '.log', log_sha256=digest(result.stdout.encode())))

    workshop = Workshop(output / 'runs', HERE / 'native/build/equinix-model')
    try:
        experiment = workshop.experiment(dict(config=copy.deepcopy(DEFAULT), variable='strategy', values=list(range(6)), replications=3))
        last = -1
        while experiment['status'] in ('queued', 'running'):
            if experiment['finished'] != last:
                print(f"Acceptance: {experiment['finished']}/{experiment['total']} runs", flush=True)
                last = experiment['finished']
            time.sleep(.25)
            experiment = workshop.experiment_state(experiment['id'])
        if experiment['status'] != 'completed':
            raise RuntimeError(str(experiment))
        replayed = []
        for result in experiment['results']:
            record = json.loads((output / 'runs' / result['session'] / 'session.json').read_text())
            verify(record)
            # Replay one complete daily trajectory per strategy, not only its total.
            if result['seed'] == DEFAULT['parameters']['seed']:
                verify(record, replay=True)
                replayed.append(result['session'])
        corrupted = copy.deepcopy(record)
        corrupted['frames'][-1]['metrics']['revenue'] += 1
        try:
            verify(corrupted)
        except APIError:
            pass
        else:
            raise RuntimeError('Corrupted record was accepted')
        summary = []
        for index, preset in enumerate(PRESETS):
            rows = [r for r in experiment['results'] if r['value'] == index]
            summary.append(dict(strategy=preset['label'], runs=len(rows), means={key: sum(r['metrics'][key] for r in rows) / len(rows) for key in ('active', 'lost', 'revenue', 'contribution', 'capex', 'cash_proxy')}))
        (output / 'comparison.json').write_text(json.dumps(dict(experiment=experiment, summary=summary), indent=2) + '\n')
        native = json.loads((HERE / 'native/build/equinix-model.build.json').read_text())
        paths = [p for p in HERE.rglob('*') if p.is_file() and 'build' not in p.parts and '__pycache__' not in p.parts and p.suffix in ('.py', '.cpp', '.js', '.html', '.css', '.md')]
        receipt = dict(passed=True, created_at=datetime.now(timezone.utc).isoformat(),
                       scope='9 project tests passed on normal and ASan/UBSan builds; 18 synthetic 120-day runs passed native checks; six daily trajectories reproduced exactly; corrupted-record negative control rejected. Technical verification only, not operational calibration.',
                       tests=tests, experiment_id=experiment['id'], native_runs=18, replays=replayed,
                       sources={**native['sources'], **{str(p.relative_to(ROOT)): digest(p.read_bytes()) for p in paths}},
                       comparison_sha256=digest((output / 'comparison.json').read_bytes()))
        (output / 'verification.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(summary, indent=2))
        print('Verification:', output / 'verification.json')
    finally:
        workshop.close()


if __name__ == '__main__':
    main()
