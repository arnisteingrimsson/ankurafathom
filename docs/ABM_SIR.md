# Network SIR reference model

`abm::models::SIR` is the synchronous, fixed-contact reference workload for the
fifth canonical ABM family in implementation-plan §7.4. It uses the native typed
population phase engine, value-owned CSR contacts, integer agent states and
addressed Philox draws. Agents are susceptible (0), infected (1) or recovered (2).

```sh
cmake --build build --target fathom_sir
./build/fathom_sir > build/sir-example.csv
```

The example reports susceptible/infected/recovered counts on a 24-agent ring through
32 ticks at dt=.25. The API is [`sir.hpp`](../include/ankurafathom/abm/models/sir.hpp).
This native workload introduces no declarative IR fields. [Continuous-time execution and sync/async convergence](ABM_SIR_ASYNC.md) now pass
their declared finite-graph gates. Convergence to the SD mean-field model remains
a separate M5 requirement.

## Model contract

The graph is fixed, undirected, simple and contains exactly the population's stable
IDs. Each agent occupies one vertex. There are no births, retirements, migration,
external infections, waning immunity or edge changes. Every tick reads one common
pre-tick population snapshot and commits all agent states together.

For a susceptible agent with k infected neighbors, infection probability is
`-expm1(-(infection_rate * dt) * k)`. For an infected agent, recovery probability is
`-expm1(-recovery_rate * dt)`. Infection rate is **per contact**, without degree or
population normalization. Recovered agents remain recovered. Separate infection
and recovery mechanisms each use one addressed uniform variate and the strict
comparison `u < probability`. Zero hazard never transitions, even with scripted
u=0; a representable probability of one always transitions. `expm1` preserves small
hazards that would cancel in `1 - exp(-hazard)`.

A newly infected agent neither transmits nor recovers until the next tick. An agent
that recovers at this tick still contributes infection pressure from the starting
snapshot. Each agent can advance at most one state per tick. The construction uses
frozen-neighbor hazards; it is a time discretization, not an exact continuous-time
epidemic simulation.

Rates are finite and nonnegative, dt finite and positive, and maximum-degree
integrated hazards must be finite. Population is bounded at one million. States
must be integer 0/1/2. CSR validation rejects duplicate edges, self-loops and missing
endpoints; the model rejects directed contacts and mismatched membership.

Ticks have 16-bit draw addresses (65,536 updates). Distinct infection and recovery
streams use seed, scenario, replication, stable entity ID, zero-based tick and draw
index zero. Recovered-agent draws have no effect on other agents because addresses
are independent. Read-only observations consume no draws; declaration order does
not change the trajectory. Time is `ticks * dt`, with finite, strictly advancing
physical time checked before each update.

Each step stages a new typed phase and validates fixed membership, legal states and
one-step S→I/I→R transitions before committing. Any failure restores population,
contacts and clock. `scripted_step` accepts one finite [0,1) variate per stable ID,
including recovered agents, and supports exact boundary/history tests. Copies own
all state; phases capture values rather than a reference to the source model.

## Independent validation

The [frozen plan](../tests/oracles/abm/sir-plan.json) declares four 24-agent cases:
a degree-four ring, complete mixing with a smaller per-contact rate, a star, and
two disconnected rings. Initial conditions have two infected agents and one immune
agent; the disconnected case seeds only one component. Each engine runs 512
replications per case and observes ticks 0, 8 and 32 (times 0, 2 and 8): **2,048
trajectories and 6,144 snapshots per engine**.

The independent reference uses pinned Mesa 3.5.1 Model/Agent/NetworkGrid with a
NetworkX 3.7 contact graph. Separate `AgentSet.do` compute and commit passes preserve
the declared synchronous semantics. Mesa uses its own random seeds. Its graph queries
supply infected neighbors; no native engine or addressed replay drives the reference.

The offline scorer independently reconstructs infected fraction, recovered fraction,
newly infected fraction of the initially susceptible population, and susceptible–
infected edge fraction from raw states and the declared edge list. The **32 frozen
gates all pass**, with maximum empirical-CDF distance **.103516** against the
frozen **.147019** threshold. They cover four cases × two noninitial times × four metrics. Each requires both
the predeclared mean difference limit (larger of metric floor and six standard errors)
and empirical-CDF distance at most .147019. Confidence intervals are diagnostic.
The frozen DKW power calculation bounds per-comparison miss probability near .01001
for a true CDF displacement of .30. These finite cases do not establish agreement
for all contact networks or parameter regimes.

All observations preserve exact population count, legal states, irreversible histories
and initial immunity. A separate reachability traversal ensures infection cannot
cross disconnected components or an initially immune barrier. Mesa graph occupancy
and one-tick transitions are checked during generation. A separate Python adjacency-
list/Philox recurrence matches **864 agent states** across twelve selected native
trajectories exactly. Native
hand tests cover snapshot timing, multiple infected contacts, zero/tiny/saturated
hazards, strict probability boundaries, isolated agents, absorbing recovery, clock
limits, invalid parameters/graphs, declaration-order invariance, copy/replay and
whole-tick rollback. An additional 16 golden agent states exercise high addresses.

Scorer contracts reject stale metadata, corrupt rows, wrong states/types/times,
changed membership, immunity violations, unreachable infections and incorrect
summaries. Deliberately frozen states preserve basic invariants but must fail model
gates. Plan/adapter/dependency hashes accompany the frozen reference; routine CTest
uses it offline. Pinned reference regeneration reproduces exactly locally. All five
new SIR checks plus five relevant network/population/RNG regressions pass in normal
and ASan/UBSan builds (ten distinct checks across focused runs). Native ensemble
generation takes about eight seconds normally and 25 seconds with sanitizers;
timings are informational. CI is configured for pinned full regeneration; remote
CI has not run.

```sh
ctest --test-dir build --output-on-failure -R '^abm_sir'
.venv-abm-oracle/bin/python tests/oracles/abm/sir_oracle.py --verify
```

Reports are `build/abm-sir-report.json` and its sanitizer equivalent. See
[M4 acceptance](M4_ACCEPTANCE.md) for scope and completed convergence evidence,
and [status](STATUS.md) for verified evidence and remaining platform milestones.
