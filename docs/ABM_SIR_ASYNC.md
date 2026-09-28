# Continuous-time SIR and sync/async convergence

`abm::models::AsyncSIR` implements the same fixed-contact infection/recovery process
as [synchronous SIR](ABM_SIR.md), using the native `AsyncPopulation` calendar. Each
susceptible agent has infection hazard beta times its current infected-neighbor
count; each infected agent has recovery hazard gamma. Contacts and membership are
fixed, recovered agents remain immune, and beta is per contact without normalization.

The separate [`well_mixed` factory](HYBRID_SIR_MEAN_FIELD.md) uses implicit complete
mixing and susceptible hazard `(beta/N)*I`; its beta is a mass-action coefficient.
This page describes the graph constructor and M4 finite-graph evidence.

```sh
cmake --build build --target fathom_sir_async
./build/fathom_sir_async > build/sir-async-example.csv
```

The API is [`sir_async.hpp`](../include/ankurafathom/abm/models/sir_async.hpp). This
native model supplies the event-driven counterpart required by implementation-plan
§7.4. It adds no declarative IR fields or general statechart hazard expressions.

## Event and rollback contract

The implementation uses the direct Gillespie method. It sums hazards in ascending
stable-ID order, draws an exponential waiting time using `-log(u_wait)/total_rate`,
and selects one agent in proportion to its hazard using an independent uniform.
Only that agent advances S→I or I→R. All hazards are recomputed after the transition
and the next event is scheduled. A total rate of zero leaves the calendar empty.

There is exactly one pending event whenever the process has positive total hazard.
The selected transition and its successor schedule commit in one asynchronous
calendar transaction. A failed successor calculation restores the state, event,
calendar and RNG generation. `run_until(horizon)` additionally stages the entire
requested horizon, including earlier successful events within that call. It includes
events exactly at the horizon. Observation-only horizon advances preserve the
already drawn pending event, so dense and sparse observation schedules agree.

Rates must be finite and nonnegative; computed total hazards and event deadlines
must be finite. Every event deadline must strictly exceed the preceding event time.
Unrepresentable waits or clock increments fail explicitly. The graph is simple,
undirected and has exactly the live stable IDs; states are integer 0/1/2. Population
is bounded at one million, with no births, migration, immunity loss or graph edits.

The waiting and selection streams are distinct. Draw addresses use seed, scenario,
replication, the population's first stable ID, event generation and draw index zero.
A positive-hazard schedule requires a generation in [0,65535]. Absorption after the
last addressable event succeeds without an additional draw. Weighted selection uses
a strict cumulative-boundary comparison; a target rounded to the total rate is
clamped to the preceding representable value. A value-owned optional script provides
one pair of open-(0,1) variates per event for hand histories and failure injection.
Copies own their calendar and callback data; read-only queries consume no draws.

These rules handle SIR's changing infection pressure inside this model. They do
**not** implement general changing-hazard statecharts or make the synchronous
finite-dt model event-exact.

## Independent event and distribution evidence

The [plan](../tests/oracles/abm/sir-async-plan.json) was frozen before either ensemble.
Four 24-agent ring/complete/star/disconnected graphs use the same beta, gamma and
initial states as the synchronous reference. Each engine runs 512 replications,
observed at times 0, 2 and 8: **2,048 runs and 6,144 snapshots per engine**.

The independent reference uses pinned Mesa 3.5.1 Model/Agent/NetworkGrid, NetworkX
3.7, Python exponential sampling and weighted event selection. Its contact queries
and agent state are separate from the native calendar. The scorer reconstructs
all snapshots from raw event histories, checking event order, legal transitions,
positive hazards, immutable prefixes, population conservation and immunity.

**All 32 predeclared distribution gates pass**, comparing infected/recovered
fractions, attack fraction and susceptible–infected edge fraction. Maximum empirical
CDF distance is **.095703**, below the fixed **.147019** threshold. Mean thresholds
and confidence diagnostics follow the frozen canonical-model policy.

Independent Python Philox/race recurrences match **864 agent states and 356 event
observations**, with maximum local event-time error **zero** under the predeclared
2e-11 relative/absolute clock tolerance. Three additional golden events and four
agent states exercise upper seed/entity bits and maximum scenario/replication/stream
addresses. Hand histories cover infection/recovery races, rate changes after events,
zero rates, isolation, absorption, selection boundaries, horizon inclusion,
observation density, copy isolation, invalid inputs, overflow and both single-event
and whole-horizon rollback. Corrupt evidence and frozen dynamics are rejected.

Pinned regeneration reproduces event identities and states exactly and checks event
times with the frozen tolerance; maximum local difference is zero. The reference
also records small-graph probability laws from pinned SciPy 1.18.1. Adapter, plan,
convergence scorer and dependency hashes are recorded. CI regeneration is configured;
remote CI has not run.

## Convergence of the full joint state distribution

Three additional graphs—a three-agent line, three-agent triangle and four-agent
star—use identical rates and initial states in both native modes. Times are 2 and 4;
synchronous dt is 1, .5, .25, .125 and .0625. Each configuration runs 1,024 native
replications: **18,432 trajectories** over one async and five sync configurations.

For these small graphs, the scorer enumerates all 27 or 81 joint agent states.
The continuous-time reference is the distribution from the CTMC generator matrix;
the synchronous reference is the product of independent per-agent transition
probabilities evaluated against the pre-tick state. Pinned SciPy matrix exponentials
and NumPy matrix powers produce frozen laws. Independent standard-library Poisson
uniformization (tail bounded at 1e-14) and sparse repeated multiplication agree with
all 36 laws within 1e-12.

The deterministic convergence gates apply to **total variation over the complete
joint distribution**, not just mean infected counts. Every curve must decrease at
each refinement, reach TV ≤ .04, reduce the coarse error to at most 15%, and have a
final halving ratio between .35 and .70. All six curves pass:

| Graph | Time | TV at dt=1 | TV at dt=.0625 | Last halving ratio |
|---|---:|---:|---:|---:|
| Line (3 agents) | 2 | .300561 | .015660 | .492173 |
| Line (3 agents) | 4 | .217040 | .012521 | .497808 |
| Triangle (3 agents) | 2 | .285018 | .015040 | .493919 |
| Triangle (3 agents) | 4 | .197258 | .012358 | .498646 |
| Star (4 agents) | 2 | .246056 | .014192 | .497215 |
| Star (4 agents) | 4 | .169398 | .010706 | .507522 |

The native samples must also reproduce their respective joint laws: **36 gates
pass** for CDF distance on a fixed base-three state encoding and full-state total
variation. CDF limits use one-sample DKW with the declared family alpha .001 and
36-way adjustment. TV limits use a union bound over subsets of the finite support:
`sqrt((K*log(2) + log(36/alpha))/(2*n))`, with K=27 or 81 and n=1,024.

**Six finest-sync/async comparisons also pass.** Their allowed CDF difference is
the known deterministic TV bias plus `sqrt(2*log(4*6/alpha)/n)`, obtained from two
one-sample DKW bounds and a six-way adjustment; it does not assume unpaired streams.
Monte Carlo noise is kept separate from the measured deterministic convergence.
Frozen native trajectories must fail the joint-law gates. A hand two-agent,
infection-only exponential law checks the CTMC solver independently.

These results establish the declared finite-graph sync/async convergence evidence.
They do not establish universal convergence tolerances, general changing-hazard
statechart support, or the well-mixed ABM→SD limit required by M5.

```sh
ctest --test-dir build --output-on-failure -R '^abm_sir_(async|convergence)'
.venv-abm-oracle/bin/python tests/oracles/abm/sir_async_oracle.py --verify
```

All seven new checks pass in both builds. Full platform regression passes **170/170
normal and 170/170 ASan/UBSan tests**, with schema conformance passing in both. The
combined native async/convergence ensemble takes about 16 seconds normally and 53
seconds with sanitizers; timings are informational.

Numeric reports are `build/abm-sir-async-report.json` and
`build/abm-sir-convergence-report.json`, with sanitizer equivalents. See
[M4 acceptance](M4_ACCEPTANCE.md) and [platform status](STATUS.md).
