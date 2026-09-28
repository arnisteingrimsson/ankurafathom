"""Bass sampling and convergence design; freeze before either ensemble."""
import argparse
import json
import math
from pathlib import Path
HERE=Path(__file__).resolve().parent


def make_plan():
    comparisons=90; nrep=256; alpha=.001
    cutoff=math.sqrt(math.log(2*comparisons/alpha)/nrep)
    return dict(version=1,populations=[40,160,640],replications=nrep,
                ensemble_steps=[.25,.0625],sd_steps=[.5,.25,.125,.0625,.03125],
                times=[0,1,2,4,6,8],seed=32452843,reference_seed=49979687,scenario=53,stream=1501,
                cases=[dict(id='bass',p=.03,q=.7,initial_fraction=0.),
                       dict(id='innovation-only',p=.15,q=0.,initial_fraction=0.),
                       dict(id='seeded-imitation',p=0.,q=.7,initial_fraction=.1)],
                independent_gates=dict(comparisons=comparisons,family_alpha=alpha,mean_floor=.015,mean_sigma=6),
                power=dict(population_ks_alternative=.45,miss_probability_bound=4*math.exp(-nrep*(.45-cutoff)**2/2)),
                analytic_gates=dict(comparisons=30,mean_sigma=6,variance_sigma=6,variance_floor=1e-6),
                limit_gates=dict(rmse_ratio_min=.25,rmse_ratio_max=.8,finest_coarse_max=.4,
                                 mean_floor=.012,mean_sigma=6,mean_comparisons=30),
                numeric_gates=dict(reference_tolerance=2e-11,euler_agreement=2e-13,
                                   euler_halving_ratio=[.4,.6],finest_euler_error=.01,
                                   rk4_finest_error=1e-8,rk4_halving_ratio=[10,22]),
                scope='fixed membership, linear Jacobi adoption probability dt*(p+q*a); N-limit is discrete Euler; dt-limit is continuous Bass; empirical finite-horizon evidence')


def encoded():
    return json.dumps(make_plan(),indent=2,allow_nan=False)+'\n'


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--verify',action='store_true');args=p.parse_args()
    path=HERE/'bass-mean-field-plan.json'
    if args.verify:
        assert path.read_text()==encoded(),'Bass mean-field plan changed'
        assert json.loads(encoded())==make_plan(),'Bass plan is not JSON-native'
    else:
        path.write_text(encoded())
