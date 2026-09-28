"""Well-mixed SIR/SD validation policy, frozen before generating samples."""
import argparse
import json
import math
from pathlib import Path
HERE=Path(__file__).resolve().parent


def make_plan():
    return dict(version=1,populations=[40,160,640],replications=256,
                initial_infected_denominator=10,times=[0,1,2,3,4,6,8,12],
                seed=179424673,reference_seed=198491317,scenario=47,
                waiting_stream=1401,selection_stream=1402,
                cases=[dict(id=name,beta=beta,gamma=gamma) for name,beta,gamma in [
                    ('epidemic',.8,.3),('declining',.4,.6),('infection-only',.6,0),('recovery-only',0,.4)]],
                sd_steps=[.03125,.015625],sd_error_limit=1e-8,sd_halving_ratio=[10,22],
                reference_tolerance=2e-11,
                metrics=['infected','recovered'],
                power=dict(population_ks_alternative=.45,miss_probability_bound=4*math.exp(-256*(.45-math.sqrt(math.log(336000)/256))**2/2)),
                independent_gates=dict(comparisons=168,family_alpha=.001,mean_sigma=6,mean_floor=.015),
                limit_gates=dict(finest_mean_floor=.012,mean_sigma=6,
                                 rmse_ratio_min=.25,rmse_ratio_max=.8,finest_coarse_max=.4,
                                 mean_comparisons=84),
                scope='individual well-mixed CTMC infection hazard beta*I/N, recovery gamma; fixed initial fraction; path RMS convergence to SD as N increases; no finite-dt ABM bias or universal parameter claim')


def encoded():
    return json.dumps(make_plan(),indent=2,allow_nan=False)+'\n'


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--verify',action='store_true');args=p.parse_args()
    path=HERE/'sir-mean-field-plan.json'
    if args.verify:
        assert path.read_text()==encoded(),'SIR mean-field plan changed'
        assert json.loads(encoded())==make_plan(),'mean-field plan is not JSON-native'
    else:
        path.write_text(encoded())
