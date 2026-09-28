"""Build and execute the new native benchmark adapters; no third-party runtime needed."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PROGRAMS = [('c22', 'c22_native.cpp', 'c22_contract.py'),
            ('abm-known', 'abm_known_answers.cpp', 'abm_known_answers.py'),
            ('lattice', 'lattice_native.cpp', 'lattice_contract.py'),
            ('coupling', 'coupling_native.cpp', 'coupling_contract.py'),
            ('stupidmodel', 'stupidmodel_native.cpp', 'stupidmodel_contract.py'),
            ('c21-ball', 'c21_ball_native.cpp', 'c21_ball_contract.py'),
            ('c17', 'c17_native.cpp', 'c17_contract.py')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True, help='New evidence directory (never overwritten)')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--only', nargs='+', choices=[item[0] for item in PROGRAMS],
                        help='Run selected adapters; omitted means the full campaign')
    args = parser.parse_args()
    destination = args.out.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    commands = []
    selected = [item for item in PROGRAMS if not args.only or item[0] in args.only]
    for name, driver, contract in selected:
        executable = destination / name
        command = [args.compiler, '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                   '-ffp-contract=off', '-fno-fast-math', '-Iinclude', '-Ithird_party/nlohmann_json/include']
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        command += [str(ROOT / 'tests/benchmarks' / driver), '-o', str(executable)]
        commands.append(command)
        with (destination / (name + '-build.log')).open('w') as log:
            subprocess.run(command, cwd=ROOT, check=True, stdout=log, stderr=subprocess.STDOUT)
        command = [sys.executable, str(ROOT / 'tests/benchmarks' / contract), str(executable), str(destination / (name + '-results'))]
        commands.append(command)
        env = dict(os.environ, ASAN_OPTIONS='halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        with (destination / (name + '-run.log')).open('w') as log:
            subprocess.run(command, cwd=ROOT, env=env, check=True, stdout=log, stderr=subprocess.STDOUT)
        print(name + ': passed', flush=True)
    inputs = sorted((ROOT / 'include/ankurafathom').rglob('*.hpp')) + sorted((ROOT / 'tests/benchmarks').glob('*.*'))
    manifest = dict(verdict='pass', sanitized=args.sanitize, selected=[item[0] for item in selected], commands=commands,
                    compiler=subprocess.check_output([args.compiler, '--version'], text=True),
                    source_sha256={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in inputs if p.is_file()})
    (destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
