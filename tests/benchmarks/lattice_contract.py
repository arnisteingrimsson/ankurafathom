"""Finite lattice oracles: exhaustive connectivity and Ising detailed balance."""
import argparse
import json
import math
from pathlib import Path
import subprocess


def connected(mask, n):
    # Independent union-find with explicit right/down edges; no native grid or CA.
    parent = list(range(n*n))
    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for y in range(n):
        for x in range(n):
            i = y*n+x
            if not mask >> i & 1:
                continue
            for xx, yy in ((x+1, y), (x, y+1)):
                if xx < n and yy < n and mask >> (yy*n+xx) & 1:
                    parent[root(i)] = root(yy*n+xx)
    burning = {root(y*n) for y in range(n) if mask >> (y*n) & 1}
    return sum(1 << i for i in range(n*n) if mask >> i & 1 and root(i) in burning)


def energy(mask, n):
    spin = [1 if mask >> i & 1 else -1 for i in range(n*n)]
    return -sum(spin[y*n+x]*(spin[y*n+(x+1)%n]+spin[((y+1)%n)*n+x])
                for y in range(n) for x in range(n))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    args.destination.mkdir(parents=True, exist_ok=False)
    plan = dict(percolation_sizes=[3, 4], ising_size=3, temperatures=[1., 2.269185314213022, 4.],
                numeric_tolerance=2e-13,
                scope='Exhaustive finite-state workload correctness, not critical-threshold estimation')
    (args.destination/'plan.json').write_text(json.dumps(plan, indent=2)+'\n')
    raw = subprocess.check_output([str(args.executable.resolve())], text=True)
    (args.destination/'native.json').write_text(raw)
    native = json.loads(raw)
    assert [r['size'] for r in native['percolation']] == plan['percolation_sizes']
    assert [r['temperature'] for r in native['ising']] == plan['temperatures']
    percolation = []
    for run in native['percolation']:
        n = run['size']; counts = [0]*(n*n+1)
        assert len(run['reachable']) == 1 << (n*n)
        for mask, actual in enumerate(run['reachable']):
            assert actual == connected(mask, n), (n, mask, actual)
            if any(actual >> (y*n+n-1) & 1 for y in range(n)):
                counts[mask.bit_count()] += 1
        # Exact polynomial sum c_k p^k (1-p)^(N-k), including p=0,1.
        assert counts[0] == 0 and counts[-1] == 1
        probabilities = {str(p):sum(c*p**k*(1-p)**(n*n-k) for k,c in enumerate(counts)) for p in (0., .5, 1.)}
        percolation.append(dict(size=n,states=len(run['reachable']),crossing_coefficients=counts,probabilities=probabilities))
    ising = []; attempts = 0; balance_checks = 0
    for run in native['ising']:
        n = run['size']; sites=n*n; temperature=run['temperature']; states=run['states']
        assert n == plan['ising_size'] and len(states) == 1 << sites
        energies = [energy(mask,n) for mask in range(1 << sites)]
        weights = [math.exp(-e/temperature) for e in energies]; z=sum(weights)
        probabilities = [w/z for w in weights]
        incoming = [0.] * len(states)
        for mask, state in enumerate(states):
            assert state['energy'] == energies[mask] and state['magnetization'] == 2*mask.bit_count()-sites
            assert len(state['acceptance']) == sites and len(state['endpoints']) == sites*2
            outward = 0.
            for site, actual in enumerate(state['acceptance']):
                flipped = mask ^ (1 << site)
                expected = min(1., math.exp(-(energies[flipped]-energies[mask])/temperature))
                assert math.isclose(actual, expected, rel_tol=2e-13, abs_tol=2e-13)
                for j,u in enumerate((.25,.75)):
                    assert state['endpoints'][site*2+j] == (flipped if u < expected else mask)
                    attempts += 1
                flow = probabilities[mask]*actual/sites
                reverse = probabilities[flipped]*states[flipped]['acceptance'][site]/sites
                assert math.isclose(flow,reverse,rel_tol=2e-13,abs_tol=2e-13)
                incoming[flipped] += flow; outward += actual/sites; balance_checks += 1
            assert -2e-13 <= 1-outward <= 1+2e-13
            incoming[mask] += probabilities[mask]*(1-outward)
        assert max(abs(a-b) for a,b in zip(incoming,probabilities)) < 2e-13
        ising.append(dict(size=n,temperature=temperature,states=len(states),partition_function=z,
                          energy_per_spin=sum(p*e for p,e in zip(probabilities,energies))/sites,
                          absolute_magnetization_per_spin=sum(p*abs(s['magnetization']) for p,s in zip(probabilities,states))/sites))
    report = dict(verdict='pass',percolation=percolation,ising=ising,spin_attempts=attempts,detailed_balance_checks=balance_checks,
                  scope='Native GridSpace and SyncPopulation custom workloads; exact finite-state validation',
                  unassessed=['Infinite-lattice percolation threshold','Ising thermodynamic critical temperature',
                              'Long-run stochastic mixing and finite-size extrapolation','NetLogo executable parity'])
    (args.destination/'validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(verdict='pass',percolation_states=sum(r['states'] for r in percolation),spin_attempts=attempts,detailed_balance_checks=balance_checks)))


if __name__ == '__main__':
    main()
