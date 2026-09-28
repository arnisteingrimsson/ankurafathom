# Schelling segregation

`abm::models::Schelling` is a native reference workload using typed population
records, spatial snapshots and transactional relocation. It supplies the second
canonical ABM comparison required by implementation-plan §7.4.

```sh
cmake --build build --target fathom_schelling
./build/fathom_schelling > build/schelling-example.csv
```

The example emits 408 agent observations: 24 agents on a 6×6 periodic grid at
sweeps 0 through 16. Every snapshot preserves 16 group-zero and eight group-one
agents, canonical coordinates, and single occupancy. The header is
[`include/ankurafathom/abm/models/schelling.hpp`](../include/ankurafathom/abm/models/schelling.hpp).

## Model contract

Each sweep visits all agents once in uniformly shuffled order. An agent is satisfied
when its same-group neighbors meet an exact rational fraction of all its neighbors.
The equality boundary is included, and isolated agents are satisfied. Neighborhoods
have radius one, exclude self, and use either Moore or von Neumann geometry with
periodic or bounded edges. Duplicate periodic cells are counted once.

A dissatisfied agent moves to a uniformly selected current vacancy. It does not
search for a satisfactory destination or move twice in the same sweep. Full grids
prevent relocation. Later activations observe earlier moves and can use newly
vacated cells. Vacancies are ordered by y then x for addressed replay. Each complete
sweep commits atomically, including its move count and clock; a later invalid
destination restores the entire pre-sweep state.

Agents keep binary groups and stable IDs. Initial positions must be canonical even
on periodic grids. Area is limited to one million cells, and the satisfaction
denominator is an integer in [1,1,000,000], with numerator between zero and the
denominator. Separate explicit order and relocation streams include seed, scenario,
replication and sweep; retries use unbiased integer rejection sampling. There are
65,536 addressable sweeps. `scripted_step` supports deterministic activation and
destination histories. Read-only observations consume no draws.

See [semantics](SEMANTICS.md#schelling-segregation-reference-model). This native API
does not add shuffled activation to typed ABM IR or establish equivalence to
synchronous or continuous-time variants.

## Independent validation

The [frozen plan](../tests/oracles/abm/schelling-plan.json) specifies four cases on
6×6 grids with the same fixed mixed initial population of 24 agents. Three periodic
Moore cases use satisfaction fractions 1/3, 1/2 and 2/3. A bounded von Neumann case
uses 1/2. Each engine runs 512 replications per case and records sweeps 0, 4 and 16:
**2,048 trajectories and 6,144 snapshots per engine**.

The independent implementation uses pinned Mesa 3.5.1 `Model`, `Agent`,
`AgentSet.shuffle_do` and `SingleGrid`, with independent random seeds. Neighbor
queries, vacancies and movements use Mesa APIs. Native observations export agent
groups/positions and move counts. A separate Python scorer uses pairwise geometry
and a connected-component traversal; it checks reference summaries against Mesa
neighborhoods and NetworkX 3.7 connected components.

**32 predeclared distribution gates pass**: four cases × two noninitial times ×
four metrics:

- Dissatisfied-agent fraction at the observation time.
- Mean same-group neighbor fraction, assigning similarity one to isolated agents.
- Largest connected same-group cluster as a fraction of all agents.
- Fraction of agents moved during the most recent sweep.

Each gate requires both the frozen mean-gap and empirical-CDF distance limits.
Maximum observed CDF distance is **.111328**, below the fixed **.147019** limit.
Mean tolerances use the larger of the metric floor and six standard errors.
Difference confidence intervals are diagnostics rather than individual coverage
requirements. The plan includes a conservative DKW/triangle-inequality power
calculation: approximately .01001 per-comparison miss probability for a true CDF
displacement of .30. These finite cases test material differences; they establish
neither universal parameter agreement nor equilibrium behavior.

Population count, each agent's group, coordinate bounds and single occupancy are
checked in native updates, reference sweeps and loaded observations. Independent
Python geometry/Philox recurrences match **864 agent states and 36 move counts**
across twelve runs. A further **16 agent states and four move counts** exercise high
entity/key bits and maximum scenario/replication/stream addresses.

Hand tests cover inclusive satisfaction thresholds, sequential satisfaction changes,
same-sweep vacancy reuse, self-cell exclusion, ordered activation histories,
Moore/von Neumann differences, seams and small periodic grids, full/empty/singleton
populations, zero thresholds, invalid parameters and schedules, copy isolation,
replay, observation invariance, clock exhaustion and late-failure rollback.

Scorer contracts reject stale metadata, missing/misidentified observations, altered
groups, collisions, invalid coordinates/counters and incorrect summaries. A frozen
but occupancy-preserving population fails the model gates. Reference files record
adapter, plan and dependency hashes; pinned regeneration matches exactly locally
and is configured in CI. Routine CTest remains offline.

```sh
ctest --test-dir build --output-on-failure -R '^abm_schelling'
.venv-abm-oracle/bin/python tests/oracles/abm/schelling_oracle.py --verify
```

The scorer writes `build/abm-schelling-report.json`. [M4 acceptance](M4_ACCEPTANCE.md)
records all five completed canonical-model suites and the declared continuous-time
SIR/sync–async convergence evidence. See
[status](STATUS.md) for regression and sanitizer evidence.
