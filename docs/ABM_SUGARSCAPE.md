# Sugarscape-lite

`abm::models::Sugarscape` is the native single-resource reference workload for
implementation-plan §7.4. It composes typed population records, stable-ID retirement,
single-occupancy grids and addressed random activation. This declared variant has
movement, harvesting, metabolism, starvation and regrowth; reproduction, trade,
pollution, seasons and inheritance are outside its scope.

```sh
cmake --build build --target fathom_sugarscape
./build/fathom_sugarscape > build/sugarscape-example.csv
```

The example records live population, reserves, land sugar and cumulative resource
accounting through sweep 16. The native API is
[`sugarscape.hpp`](../include/ankurafathom/abm/models/sugarscape.hpp); it introduces no
new declarative IR fields or synchronous/async equivalence claim.

## Model contract

Each sweep shuffles the stable IDs of agents alive at its start, then activates each
once. An agent sees its own cell and unoccupied cells along the four cardinal rays
within its integer vision. This is axial vision, not the full Manhattan ball.
Occupied intervening cells do not block sight. On periodic grids cells are deduplicated
and distance is the shortest wrapped axial distance. Initial positions must be
canonical. An agent maximizes current land sugar, then minimizes distance, then
chooses uniformly among remaining ties ordered by y then x. Its own cell is eligible;
if all visible cells have equal sugar, it stays.

The agent moves, harvests all destination sugar, then consumes the smaller of its
reserve and metabolism. Zero reserve causes immediate retirement and releases the
cell. Later activations observe all earlier movements, harvests and retirements.
Metabolism never makes reserves negative or consumes unavailable sugar. Immutable
metabolism is positive; initial reserve is positive; vision is in [0,1,000,000].
There are no births and IDs are never reused.

After all activations, every cell regrows by `min(regrowth, capacity - sugar)`,
including occupied cells. Regrowth continues for an empty population. Dimensions
are positive and total area is at most one million cells. Capacity, initial sugar
and regrowth are nonnegative signed 64-bit integers, with sugar at most capacity.
The exact accounting identity is:

```
initial land + initial agent reserves + cumulative regrowth
    = current land + current live reserves + cumulative consumption
initial population = current live population + cumulative deaths
```

Every sweep stages the complete landscape, population, ledger and clock before
committing. Invalid scripted activation/destinations or any resource-total overflow
restore all of them. The initial resource total plus cumulative regrowth must remain
representable as signed 64-bit, even when consumption has removed most of that
resource. Copies own their state and observations consume no random draws.

Separate order and movement streams use Philox seed/scenario/replication/entity/
sweep addresses and unbiased integer rejection sampling. Fisher–Yates order draws
use `first_id + remaining - 1`; movement draws use the activating agent's stable ID.
Each choice starts at draw index zero; rejection attempts advance it. There are
65,536 addressable sweeps; overlapping streams and overflowing addresses are rejected,
even for empty initial populations. `scripted_step` requires a permutation of current
live IDs and one optimal destination per activation.

## Independent validation

The plan was frozen before generating either ensemble. Four fixed 6×6 landscapes
start with 18 heterogeneous agents: periodic and bounded regrowth-one cases,
periodic resource depletion, and bounded regrowth-two. Each engine runs 512
replications per case, observed at sweeps 0, 4 and 16: 2,048 trajectories and 6,144
snapshots per engine.

The independent reference uses pinned Mesa 3.5.1 `Model`, `Agent`, `SingleGrid` and
`AgentSet.shuffle_do`, with its own random seeds. Mesa neighborhood queries provide
canonical candidate cells, filtered to axial vision; its grid handles occupancy,
movement and retirement. The offline scorer uses exported raw agents, cells and
cumulative counters. It checks immutable traits, stable-ID membership, cell occupancy,
land bounds, exact resource/population balances, cumulative monotonicity and regrowth
rate bounds. Mesa summaries are checked against independently reconstructed summaries.

**All 32 predeclared gates pass**, with maximum empirical-CDF distance .072266
against the frozen .147019 limit. The gates compare survival fraction, live reserves per initial agent,
land sugar as a fraction of capacity, and cumulative consumption per initial agent
at both noninitial observation times. Each gate requires a mean difference within
the larger of its fixed floor and six standard errors, plus empirical-CDF distance
at most .147019. Confidence intervals are diagnostics. The frozen power calculation
bounds per-comparison miss probability near .01001 for a true CDF displacement of .30.
These are finite-case comparisons, not universal parameter or equilibrium claims.

A separate Python list/dictionary implementation enumerates cardinal rays and uses
independent Philox words to replay twelve native trajectories exactly, including
**537 live agent states, 1,296 land values and 36 complete ledgers**, including
exact live identities. Hand histories
cover resource choice, distance ties, own-cell harvest, axial vision, zero vision,
sequential occupancy/harvest, vacancies, starvation, bounded/periodic edges, very
large vision on tiny grids, capped/end-sweep regrowth, invalid inputs/schedules,
copy isolation, replay, clock exhaustion and late rollback. High-address golden
histories cover 16 agent states, 32 land values and four ledgers.

Scorer contracts reject corrupt metadata, observations, types, traits, positions,
resources, counters and metrics. A deliberately frozen population/landscape preserves
basic budgets but must fail the model gates. Reference metadata hashes the plan,
adapter and pinned requirements. Routine CTest uses the frozen reference offline;
pinned regeneration reproduces the reference exactly locally. CI is configured to
regenerate it with Mesa; remote CI has not run. All five Sugarscape checks and eight
relevant grid/population/lifecycle/RNG regressions pass in normal and ASan/UBSan
builds. The native ensemble takes about 11 and 34 seconds respectively; these
timings are informational, not acceptance gates.

```sh
ctest --test-dir build --output-on-failure -R '^abm_sugarscape'
.venv-abm-oracle/bin/python tests/oracles/abm/sugarscape_oracle.py --verify
```

Reports are written to `build/abm-sugarscape-report.json` and its sanitizer equivalent.
See [M4 acceptance](M4_ACCEPTANCE.md) and [status](STATUS.md) for completed evidence and
remaining milestone gates.
