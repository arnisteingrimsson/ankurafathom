"""C17R bounded engine contract: independent particle transport and Taylor ODE oracle."""
import argparse
import copy
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import subprocess

SOURCE = 'https://www.sne-journal.org/fileadmin/user_upload_sne/SNE_Issues_OA/SNE_25_1/articles/sne.25.1.10283.bn17r.OA.pdf'
OFFSETS = [(1, 0), (0, 1), (-1, 1), (-1, 0), (0, -1), (1, -1)]


def uniform(word):
    return (word + .5) / 2**32


def local_oracle(spec):
    state = list(spec['state'])
    occupied = {i for i, s in enumerate(state) if s >= 0}
    # Enumerated FHP-I incoming configurations, independently of native bit masks.
    if spec['collision']:
        rotation = 0
        if occupied in ({0, 3}, {1, 4}, {2, 5}):
            rotation = 1 if spec['u'] < .5 else -1
        elif occupied in ({0, 2, 4}, {1, 3, 5}):
            rotation = 1
        moved = [-1] * 6
        for i, value in enumerate(state):
            moved[(i + rotation) % 6] = value
        state = moved
    infection = 1 - (1 - spec['alpha'])**state.count(1)
    return [1 if s == 0 and uniform(w) < infection else
            2 if s == 1 and uniform(w) < spec['beta'] else s
            for s, w in zip(state, spec['words'])]


def oracle(spec):
    n = spec['n']
    # Sparse particles pushed to their destinations, unlike the native dense
    # cell population pulling each incoming channel from a frozen snapshot.
    particles = {(c % n, c // n, d): s for c, cell in enumerate(spec['initial'])
                 for d, s in enumerate(cell) if s >= 0}
    total = len(particles)
    policy = spec['intervention']
    trigger = None

    def snapshot():
        states = [[-1] * 6 for _ in range(n*n)]
        for (x, y, d), s in particles.items():
            states[y*n+x][d] = s
        return dict(states=states, counts=[sum(s == k for s in particles.values()) for k in range(3)])

    result = dict(samples=[snapshot()], events=[], rates=[])
    for tick in range(spec['steps']):
        infected = sum(s == 1 for s in particles.values())
        if policy['kind'] != 'none' and trigger is None and infected >= policy['threshold']*total:
            trigger = tick
            changed = 0
            if policy['kind'] == 'hard':
                rank = {slot: order for order, slot in enumerate(policy['priority'])}
                candidates = sorted((key for key, s in particles.items() if s == policy['target']),
                                    key=lambda key: rank[6*(key[1]*n+key[0])+key[2]])
                changed = math.floor(len(candidates)*policy['fraction'])
                for key in candidates[:changed]:
                    particles[key] = 2
            result['events'].append(dict(time=tick, infected_before=infected, changed=changed))
        alpha, beta = spec['alpha'], spec['beta']
        if policy['kind'] == 'soft' and trigger is not None:
            fraction = min(1., (tick-trigger)/policy['duration']) if policy['duration'] else 1.
            if policy['shape'] == 'smooth':
                fraction = 3*fraction**2-2*fraction**3
            multiplier = 1 + (policy['fraction']-1)*fraction
            if policy['parameter'] == 'alpha':
                alpha *= multiplier
            else:
                beta *= multiplier
        result['rates'].append([alpha, beta])
        grouped = {}
        for (x, y, d), s in particles.items():
            ox, oy = OFFSETS[d]
            destination = ((x+ox) % n, (y+oy) % n)
            bucket = grouped.setdefault(destination, {})
            assert d not in bucket
            bucket[d] = s
        particles = {}
        for (x, y), bucket in grouped.items():
            cell_index = y*n+x
            base = 7*(tick*n*n+cell_index)
            words = spec['words'][base:base+6]
            state = [bucket.get(d, -1) for d in range(6)]
            if spec['movement'] == 'diffusion':
                destinations = spec['permutations'][tick*n*n+cell_index]
                shuffled = [-1]*6
                for d, s in enumerate(state):
                    shuffled[destinations[d]] = s
                state = shuffled
            updated = local_oracle(dict(state=state, words=words, alpha=alpha, beta=beta,
                                        collision=spec['movement'] == 'fhp', u=uniform(spec['words'][base+6])))
            for d, s in enumerate(updated):
                if s >= 0:
                    particles[x, y, d] = s
        result['samples'].append(snapshot())
    return result


def check_ca(actual, spec):
    expected = oracle(spec)
    assert set(actual) == set(expected)
    assert len(actual['samples']) == spec['steps']+1
    assert len(actual['rates']) == spec['steps']
    assert actual['events'] == expected['events'], (spec['id'], 'interventions')
    comparisons = 0
    previous = None
    total = sum(expected['samples'][0]['counts'])
    for tick, (a, e) in enumerate(zip(actual['samples'], expected['samples'])):
        assert a == e, (spec['id'], tick, 'full lattice mismatch')
        assert len(a['states']) == spec['n']**2 and all(len(cell) == 6 for cell in a['states'])
        counts = a['counts']
        assert sum(counts) == total
        if previous:
            assert counts[0] <= previous[0] and counts[2] >= previous[2]
        previous = counts
        comparisons += 6*spec['n']**2+3
    for a, e in zip(actual['rates'], expected['rates']):
        assert all(abs(x-y) <= 2e-16 for x, y in zip(a, e))
        comparisons += 2
    return comparisons


def make_case(name, n, count, infected, steps=40, seed=17, movement='fhp', alpha=.1, beta=.1, policy=None):
    rng = random.Random(seed)
    slots = rng.sample(range(6*n*n), count)
    initial = [[-1]*6 for _ in range(n*n)]
    for index, slot in enumerate(slots):
        initial[slot//6][slot % 6] = int(index < infected)
    policy = dict(policy or dict(kind='none'))
    if policy['kind'] == 'hard':
        # Separate independent seed, before any trajectory is observed.
        priority = list(range(6*n*n))
        random.Random(seed+987654).shuffle(priority)
        policy['priority'] = priority
    result = dict(mode='ca', id=name, n=n, steps=steps, initial=initial, movement=movement,
                  alpha=alpha, beta=beta, intervention=policy,
                  words=[rng.getrandbits(32) for _ in range(steps*n*n*7)])
    if movement == 'diffusion':
        result['permutations'] = []
        for _ in range(steps*n*n):
            permutation = list(range(6))
            rng.shuffle(permutation)
            result['permutations'].append(permutation)
    return result


def taylor_reference(spec, dt):
    """Order-18 power-series recurrence; no RK stages or native SD machinery."""
    state = list(spec['initial'])
    gamma = -spec['contacts']*math.log1p(-spec['alpha'])/sum(state)
    delta = -math.log1p(-spec['beta'])
    samples = [state[:]]
    for _ in range(spec['horizon']):
        for _ in range(round(1/dt)):
            s, i, r = [state[0]], [state[1]], [state[2]]
            for k in range(18):
                incidence = gamma*math.fsum(s[j]*i[k-j] for j in range(k+1))
                recovery = delta*i[k]
                s.append(-incidence/(k+1))
                i.append((incidence-recovery)/(k+1))
                r.append(recovery/(k+1))
            state = [math.fsum(c*dt**k for k, c in enumerate(series)) for series in (s, i, r)]
        samples.append(state[:])
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    args.destination.mkdir(parents=True, exist_ok=False)
    input_hashes = {}

    def execute(spec, name):
        path = args.destination/(name+'.input.json')
        path.write_text(json.dumps(spec, separators=(',', ':'))+'\n')
        input_hashes[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        raw = subprocess.check_output([str(args.executable.resolve()), str(path)], text=True)
        (args.destination/(name+'.native.json')).write_text(raw)
        return json.loads(raw)

    # All policies/gates fixed here before native execution. Once-only, at integer
    # boundaries before transport; floor(f_H * eligible) selected without replacement.
    policies = [dict(kind='none')]
    policies += [dict(kind='hard', threshold=.12, fraction=f, target=target)
                 for target in (0, 1) for f in (.5, 1.)]
    policies += [dict(kind='soft', threshold=.12, fraction=.2, parameter=parameter,
                      duration=duration, shape=shape)
                 for parameter in ('alpha', 'beta') for duration, shape in ((0, 'linear'), (4, 'linear'), (4, 'smooth'))]
    cases = [make_case(f'{movement}-policy-{index}', 8, 300, 30, movement=movement, policy=policy)
             for movement in ('fhp', 'diffusion') for index, policy in enumerate(policies)]
    cases += [make_case(f'published-baseline-{seed}', 46, 10000, 500, steps=100, seed=seed) for seed in (21, 37)]
    cases += [make_case('fully-occupied', 4, 96, 48, steps=12),
              make_case('no-transmission', 4, 60, 10, alpha=0, beta=1, steps=6),
              make_case('no-infected', 4, 60, 0, steps=6),
              make_case('no-recovery', 4, 60, 10, alpha=1, beta=0, steps=6),
              make_case('threshold-equality', 4, 60, 12, steps=6,
                        policy=dict(kind='hard', threshold=.2, fraction=1., target=1)),
              make_case('threshold-unreached', 4, 60, 0, steps=6,
                        policy=dict(kind='hard', threshold=.2, fraction=1., target=1))]
    # Fully susceptible incoming cell plus one infectious particle that also
    # recovers: newly infected particles must not recover in this same tick.
    controls = [dict(state=[1, 0, 0, 0, 0, 0], words=[0]*6, alpha=1., beta=1., collision=False, u=.25)]
    controls += [dict(state=list(state), words=[word]*6, alpha=.25, beta=.5, collision=False, u=.25)
                 for state in itertools.product((-1, 0, 1, 2), repeat=6) for word in (0, 2**31, 2**32-1)]
    collision_start = len(controls)
    controls += [dict(state=[d % 3 if mask & (1 << d) else -1 for d in range(6)],
                      words=[0]*6, alpha=0., beta=0., collision=True, u=u)
                 for mask in range(64) for u in (.25, .75)]
    plan = dict(source=SOURCE, source_sha256='99fb09e8783afeb47b433a568a1dcc9c46f1ab9777c5ba5d353a53026a4f356b',
                ca_cases=[{k: v for k, v in c.items() if k not in ('initial', 'words', 'permutations')} for c in cases],
                local_cases=len(controls), ode_cases=12, ode_gate='max absolute state error <= 1e-7 + 5*dt^4; successive error ratio >= 10 above floor',
                fast_ode_gate='alpha=.35: dt=1 rejected at original 5.0000001 bound; dt=1/32 rejected at fixed 1e-4-person accuracy; accepted refinements 1/64..1/512 retain that fixed accuracy (1e-8 of population)',
                semantics='Periodic axial hex grid; stream, collision, synchronous disease; once-only intervention at integer boundary before stream',
                baseline='N=10000, S=9500, I=500, alpha=beta=.1, nominal C=4; explicit n=46, actual uniform expected C=5*9999/(6*46^2-1)',
                scope='Frozen shared inputs validate paths; no independent-RNG distributional docking')
    (args.destination/'plan.json').write_text(json.dumps(plan, indent=2)+'\n')
    local_result = execute(dict(mode='local', cases=controls), 'local')
    assert len(local_result) == len(controls)
    assert local_result[0] == [2, 1, 1, 1, 1, 1]
    for index, (actual, control) in enumerate(zip(local_result, controls)):
        assert actual == local_oracle(control), ('local', index)
        if index >= collision_start:
            assert sorted(actual) == sorted(control['state'])
            for axis in (0, 1):
                assert sum(OFFSETS[d][axis] for d, s in enumerate(actual) if s >= 0) == sum(
                    OFFSETS[d][axis] for d, s in enumerate(control['state']) if s >= 0)
    reports = []
    for spec in cases:
        actual = execute(spec, spec['id'])
        checks = check_ca(actual, spec)
        if spec['id'] == 'no-transmission':
            assert actual['samples'][1]['counts'] == [50, 0, 10]
        if spec['id'] == 'threshold-equality':
            assert actual['events'] == [dict(time=0, infected_before=12, changed=12)]
            assert actual['samples'][-1]['counts'] == [48, 0, 12]
        if spec['id'] == 'threshold-unreached':
            assert actual['events'] == []
        if '-policy-' in spec['id'] and spec['intervention']['kind'] != 'none':
            assert len(actual['events']) == 1, 'intervention fixture failed to exercise trigger'
        reports.append(dict(case=spec['id'], comparisons=checks, interventions=actual['events'],
                            final_counts=actual['samples'][-1]['counts']))
    # Check that the validator rejects both a plausible conservative state error
    # and a truncated run. Original output files remain unchanged.
    spec = cases[0]
    actual = json.loads((args.destination/(spec['id']+'.native.json')).read_text())
    mutations = [copy.deepcopy(actual), copy.deepcopy(actual)]
    flat = list(itertools.chain.from_iterable(mutations[0]['samples'][1]['states']))
    left, right = next((i, j) for i in range(len(flat)) for j in range(i+1, len(flat)) if flat[i] != flat[j])
    state = mutations[0]['samples'][1]['states']
    state[left//6][left % 6], state[right//6][right % 6] = state[right//6][right % 6], state[left//6][left % 6]
    mutations[1]['samples'].pop()
    for mutated in mutations:
        try:
            check_ca(mutated, spec)
        except AssertionError:
            pass
        else:
            raise AssertionError('corrupt trajectory accepted')
    ode_reports = []
    coarse_rejected = []
    for alpha, beta in ((.1, .1), (.01, .02), (.35, .1)):
        base = dict(mode='ode', initial=[9500., 500., 0.], contacts=4., alpha=alpha, beta=beta, horizon=100)
        reference = taylor_reference(base, .25)
        refined = taylor_reference(base, .125)
        ref_gap = max(abs(a-b) for x, y in zip(reference, refined) for a, b in zip(x, y))
        assert ref_gap < 5e-9, ref_gap
        previous = None
        if alpha == .35:
            coarse = execute(dict(base, substeps=1), 'ode-fast-coarse-control')
            coarse_gap = max(abs(a-b) for x, y in zip(coarse['samples'], refined) for a, b in zip(x, y))
            assert coarse_gap > 5.0000001, 'coarse-step rejection fixture changed'
            coarse_rejected.append(dict(alpha=alpha, dt=1., error=coarse_gap, rejected_bound=5.0000001))
            insufficient = execute(dict(base, substeps=32), 'ode-fast-refinement-control')
            insufficient_gap = max(abs(a-b) for x, y in zip(insufficient['samples'], refined) for a, b in zip(x, y))
            assert insufficient_gap > 1e-4
            coarse_rejected.append(dict(alpha=alpha, dt=1/32, error=insufficient_gap, rejected_bound=1e-4))
        for substeps in ((64, 128, 256, 512) if alpha == .35 else (1, 2, 4, 8)):
            spec = dict(base, substeps=substeps)
            native = execute(spec, f'ode-{alpha}-{beta}-{substeps}')
            assert len(native['samples']) == 101 and all(len(row) == 3 for row in native['samples'])
            assert abs(native['gamma']+4*math.log1p(-alpha)/10000) < 1e-18
            assert abs(native['delta']+math.log1p(-beta)) < 1e-16
            gap = max(abs(a-b) for x, y in zip(native['samples'], refined) for a, b in zip(x, y))
            tolerance = 1e-4 if alpha == .35 else 1e-7+5/substeps**4
            assert gap <= tolerance, (spec, gap, tolerance)
            if previous is not None and gap > 1e-7:
                assert previous/gap >= 10, (previous, gap)
            for row in native['samples']:
                assert min(row) >= 0 and abs(sum(row)-10000) < 1e-7
            ode_reports.append(dict(alpha=alpha, beta=beta, substeps=substeps, maximum_error=gap,
                                    tolerance=tolerance, reference_refinement_gap=ref_gap))
            previous = gap
    report = dict(verdict='pass', source=SOURCE, ca_cases=len(cases), local_cases=len(controls), ode_cases=len(ode_reports),
                  state_and_rate_comparisons=len(controls)*6+sum(r['comparisons'] for r in reports),
                  comparison_semantics='States/counts/events exact; soft-intervention rates within 2e-16 absolute',
                  ode_state_comparisons=303*len(ode_reports), corruption_controls_rejected=len(mutations),
                  coarse_accuracy_controls_rejected=coarse_rejected, source_sha256=plan['source_sha256'],
                  cases=reports, ode=ode_reports, input_sha256=input_hashes,
                  scope=plan['semantics'],
                  unassessed=['Full parameter-region and alpha*C tradeoff studies', 'Global random mixing and finite-size ensemble comparisons',
                              'Repeated or spatially targeted interventions and ODE intervention event handling',
                              'Published executable docking; general production hex-lattice/declarative support'],
                  out_of_scope=['GUI interactions', 'Display rendering', 'Graphical publication artifacts'])
    (args.destination/'validation.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k: v for k, v in report.items() if k not in ('cases', 'ode', 'input_sha256')}))


if __name__ == '__main__':
    main()
