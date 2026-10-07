"""Build project-native live controller against the existing engine build."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import pyarrow

HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,default=ROOT/'build-arrow');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
build=a.build.resolve();out=HERE/'native/build';out.mkdir(exist_ok=True)
arrow=Path(pyarrow.__file__).parent
flags=['-std=c++20','-g','-ffp-contract=off','-fno-fast-math','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),'-I'+str(ROOT/'third_party/nlohmann_json/include')]
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
libraries=[str(build/('lib'+name+'.a')) for name in ('fathom_ir','fathom_outputs','fathom_data','fathom_core')]
for name in ('parquet','arrow'):
    candidates=sorted(arrow.glob('lib'+name+'.*.dylib')) or sorted(arrow.glob('lib'+name+'.so.*'))
    if not candidates:raise SystemExit('Arrow shared library not found: '+name)
    libraries.append(str(candidates[0]))
for source,name in [('live_runner.cpp','tr-live'),('observer_tests.cpp','observer-tests')]:
    command=['c++',*flags,str(HERE/'native'/source),*libraries,'-Wl,-rpath,'+str(arrow),'-o',str(out/(name+('-sanitize' if a.sanitize else '')))]
    subprocess.run(command,check=True)
print(out)
