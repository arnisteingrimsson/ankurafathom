"""Synchronous network SIR cases and gates, frozen before either ensemble."""
import argparse
import json
import math
from pathlib import Path
HERE = Path(__file__).resolve().parent


def make_plan():
    n = 24
    initial = [1 if i in (0,6) else 2 if i == 23 else 0 for i in range(n)]
    ring = sorted({tuple(sorted((i,(i+d)%n))) for i in range(n) for d in (1,2)})
    split = sorted({tuple(sorted((base+i,base+(i+1)%12))) for base in (0,12) for i in range(12)})
    cases = [dict(id=name, states=initial, edges=[list(edge) for edge in edges], infection_rate=beta, recovery_rate=gamma, dt=.25)
             for name, edges, beta, gamma in [
                 ('ring-local',ring,.5,.3),
                 ('complete-mixing',[(i,j) for i in range(n) for j in range(i+1,n)],.06,.3),
                 ('star-contact',[(0,i) for i in range(1,n)],.45,.5),
                 ('disconnected',split,.65,.25)]]
    return dict(version=1,replications=512,seed=67867967,reference_seed=86028121,
                scenario=37,infection_stream=1301,recovery_stream=1302,
                ticks=[0,8,32],cases=cases,
                metrics=['infected_fraction','recovered_fraction','attack_fraction','si_edge_fraction'],
                gates=dict(family_alpha=.001,comparisons=32,mean_sigma=6,
                           mean_floor=dict(infected_fraction=.035,recovered_fraction=.035,
                                           attack_fraction=.04,si_edge_fraction=.035)),
                power=dict(population_ks_alternative=.30,
                           miss_probability_bound=4*math.exp(-512*(.30-math.sqrt(math.log(64000)/512))**2/2)),
                scope='fixed undirected contact graphs; synchronous frozen-neighbor hazards with p=-expm1(-rate*dt); finite-time distributions, not asynchronous or mean-field equivalence')


def encoded():
    return json.dumps(make_plan(),indent=2,allow_nan=False)+'\n'


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--verify',action='store_true')
    args=parser.parse_args()
    path=HERE/'sir-plan.json'
    if args.verify:
        assert path.read_text()==encoded(),'SIR plan changed'
        assert json.loads(encoded())==make_plan(),'SIR plan is not JSON-native'
    else:
        path.write_text(encoded())
