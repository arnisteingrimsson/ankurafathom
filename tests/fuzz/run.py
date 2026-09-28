"""Run independent libFuzzer campaigns and retain auditable logs/artifacts."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--seconds', type=int, default=600)
parser.add_argument('--seed', type=int, default=20260927)
args = parser.parse_args()
if args.seconds < 1 or not 1 <= args.seed <= 4294967295:
    parser.error('positive duration and uint32 nonzero seed required')
build = args.build.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)  # never overwrite an evidence run
subprocess.run([sys.executable, str(Path(__file__).with_name('prepare_corpus.py')),
                str(output/'corpus')], check=True)
environment = os.environ.copy()
environment['ASAN_OPTIONS'] = environment.get('ASAN_OPTIONS', '') + ':halt_on_error=1:abort_on_error=1'
environment['UBSAN_OPTIONS'] = environment.get('UBSAN_OPTIONS', '') + ':halt_on_error=1:print_stacktrace=1'

def campaign(name):
    binary = build/'tests'/'fuzz'/('fuzz_' + name)
    artifacts = output/name
    artifacts.mkdir()
    command = [str(binary), str(output/'corpus'/name),
               f'-max_total_time={args.seconds}', '-max_len=65536', '-timeout=10',
               '-rss_limit_mb=2048', f'-seed={args.seed}', '-print_final_stats=1',
               '-artifact_prefix=' + str(artifacts) + os.sep]
    result = {'target': name, 'command': command, 'requested_seconds': args.seconds}
    started = time.monotonic()
    try:
        result['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        with (artifacts/'run.log').open('wb') as log:
            process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                     env=environment, timeout=args.seconds + 120)
        result['returncode'] = process.returncode
        text = (artifacts/'run.log').read_text(errors='replace')
        match = re.search(r'stat::number_of_executed_units:\s+(\d+)', text)
        result['executions'] = int(match[1]) if match else 0
        result['sanitizer_diagnostic'] = bool(re.search(
            r'ERROR: (?:AddressSanitizer|UndefinedBehaviorSanitizer|libFuzzer)|runtime error:|SUMMARY: .*Sanitizer', text))
        result['clean'] = process.returncode == 0 and result['executions'] > 0 and not result['sanitizer_diagnostic']
    except (OSError, subprocess.TimeoutExpired) as error:
        result.update(clean=False, error=str(error))
    result['elapsed_seconds'] = round(time.monotonic() - started, 3)
    result['completed_duration'] = result['elapsed_seconds'] >= args.seconds
    result['ten_minute_gate'] = result['clean'] and result['completed_duration'] and args.seconds >= 600
    return result

with ThreadPoolExecutor(max_workers=2) as pool:
    results = list(pool.map(campaign, ('expression', 'ir_loader')))
report = {'version': '0.1', 'finished_utc': datetime.now(timezone.utc).isoformat(),
          'platform': platform.platform(), 'seed': args.seed, 'campaigns': results,
          'ten_minute_gate': all(result['ten_minute_gate'] for result in results)}
cache = build/'CMakeCache.txt'
if cache.exists():
    report['cmake_configuration'] = [line for line in cache.read_text().splitlines()
        if re.match(r'(CMAKE_(CXX_COMPILER|BUILD_TYPE)|FATHOM_(ENABLE_FUZZING|ENABLE_SANITIZERS|LIBFUZZER_LIBRARY)):', line)]
(output/'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
sys.exit(0 if all(result['clean'] and result['completed_duration'] for result in results) else 1)
