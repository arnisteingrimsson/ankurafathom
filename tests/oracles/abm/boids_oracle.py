"""Pinned synchronous Mesa Boids, independent geometry, and offline acceptance gates."""
import argparse
import bisect
import copy
import hashlib
import importlib.metadata
import json
import math
from pathlib import Path
import statistics
import sys
from boids_plan import HERE, encoded as encoded_plan, make_plan

REFERENCE = HERE/'boids-reference.json'


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata():
    return dict(plan_sha256=digest(HERE/'boids-plan.json'), adapter_sha256=digest(Path(__file__)),
                requirements_sha256=digest(HERE/'requirements.txt'), engines=dict(Mesa='3.5.1', numpy='2.5.3'))


def encode(value):
    return json.dumps(value, separators=(',', ':'), sort_keys=True, allow_nan=False)+'\n'


def validate_state(case, agents):
    p = case['parameters']
    require(isinstance(agents, list) and len(agents) == case['n'], 'Boids population count')
    for a in agents:
        require(isinstance(a, list) and len(a) == 4 and all(type(v) in (int,float) and math.isfinite(v) for v in a), 'Boids finite record')
        require(0 <= a[0] < p['width'] and 0 <= a[1] < p['height'], 'Boids canonical coordinates')
        require(math.hypot(a[2],a[3])/p['max_speed'] <= 1+4e-15, 'Boids speed invariant')


def metrics(case, agents):
    p, n = case['parameters'], len(agents)
    speed = [math.hypot(a[2],a[3]) for a in agents]
    ux = sum(a[2]/v if v else 0 for a,v in zip(agents,speed))/n
    uy = sum(a[3]/v if v else 0 for a,v in zip(agents,speed))/n
    distances = []
    for i,a in enumerate(agents):
        for b in agents[i+1:]:
            dx,dy = abs(a[0]-b[0]),abs(a[1]-b[1])
            if p['wrap']:
                dx,dy = min(dx,p['width']-dx),min(dy,p['height']-dy)
            distances.append(math.hypot(dx,dy))
    diameter = math.hypot(p['width'],p['height'])/(2 if p['wrap'] else 1)
    return dict(polarization=math.hypot(ux,uy), mean_speed=sum(speed)/(n*p['max_speed']),
                neighbor_fraction=sum(d <= p['vision'] for d in distances)/len(distances),
                mean_pair_distance=sum(distances)/(len(distances)*diameter))


def generate():
    for line in (HERE/'requirements.txt').read_text().splitlines():
        package,version = line.split('==')
        require(importlib.metadata.version(package) == version, 'unpinned '+package)
    import mesa
    from mesa.space import ContinuousSpace
    import numpy as np

    def bounded(vector, maximum):
        length = math.hypot(*vector)
        return vector*(maximum/length) if length > maximum else vector

    class Bird(mesa.Agent):
        def __init__(self, model, index, position, velocity):
            super().__init__(model)
            self.index, self.velocity = index, np.array(velocity, dtype=float)
            model.space.place_agent(self, tuple(position))

        def compute(self):
            p, space = self.model.p, self.model.space
            peers = sorted((a for a in space.get_neighbors(self.pos, p['vision'], include_center=True) if a is not self), key=lambda a:a.index)
            displacement, close = [], []
            for other in peers:
                delta = np.array(space.get_heading(self.pos,other.pos), dtype=float)
                # Mesa chooses the opposite half-box tie. This declared variant
                # retains the original displacement sign, including exact ties.
                if p['wrap']:
                    for d,length in enumerate([p['width'],p['height']]):
                        raw = other.pos[d]-self.pos[d]
                        if abs(raw) == length/2:
                            delta[d] = raw
                displacement.append(delta)
                if math.hypot(*delta) <= p['separation_radius']:
                    close.append(-delta/(float(np.dot(delta,delta))+p['softening']**2))
            force = np.zeros(2)
            if peers:
                force += p['alignment']*(np.mean([a.velocity for a in peers],axis=0)-self.velocity)
                force += p['cohesion']*np.mean(displacement,axis=0)
            if close:
                force += p['separation']*np.mean(close,axis=0)
            velocity = bounded(self.velocity+p['dt']*bounded(force,p['max_acceleration']),p['max_speed'])
            position = np.array(self.pos)+p['dt']*velocity
            for d,length in enumerate([p['width'],p['height']]):
                if p['wrap']:
                    position[d] = float(position[d]) % length
                    if position[d] >= length:
                        position[d] = 0
                else:
                    folded = float(position[d]) % (2*length)
                    if folded == 0 or folded >= 2*length:
                        position[d],velocity[d] = 0,abs(velocity[d])
                    elif folded == length:
                        position[d],velocity[d] = math.nextafter(length,0),-abs(velocity[d])
                    elif folded < length:
                        position[d] = folded
                    else:
                        position[d],velocity[d] = 2*length-folded,-velocity[d]
            self.next_position,self.next_velocity = tuple(map(float,position)),velocity

        def commit(self):
            self.velocity = self.next_velocity
            self.model.space.move_agent(self,self.next_position)

    class Flock(mesa.Model):
        def __init__(self, case, seed):
            super().__init__(seed=seed)
            self.case,self.p = case,case['parameters']
            p = self.p
            self.space = ContinuousSpace(p['width'],p['height'],p['wrap'])
            self.birds = []
            for i in range(case['n']):
                position = [self.random.random()*p['width'],self.random.random()*p['height']]
                velocity = [(self.random.random()*2-1)*(p['max_speed']/2) for _ in range(2)]
                self.birds.append(Bird(self,i,position,velocity))

        def state(self):
            return [[float(v) for v in (*a.pos,*a.velocity)] for a in self.birds]

        def step(self):
            self.agents.do('compute')
            self.agents.do('commit')
            validate_state(self.case,self.state())

        def summaries(self):
            p,n = self.p,len(self.birds)
            speed = [math.hypot(*a.velocity) for a in self.birds]
            headings = [a.velocity/v if v else np.zeros(2) for a,v in zip(self.birds,speed)]
            distances = [float(self.space.get_distance(a.pos,b.pos)) for i,a in enumerate(self.birds) for b in self.birds[i+1:]]
            neighbors = sum(len(self.space.get_neighbors(a.pos,p['vision'],include_center=True))-1 for a in self.birds)
            return dict(polarization=float(np.linalg.norm(np.mean(headings,axis=0))),
                        mean_speed=float(np.mean(speed))/p['max_speed'],neighbor_fraction=neighbors/(n*(n-1)),
                        mean_pair_distance=float(np.mean(distances))/(math.hypot(p['width'],p['height'])/(2 if p['wrap'] else 1)))

    plan,rows = make_plan(),[]
    for index,case in enumerate(plan['cases']):
        for replication in range(plan['replications']):
            model = Flock(case,plan['reference_seed']+1000003*index+replication)
            tick = 0
            for observation in plan['ticks']:
                while tick < observation:
                    model.step(); tick += 1
                rows.append(dict(case=case['id'],replication=replication,tick=tick,agents=model.state(),metrics=model.summaries()))
    return dict(metadata=metadata(),rows=rows)


def validate_rows(rows, reference=False, paths=False):
    plan = make_plan()
    expected = [(c,r,t) for c in plan['cases'] for r in ([0,1,plan['replications']-1] if paths else range(plan['replications'])) for t in plan['ticks']]
    require(len(rows) == len(expected), 'Boids observation count')
    for row,(case,replication,tick) in zip(rows,expected):
        require(set(row) == {'case','replication','tick','agents'} | ({'metrics'} if reference else {'path'}), 'Boids row fields')
        require(row['case'] == case['id'] and type(row['replication']) is int and row['replication'] == replication
                and type(row['tick']) is int and row['tick'] == tick, 'Boids observation identity/order')
        if not reference:
            require(type(row['path']) is bool and row['path'] == paths, 'Boids path identity')
        validate_state(case,row['agents'])
        if reference:
            require(set(row['metrics']) == set(plan['metrics']), 'Boids metric names')
            derived = metrics(case,row['agents'])
            for name,value in row['metrics'].items():
                require(type(value) in (int,float) and math.isfinite(value) and abs(value-derived[name]) < 2e-14, 'Mesa/pairwise Boids metric mismatch')


def read_reference():
    value = json.loads(REFERENCE.read_text())
    require(set(value) == {'metadata','rows'} and value['metadata'] == metadata(), 'stale Boids reference')
    validate_rows(value['rows'],reference=True)
    return value


def verify():
    reference,regenerated = read_reference(),generate()
    require(regenerated['metadata'] == reference['metadata'], 'Boids regeneration metadata differs')
    validate_rows(regenerated['rows'],reference=True)
    # Floating trajectories follow the plan's numerical contract; do not impose
    # byte identity across NumPy/BLAS implementations on different CPU platforms.
    tolerance = make_plan()['path_tolerance']
    maximum,count = 0.,0
    for actual,wanted in zip(regenerated['rows'],reference['rows']):
        for a,b in zip(actual['agents'],wanted['agents']):
            for x,y in zip(a,b):
                maximum = max(maximum,abs(x-y)); count += 1
                require(math.isclose(x,y,rel_tol=tolerance['relative'],abs_tol=tolerance['absolute']), 'Boids regeneration trajectory differs')
        for name,x in actual['metrics'].items():
            require(math.isclose(x,wanted['metrics'][name],rel_tol=tolerance['relative'],abs_tol=tolerance['absolute']), 'Boids regeneration metric differs')
    print(f'Boids pinned regeneration: {count} state values, maximum error {maximum:g}')


def ks(a,b):
    a,b = sorted(a),sorted(b)
    return max(abs(bisect.bisect_right(a,x)/len(a)-bisect.bisect_right(b,x)/len(b)) for x in set(a+b))


def compare(rows,reference):
    plan = make_plan()
    cases = {c['id']:c for c in plan['cases']}
    samples,ref = {},{}
    for row in rows:
        samples.setdefault((row['case'],row['tick']),[]).append(metrics(cases[row['case']],row['agents']))
    for row in reference:
        ref.setdefault((row['case'],row['tick']),[]).append(row['metrics'])
    n,policy = plan['replications'],plan['gates']
    limit = math.sqrt(math.log(2*policy['comparisons']/policy['family_alpha'])/n)
    gates = []
    for case in plan['cases']:
        for tick in plan['ticks'][1:]:
            for metric in plan['metrics']:
                a = [v[metric] for v in samples[case['id'],tick]]
                b = [v[metric] for v in ref[case['id'],tick]]
                ma,mb = statistics.mean(a),statistics.mean(b)
                se = math.sqrt((statistics.variance(a)+statistics.variance(b))/n)
                mean_limit = max(policy['mean_floor'][metric],policy['mean_sigma']*se)
                distance = ks(a,b)
                gates.append(dict(case=case['id'],tick=tick,metric=metric,native_mean=ma,reference_mean=mb,
                                  mean_gap=abs(ma-mb),mean_limit=mean_limit,difference_ci95=[ma-mb-1.96*se,ma-mb+1.96*se],
                                  ks=distance,ks_limit=limit,passed=abs(ma-mb) <= mean_limit and distance <= limit))
    require(len(gates) == policy['comparisons'], 'Boids gate count drift')
    return gates


def initializer(rows):
    sys.path.insert(0,str(HERE.parents[1]))
    from abm_ir_contract import word
    plan = make_plan()
    cases = {c['id']:c for c in plan['cases']}
    count = 0
    for row in rows:
        if row['tick'] or row['replication'] not in [0,1,plan['replications']-1]:
            continue
        p = cases[row['case']]['parameters']
        for entity,actual in enumerate(row['agents']):
            def u(stream,axis):
                return (word(plan['seed'],plan['scenario'],row['replication'],entity,axis << 16,stream)+.5)/2**32
            expected = [p['width']*u(plan['position_stream'],0),p['height']*u(plan['position_stream'],1),
                        (p['max_speed']/2)*(2*u(plan['velocity_stream'],0)-1),
                        (p['max_speed']/2)*(2*u(plan['velocity_stream'],1)-1)]
            require(actual == expected, 'Boids addressed initial state differs')
            count += 4
    return count


def paired(paths,reference):
    expected = {(r['case'],r['replication'],r['tick']):r['agents'] for r in reference}
    tolerance = make_plan()['path_tolerance']
    count,maximum = 0,0.
    for row in paths:
        for actual,wanted in zip(row['agents'],expected[row['case'],row['replication'],row['tick']]):
            for a,b in zip(actual,wanted):
                maximum = max(maximum,abs(a-b))
                require(math.isclose(a,b,rel_tol=tolerance['relative'],abs_tol=tolerance['absolute']), 'paired Mesa Boids path differs')
                count += 1
    return count,maximum


def contract():
    reference = read_reference()
    for mutation in [lambda r:r['rows'].pop(),lambda r:r['metadata'].__setitem__('plan_sha256','stale'),
                     lambda r:r['rows'][1].__setitem__('tick',99),
                     lambda r:r['rows'][1]['agents'][0].__setitem__(0,-1),
                     lambda r:r['rows'][1]['agents'][0].__setitem__(2,999),
                     lambda r:r['rows'][1]['agents'][0].__setitem__(3,float('nan')),
                     lambda r:r['rows'][1]['metrics'].__setitem__('polarization',99)]:
        bad = copy.deepcopy(reference); mutation(bad)
        try:
            require(bad['metadata'] == metadata(),'stale metadata'); validate_rows(bad['rows'],reference=True)
        except ValueError:
            continue
        raise ValueError('corrupt Boids reference accepted')
    case = dict(parameters=dict(width=4,height=4,wrap=True,max_speed=2,vision=1))
    values = metrics(case,[[.5,1,1,0],[3.5,1,1,0]])
    require(values == dict(polarization=1,mean_speed=.5,neighbor_fraction=1,mean_pair_distance=1/math.sqrt(8)), 'Boids hand metrics')
    values = metrics(case,[[.5,1,1,0],[.5,1,-1,0]])
    require(values['polarization'] == 0 and values['mean_pair_distance'] == 0, 'Boids colocated/opposed metrics')
    require(ks([0,0],[1,1]) == 1 and ks([0,1],[0,1]) == 0, 'KS contract')
    initial = {(r['case'],r['replication']):r['agents'] for r in reference['rows'] if r['tick'] == 0}
    frozen = [dict(case=r['case'],replication=r['replication'],tick=r['tick'],path=False,
                   agents=initial[r['case'],r['replication']]) for r in reference['rows']]
    validate_rows(frozen)
    require(any(not g['passed'] for g in compare(frozen,reference['rows'])), 'frozen Boids dynamics passed gates')
    print('Boids reference, metric, corruption and wrong-dynamics contracts passed')


def score(path,report):
    with Path(path).open() as file:
        require(json.loads(next(file)) == dict(plan=make_plan()), 'native Boids plan differs')
        data = [json.loads(line) for line in file]
    count = len(make_plan()['cases'])*make_plan()['replications']*len(make_plan()['ticks'])
    rows,paths = data[:count],data[count:]
    validate_rows(rows); validate_rows(paths,paths=True)
    reference = read_reference()
    exact = initializer(rows)
    comparisons,error = paired(paths,reference['rows'])
    gates = compare(rows,reference['rows'])
    result = dict(metadata=metadata(),runs_per_engine=2048,initializer_values=exact,paired_values=comparisons,
                  maximum_path_error=error,gates=gates,passed=all(g['passed'] for g in gates))
    if report:
        Path(report).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    require(result['passed'], 'Boids distribution gates failed: '+str([g for g in gates if not g['passed']]))
    print(f'Boids: {len(gates)} gates, 2048 runs/engine, {comparisons} Mesa path values (max error {error:g}), {exact} exact initializer values passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for flag in ['write','verify','contract']:
        parser.add_argument('--'+flag,action='store_true')
    parser.add_argument('--native'); parser.add_argument('--report')
    args = parser.parse_args()
    require((HERE/'boids-plan.json').read_text() == encoded_plan(), 'stale Boids plan')
    if args.write: REFERENCE.write_text(encode(generate()))
    if args.verify: verify()
    if args.contract: contract()
    if args.native: score(args.native,args.report)
