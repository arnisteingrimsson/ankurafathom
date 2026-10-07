"""Build the project-owned hybrid model from native header-only engine components."""
from pathlib import Path
import argparse,subprocess,json,hashlib
p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');a=p.parse_args()
here=Path(__file__).resolve().parents[1];root=here.parents[1];out=here/'native/build';out.mkdir(exist_ok=True)
flags=['-std=c++20','-O1' if a.sanitize else '-O2','-g','-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math','-I'+str(root/'include'),'-I'+str(root/'third_party/nlohmann_json/include')]
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
for source,target in [('workshop_runner.cpp','tr-workshop'),('workshop_tests.cpp','workshop-tests')]:
    if (here/'native'/source).exists():subprocess.run(['c++',*flags,str(here/'native'/source),'-o',str(out/(target+('-sanitize' if a.sanitize else '')))],check=True)

runner=out/('tr-workshop'+('-sanitize' if a.sanitize else ''))
paths=[here/'native/workshop_runner.cpp',*[root/'include/ankurafathom'/p for p in ['abm/sync_population.hpp','devs/simulator.hpp','sd/model.hpp','rng/philox.hpp']]]
receipt=dict(runner_sha256=hashlib.sha256(runner.read_bytes()).hexdigest(),sources={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},compiler_flags=flags)
runner.with_suffix('.build.json').write_text(json.dumps(receipt,indent=2)+'\n')
