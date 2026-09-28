# Declarative graph generators

Typed synchronous and asynchronous populations can initialize an undirected network
with Erdős–Rényi, Watts–Strogatz or Barabási–Albert generation. Run the example:

```sh
./build/fathom lint models/typed_abm_network_generator.ir.json
./build/fathom run models/typed_abm_network_generator.ir.json
```

The population declares one of these `network` objects:

```json
{"generator":{"kind":"erdos_renyi","probability":"density","stream":901}}
{"generator":{"kind":"watts_strogatz","degree":"4","probability":"0.25","stream":901}}
{"generator":{"kind":"barabasi_albert","m":"3","stream":901}}
```

Each numeric option is a dimensionless expression over model parameters. Lint
validates defaults without allocating a graph; scenario overrides are checked again
at initialization. Probability must lie in [0,1]. WS requires positive population
size and an even integer degree from zero to n−1. BA requires integer 1 ≤ m < n
and begins with a star on m+1 vertices. ER allows an empty population. Generated
networks cannot also declare explicit edges or a directed flag.

The required stream is an integer from 0 to 65535, distinct from all statechart rate
streams, including disabled rates. Generation uses initial stable IDs and the run's
seed, scenario and replication. It precedes chart initialization and time-zero
lifecycle/edge edits. Births add isolated vertices; retirement prunes incident edges.
The graph is generated once, then follows the existing mutable-network contracts.
See [semantics](SEMANTICS.md#declarative-random-graph-initialization).

## Evidence

The CLI contract checks **2,808 exact observations** using independently implemented
Python Philox addressing and graph construction. Powers-of-two neighbor sums identify
full adjacency, alongside degree checks. Cases cover all three generators, both
execution modes, two seeds/streams, multiple scenario/replication addresses, boundary
parameters, replay, reordered outputs/queries and observation density. Additional
checks cover override failures, stream conflicts, empty/singleton/complete graphs,
time-zero retirement and isolated birth, subsequent edge edits and duplicate edges.

The [frozen statistical plan](../tests/oracles/abm/generator-plan.json) was defined
before collecting samples. It contains nine cases, each with n=48 and 512 independent
replications per engine: ER p={.05,.3,.8}; WS (degree,p)={(4,0),(4,.25),(8,1)};
BA m={1,3,8}. Native Philox draws and pinned NetworkX 3.7 draws use separate seeds.
The native executable exports canonical edges; a separate standard-library Python
scorer calculates statistics. Reference statistics come from NetworkX APIs.

All **51 predeclared gates pass over 4,608 graphs per engine**:

- 45 paired metric gates compare density, normalized maximum degree, normalized
  degree second moment, transitivity and largest-component fraction. Each requires
  both a two-sample empirical-CDF distance below the fixed KS threshold and a mean
  difference below the predeclared six-standard-error bound or metric floor.
- Six ER analytical gates check the native and reference edge-count mean and variance
  against the binomial target. Mean tolerance is six standard errors; relative
  variance tolerance is .35.
- Every graph also satisfies simple-graph/range checks, exact WS/BA edge counts and
  BA connectivity. The KS threshold is approximately .149, using the plan's .001
  family-alpha setting across 45 comparisons. These finite cases are evidence for
  the declared variants, not proof for all population sizes and parameters.

Frozen references record plan, adapter and dependency hashes. Pinned regeneration
matches exactly locally; CI is configured to repeat it. Routine CTest needs no
NetworkX installation or network access. Results are written to
`build/abm-generator-report.json`.

```sh
ctest --test-dir build --output-on-failure -R '^abm_generator_'
.venv-abm-oracle/bin/python tests/oracles/abm/generator_oracle.py --verify
```

[Boltzmann wealth exchange](ABM_WEALTH.md), [Schelling segregation](ABM_SCHELLING.md),
[Boids/flocking](ABM_BOIDS.md), [Sugarscape-lite](ABM_SUGARSCAPE.md) and
[synchronous network SIR](ABM_SIR.md) now pass their declared canonical-model suites.
[Continuous-time SIR and sync/async convergence](ABM_SIR_ASYNC.md) now pass their
declared finite-graph gates; see [M4 acceptance](M4_ACCEPTANCE.md). Behavior-generated rewiring
and topic-handler lifecycle are separate unsupported extensions. See [status](STATUS.md)
for the current regression and sanitizer results.
