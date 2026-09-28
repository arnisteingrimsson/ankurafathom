"""M/M/1 -> reflected SD fluid design, frozen before native/reference ensembles."""
import argparse
import json
import math
from pathlib import Path
HERE=Path(__file__).resolve().parent


def make_plan():
    comparisons=180;replications=256;alpha=.001
    cutoff=math.sqrt(math.log(2*comparisons/alpha)/replications)
    return dict(version=1,scales=[8,32,128],replications=replications,times=[0,.5,1,2,3,4],
                sd_steps=[.25,.125],seed=67867967,reference_seed=86028121,scenario=59,
                arrival_stream=1601,service_stream=1602,max_entities=65536,
                cases=[dict(id='growing',arrival_rate=1.5,service_rate=1.,initial_backlog=.5),
                       dict(id='draining',arrival_rate=.5,service_rate=1.,initial_backlog=1.),
                       dict(id='critical',arrival_rate=1.,service_rate=1.,initial_backlog=0.),
                       dict(id='underloaded',arrival_rate=.5,service_rate=1.,initial_backlog=0.)],
                independent_gates=dict(comparisons=comparisons,family_alpha=alpha,mean_floor=.02,mean_sigma=6),
                power=dict(population_ks_alternative=.45,miss_probability_bound=4*math.exp(-replications*(.45-cutoff)**2/2)),
                limit_gates=dict(rmse_ratio_min=.15,rmse_ratio_max=.8,finest_coarse_max=.4,finest_rmse_max=.4),
                numeric_tolerance=2e-12,
                scope='FIFO M/M/1 with arrival/service rates multiplied by N and initial jobs N*b0; backlog includes service; integer SD pulse ledger; constant-rate reflected fluid; all native event steps transactional')


def encoded():
    return json.dumps(make_plan(),indent=2,allow_nan=False)+'\n'


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--verify',action='store_true');args=p.parse_args()
    path=HERE/'des-fluid-plan.json'
    if args.verify:
        assert path.read_text()==encoded(),'DES fluid plan changed'
        assert json.loads(encoded())==make_plan(),'DES fluid plan not JSON-native'
    else:path.write_text(encoded())
