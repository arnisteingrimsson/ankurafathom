"""Offline installed-wheel and relocated native-SDK acceptance. No package publication."""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import venv
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ENV = {k: v for k, v in os.environ.items() if k not in (
    'PYTHONPATH', 'PYTHONHOME', 'DYLD_LIBRARY_PATH', 'DYLD_FALLBACK_LIBRARY_PATH', 'LD_LIBRARY_PATH')}

def call(command, cwd, *, env=ENV):
    result = subprocess.run(list(map(str, command)), cwd=cwd, env=env, capture_output=True, text=True)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result.stdout

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wheel', type=Path, required=True)
    parser.add_argument('--sdist', type=Path, required=True)
    parser.add_argument('--wheelhouse', type=Path, required=True)
    parser.add_argument('--native-build', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    args = parser.parse_args()
    with tarfile.open(args.sdist) as archive:
        names = [Path(n).parts[1:] for n in archive.getnames()]
        allowed = {'CMakeLists.txt', 'pyproject.toml', 'README.md', 'PKG-INFO', 'cmake', 'include',
                   'src', 'runtime', 'bindings', 'third_party', 'docs', 'ir'}
        assert all(n and n[0] in allowed for n in names), names
        assert not any('__pycache__' in n or any(p.startswith('.venv') for p in n) for n in names)
        assert ('cmake', 'AnkuraFathomConfig.cmake.in') in names
        assert ('bindings', 'python', 'ankurafathom', '__init__.py') in names
    with zipfile.ZipFile(args.wheel) as archive:
        names = archive.namelist()
        assert all(n.startswith('ankurafathom/') or '.dist-info/' in n for n in names)
        assert any('/.libs/libankurafathom' in n for n in names)
        metadata = archive.read(next(n for n in names if n.endswith('/METADATA'))).decode()
        assert 'Requires-Dist: pyarrow==25.0.1' in metadata
        for name in ('arrow_c/LICENSE.txt', 'nlohmann_json/LICENSE.MIT', 'sobol/LICENSE.txt', 'nanobind/LICENSE'):
            assert any(n.endswith(name) for n in names), name
        assert not any('/libarrow.' in n or '/libparquet.' in n for n in names)
    with tempfile.TemporaryDirectory(prefix='fathom packaging ') as directory:
        tmp = Path(directory)
        environment = tmp/'isolated environment'
        venv.EnvBuilder(with_pip=True).create(environment)
        python = environment/'bin/python'
        call([python, '-m', 'pip', 'install', '--no-index', '--find-links', args.wheelhouse.resolve(),
              args.wheel.resolve()], tmp)
        call([python, '-m', 'pip', 'check'], tmp)
        site = Path(call([python, '-I', '-c', 'import sysconfig; print(sysconfig.get_path("platlib"))'], tmp).strip())
        assert not (site/'nanobind').exists() and not (site/'scikit_build_core').exists()
        # Move both runtime packages away from their original installed locations.
        relocated = tmp/'relocated packages'; relocated.mkdir()
        for path in site.iterdir():
            if path.name.startswith(('ankurafathom', 'pyarrow')):
                shutil.move(str(path), relocated/path.name)
        package = relocated/'ankurafathom'
        binaries = [*package.glob('_native*.so'), *package.glob('.libs/*')]
        assert binaries
        for binary in binaries:
            if sys.platform == 'darwin':
                report = call(['otool', '-l', binary], tmp)
                assert str(ROOT) not in report and 'build/' not in report, (binary, report)
                assert '@loader_path/' in report, (binary, report)
            elif sys.platform.startswith('linux'):
                report = call(['readelf', '-d', binary], tmp)
                assert str(ROOT) not in report and '$ORIGIN/' in report, (binary, report)
        summary = call([python, '-I', ROOT/'tests/python_contract.py', relocated, args.probe.resolve()], tmp)
        assert '112 C-ABI comparisons / 82931 exact observations; 64 controls pass' in summary, summary
        print('Relocated installed wheel:', summary.strip())
        summary = call([python, '-I', ROOT/'tests/callbacks_contract.py', relocated], tmp)
        assert '36 mode/parity cases; 59 cancellation' in summary, summary
        print('Installed callbacks:', summary.strip())
        # Exercise the additive provenance export from the installed extension too.
        output = tmp/'installed-provenance.parquet'
        code = """import sys,json
from pathlib import Path
sys.path.insert(0,sys.argv[1])
import ankurafathom as af
import pyarrow.parquet as pq
model=af.Model.from_json(Path(sys.argv[2]).read_bytes())
table=af.run(model,provenance=True)
manifest=json.loads(table.schema.metadata[b'ankurafathom.manifest'])
assert manifest['manifest_version']=='0.3' and manifest['inputs']['model']['path']==''
assert manifest['output']['format']=='memory'
pq.write_table(table,sys.argv[3])
"""
        call([python, '-I', '-c', code, relocated, ROOT/'models/decay.ir.json', output], tmp)
        verdict = json.loads(call([args.probe.resolve().with_name('fathom'), 'verify-results', output, '--embedded'], tmp))
        assert verdict['verdict'] == 'pass'
        print('Installed provenance: schema-0.2 Parquet and embedded manifest verification pass')
        # Native installation is a separate prefix and independent of the wheel.
        original = tmp/'native original'
        call(['cmake', '--install', args.native_build.resolve(), '--prefix', original], tmp)
        native = tmp/'native relocated'; original.rename(native)
        for config in native.rglob('*.cmake'):
            assert str(args.native_build.resolve()) not in config.read_text(), config
        consumer = tmp/'C consumer'; consumer.mkdir()
        shutil.copyfile(ROOT/'tests/c_api_tests.c', consumer/'main.c')
        shutil.copyfile(ROOT/'models/decay.ir.json', consumer/'decay.json')
        (consumer/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(InstalledConsumer LANGUAGES C)
set(CMAKE_C_STANDARD 11)
find_package(AnkuraFathom 0.1 CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE AnkuraFathom::c_api Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_libraries(consumer PRIVATE m)
endif()
''')
        call(['cmake', '-S', consumer, '-B', consumer/'build', f'-DCMAKE_PREFIX_PATH={native}'], tmp)
        call(['cmake', '--build', consumer/'build'], tmp)
        summary = call([consumer/'build/consumer', consumer/'decay.json'], tmp)
        assert 'pass' in summary, summary
        print('Relocated native C SDK:', summary.strip())
        output = tmp/'cli.csv'
        call([native/'bin/fathom', 'run', consumer/'decay.json', '--out', output], tmp)
        with output.open() as stream:
            rows = list(csv.DictReader(stream))
        assert len(rows) == 11
        value = 100.0
        for row in rows:
            assert abs(float(row['value']) - value) < 1e-12
            value -= 0.1 * (0.2 * value)
        receipt = Path(str(output) + '.manifest.json')
        assert receipt.exists() and rows[0]['manifest_id'] == json.loads(receipt.read_text())['id']
        verdict = json.loads(call([native/'bin/fathom', 'verify-results', receipt], tmp))
        assert verdict['verdict'] == 'pass'
        verdict = json.loads(call([native/'bin/fathom', 'replay', receipt], tmp))
        assert verdict['verdict'] == 'pass'
        bundle = tmp/'installed bundle'
        verdict = json.loads(call([native/'bin/fathom', 'bundle', receipt, '--out', bundle], tmp))
        assert verdict['scope'] == 'input-bundle'
        moved = tmp/'moved bundle'; bundle.rename(moved)
        (consumer/'decay.json').unlink(); output.unlink(); receipt.unlink()
        verdict = json.loads(call([native/'bin/fathom', 'replay', moved, '--threads', '8'], tmp))
        assert verdict['verdict'] == 'pass'
        print('Installed CLI: 11 analytic Euler observations, default sidecar verification and relocated bundle replay pass')
    print('Packaging: sdist allowlist, wheel contents/notices, offline installation, relative library paths and relocation pass')

if __name__ == '__main__':
    main()
