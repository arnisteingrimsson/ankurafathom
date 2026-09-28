"""Continuous-time SIR and finite-state convergence gates, frozen before samples."""
import argparse
import json
import math
from pathlib import Path
from sir_plan import make_plan as synchronous_plan
HERE=Path(__file__).resolve().parent


def make_plan():
    source=synchronous_plan()
    cases=[{k:v for k,v in c.items() if k!='dt'} for c in source['cases']]
    small=[dict(id=name,states=states,edges=edges,infection_rate=beta,recovery_rate=gamma)
           for name,states,edges,beta,gamma in [
               ('line-three',[1,0,0],[[0,1],[1,2]],.9,.4),
               ('triangle-three',[1,0,0],[[0,1],[0,2],[1,2]],.6,.5),
               ('star-four',[1,0,0,0],[[0,1],[0,2],[0,3]],.7,.35)]]
    return dict(version=1,replications=512,seed=104395301,reference_seed=122949829,
                scenario=41,waiting_stream=1401,selection_stream=1402,
                times=[0,2,8],cases=cases,metrics=source['metrics'],gates=source['gates'],power=source['power'],
                event_tolerance=2e-11,
                convergence=dict(cases=small,replications=1024,scenario=43,seed=141650939,
                                 infection_stream=1301,recovery_stream=1302,
                                 times=[2,4],steps=[1,.5,.25,.125,.0625],
                                 family_alpha=.001,joint_comparisons=36,pair_comparisons=6,
                                 finest_tv_limit=.04,finest_coarse_ratio_limit=.15,
                                 last_ratio_min=.35,last_ratio_max=.7,
                                 uniformization_tail=1e-14),
                scope='native AsyncPopulation direct Gillespie SIR; same per-edge beta and gamma as sync; exact finite-state distribution convergence on three small graphs, not mean-field or general changing-hazard statecharts')


def encoded():
    return json.dumps(make_plan(),indent=2,allow_nan=False)+'\n'


if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--verify',action='store_true')
    args=p.parse_args()
    path=HERE/'sir-async-plan.json'
    if args.verify:
        assert path.read_text()==encoded(),'async SIR plan changed'
        assert json.loads(encoded())==make_plan(),'async SIR plan is not JSON-native'
    else:
        path.write_text(encoded())
