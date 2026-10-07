"""Record the application contract suite without changing engine CTest registration."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    command = [sys.executable, '-m', 'unittest', 'discover', '-s', 'application/tests', '-v']
    with (args.out/'tests.log').open('w') as log:
        result = subprocess.run(command, env=dict(os.environ, FATHOM_SERVICE_ENGINE=str(args.engine.resolve())), stdout=log, stderr=subprocess.STDOUT)
    import re
    text = (args.out/'tests.log').read_text()
    counts = re.search(r'Ran (\d+) tests', text)
    report = dict(verdict='pass' if result.returncode == 0 else 'fail', tests=int(counts[1]) if counts else None,
                  elapsed_seconds=time.monotonic()-started, command=command,
                  engine=str(args.engine.resolve()), engine_sha256=hashlib.sha256(args.engine.read_bytes()).hexdigest(),
                  source_sha256={str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in sorted(Path('application').rglob('*')) if path.is_file() and '__pycache__' not in path.parts})
    (args.out/'validation.json').write_text(json.dumps(report, indent=2)+'\n')
    print(text)
    raise SystemExit(result.returncode)


if __name__ == '__main__': main()
