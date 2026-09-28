"""Fixed cases/tolerances; freeze before generating either sample ensemble."""
import argparse,json
from pathlib import Path
HERE=Path(__file__).resolve().parent

def make_plan():
    cases=[]
    for p in [.05,.3,.8]:cases.append(dict(id=f'er-{p}',kind='erdos_renyi',n=48,probability=p))
    for k,p in [(4,0),(4,.25),(8,1)]:cases.append(dict(id=f'ws-{k}-{p}',kind='watts_strogatz',n=48,degree=k,probability=p))
    for m in [1,3,8]:cases.append(dict(id=f'ba-{m}',kind='barabasi_albert',n=48,m=m))
    return dict(version=1,replications=512,seed=908172635,reference_seed=271828183,scenario=17,stream=901,
        metrics=['density','max_degree','degree_second_moment','transitivity','giant_fraction'],
        gates=dict(family_alpha=.001,ks_tests=len(cases)*5,mean_sigma=6,
                   mean_floor=dict(density=.005,max_degree=.01,degree_second_moment=.005,transitivity=.01,giant_fraction=.01),
                   er_mean_sigma=6,er_variance_relative_tolerance=.35),
        cases=cases)

def encoded():return json.dumps(make_plan(),indent=2,allow_nan=False)+'\n'
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--verify',action='store_true');args=parser.parse_args();path=HERE/'generator-plan.json'
    if args.verify:assert path.read_text()==encoded(),'generator plan changed'
    else:path.write_text(encoded())
