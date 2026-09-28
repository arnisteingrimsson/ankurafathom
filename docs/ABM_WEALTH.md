# Boltzmann wealth exchange

`abm::models::WealthExchange` is a native reference workload built on typed population
records, transactional updates, population-owned CSR networks and addressed Philox
draws. It supplies the first canonical ABM model comparison required by §7.4 of the
implementation plan. It does not estimate household or consulting-firm economics.

Build and run the example:

```sh
cmake --build build --target fathom_wealth
./build/fathom_wealth > build/wealth-example.csv
```

The example starts 24 agents with one unit each, runs 32 sweeps, and emits 792
agent observations including the initial state. Every sweep preserves 24 total
units and 24 agents. The header is
[`include/ankurafathom/abm/models/wealth.hpp`](../include/ankurafathom/abm/models/wealth.hpp).

## Model contract

Each sweep activates all agents once in a uniformly shuffled order. A donor with
positive current wealth gives one unit to a uniformly chosen eligible recipient.
Without a network, recipients include every agent, including the donor. With a
network, recipients are outgoing neighbors; isolated agents do nothing. A recipient
can spend newly received wealth later in the same sweep. Transfers are sequential;
the entire sweep commits atomically. An invalid later choice restores the original
records and sweep count.

Initial wealth is a vector of nonnegative signed 64-bit integers with a total that
fits the same type. IDs may start at a nonzero 48-bit value. Membership is fixed.
`store()` and `wealth()` expose observations; `step()` performs a random sweep and
`scripted_step(order, recipients)` supports deterministic activation histories.

Order and recipient selection use distinct explicit streams. Fisher–Yates draws
address the original array position's stable ID; recipient draws address the donor.
Both include seed, scenario, replication and sweep. Unbiased integer rejection
sampling uses the draw-index field for retries. Exhausted addresses fail rather
than wrapping. Up to 65,536 sweeps are supported. See the full
[semantics](SEMANTICS.md#boltzmann-wealth-reference-model).

This API implements sequential random activation. It adds neither a new typed-IR
dialect nor continuous-time wealth dynamics.

## Independent validation

The [frozen plan](../tests/oracles/abm/wealth-plan.json) specifies four n=24 cases:
well-mixed populations with initial wealth 1 and 4, and ring/star networks with
initial wealth 1. Each engine runs 512 replications per case, recording sweeps
0, 8 and 32: **2,048 trajectories and 6,144 snapshots per engine**.

The independent implementation uses pinned Mesa 3.5.1 `Model`, `Agent`,
`AgentSet.shuffle_do` and `NetworkGrid`, with NetworkX 3.7 ring/star topology.
Mesa uses independent Python random seeds; native runs use Philox. The reference
implements the declared rules directly rather than invoking native model code.
Integer conservation and population size are checked throughout native transfers
and in reference sweeps, and again when loading observations.

**32 predeclared distribution gates pass**: four cases × two noninitial times ×
four statistics (Gini coefficient, penniless fraction, largest wealth share, and
concentration). Each gate requires both a mean-gap bound and an empirical-CDF
distance bound. The fixed KS threshold is .147019; the largest observed distance
is .080078. Mean bounds use the larger of the metric floor and six standard errors.
Difference confidence intervals are reported as diagnostics, not individual
coverage requirements.

The plan was frozen before sampling. With 512 samples per engine, a true CDF
displacement of .30 has a conservative per-comparison miss-probability bound of
approximately .01001 under the DKW/triangle-inequality calculation in the plan.
This design targets material distribution differences; passing it does not prove
equivalence for every parameter, horizon or rare outcome. No equilibrium claim is made.

Independent Python addressed histories match **864 agent observations** across
twelve runs. A further 16 native golden observations exercise high entity/key bits
and maximum scenario/replication/stream fields. Hand histories cover same-sweep
spending, activation-order effects, isolated and self transfers, directed edges,
copy isolation, replay, empty/penniless/singleton populations, maximum integer totals,
invalid schedules, late-failure rollback and exhausted draw addresses.

The scorer rejects corrupt metadata, missing/misidentified observations and invalid
wealth records. A deliberately frozen but conserved population fails the model gates,
checking that conservation alone cannot make the validation pass. The reference
records adapter, plan and dependency hashes; pinned regeneration matches exactly
locally and is configured in CI. Routine tests consume frozen data offline.

```sh
ctest --test-dir build --output-on-failure -R '^abm_wealth'
.venv-abm-oracle/bin/python tests/oracles/abm/wealth_oracle.py --verify
```

The scorer writes `build/abm-wealth-report.json`. See [M4 acceptance](M4_ACCEPTANCE.md)
for the remaining canonical models and convergence gate, and [status](STATUS.md)
for current build evidence.
