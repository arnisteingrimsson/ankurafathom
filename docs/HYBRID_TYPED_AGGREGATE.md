# Typed population aggregates and SD consumers

The native aggregate bridge now publishes **sum, mean, count, minimum and maximum**
from committed typed population snapshots. `SignalSD` consumes the resulting
scalar vector in stock/flow expressions. Both endpoints are cloneable DEVS atomics
with staged transitions and checked-kernel rollback.

The producer sends a value-owned population snapshot with an absolute timestamp and
increasing revision. The bridge verifies the population schema and namespace, then
evaluates all reducers before committing one scalar batch. It publishes that batch
one zero-time microstep later. No mutable population pointer is retained. A pending
publication survives a failed downstream step, so retry delivers it once.

Each stream has one owner. Multiple snapshots in one bag, mixed writers, duplicate
or older revisions, nonfinite results and mismatched schemas are rejected. Equal
values with a newer revision still publish. Same-time confluence publishes the old
pending batch and then stages the newer batch.

| Reducer | Empty selection | Numeric behavior |
|---|---|---|
| Sum | Zero | Signed values allowed; rejects intermediate overflow |
| Count | Zero | Counts selected live records; no projection |
| Mean | Error, or explicit finite fallback | Running interpolation avoids overflow of the corresponding sum |
| Minimum | Error, or explicit finite fallback | Finite projected values only |
| Maximum | Error, or explicit finite fallback | Finite projected values only |

All reducers support a pure selection predicate. Retired rows are ignored. Values
are evaluated in ascending stable-ID order; floating-point means need not equal a
sum followed by division bit for bit. Unique reducer names define stable positions
in the publication vector.

`SignalSD` integrates to an input timestamp with the **previously latched vector**,
then accepts the new vector. This applies at normal ticks, off-grid timestamps and
later microsteps at an existing tick. Its initial vector is explicit. Flow callbacks
can read stocks, scalar inputs and time. The current method is Euler, with explicit
stock/flow sign policies and no clipping.

For example, capacity is six from time zero until a retirement at .375, then four.
At .5 the integrated capacity-time must be `6*.375 + 4*.125 = 2.75`. Using the new
capacity for the preceding interval would give the wrong answer. See the runnable
[example](../examples/typed_aggregate.cpp) and
[semantics](SEMANTICS.md#typed-snapshot-aggregates-and-scalar-driven-sd).

## Evidence

The frozen [24-case plan](../tests/oracles/hybrid/typed-aggregate-plan.json) covers
signed values, changing population size, tombstones, selected/unselected agents,
and empty final populations. Each case has six publications, tick spacing .25 or
.5, and 21 observation horizons. Both aggregate-first and SD-first component
declaration orders run through the native checked DEVS kernel.

The [independent oracle](../tests/oracles/hybrid/typed_aggregate_oracle.py) computes
reducers and rectangular flow integrals with Python `Fraction`. It uses no native
SD solver or native output to form expectations. It also checks the exact last
committed SD timestamp: merely asking for an observation between events must not
advance the model or change its integration grid.

- **1,008 snapshots and 10,080 scalar comparisons pass** across 48 native runs.
- Maximum absolute discrepancy from the exact-rational reference is **6.67e-16**.
- All clocks and publication revisions agree exactly.
- Sixteen negative controls reject missing/duplicate rows, corruption of each
  reducer and integrated stock, wrong revision/time, NaN and retroactive inputs.
- Hand tests cover finite means of extreme values whose sum overflows, callback
  result validation, schema/owner/revision checks, same-time confluence, unchanged
  publications, clone independence, queued-snapshot ownership, signed and
  state/time-dependent flow expressions, fractional absolute clocks, tick overflow,
  invalid stock integration and downstream rollback/retry.

All **ten focused CTests pass in normal and ASan/UBSan builds**: five new aggregate
checks plus existing SD, typed-population, clock, publication and agent-stock
regressions. Trajectories and reports are byte-identical across builds; both
example outputs match. The configured suite is 194 tests. The last full regression
remains 170/170 in both builds. No remote CI result is claimed.

Trajectory SHA-256:
`a5bb16b0d1516e0ca58d0059d250457ab0ecbd2d01367fdde45d5864efd47ecc`.
The generated report also records the plan hash and maximum discrepancy.

## Reproduce

```sh
cmake -S . -B build
cmake --build build --target hybrid_typed_aggregate_tests hybrid_typed_aggregate_oracle_tests fathom_typed_aggregate
ctest --test-dir build -R '^hybrid_typed_aggregate' --output-on-failure
./build/fathom_typed_aggregate
python3 tests/oracles/hybrid/typed_aggregate_oracle.py --verify --contract
```

## Remaining scope

This provides native typed DEVS publication and C++ flow expressions. It does not
add JSON IR components, units or named declarative input bindings. The subsequent
`PopulationResultPublisher` adapter connects existing sync/async `PopulationAtomic`
committed results to this snapshot interface. It publishes an explicit initial
population at time zero, then assigns increasing revisions without rerunning
behavior. Its five focused checks pass in both session builds, including 336 exact
snapshots over all 24 four-component declaration orders, sync/async clocks,
births/retirements, timer cancellation, commands and downstream rollback/retry.
See [adapter semantics](SEMANTICS.md#typed-abm-result-to-snapshot-adapter).
Producers remain responsible for sending committed snapshots;
the bridge does not validate lifecycle history between them. The SD consumer does
not yet combine publications with pulse/lifecycle inputs.

Off-grid inputs intentionally split Euler intervals. Publishing even an unchanged
vector can change numerical error when a flow depends on stock state; observation
alone never splits an interval. Pure, value-owned callbacks are required for
independent clones; external side effects are outside rollback guarantees.
[M5](M5_ACCEPTANCE.md) remains open, with agent-stock declarative composition
and the remaining typed bridge contracts next.
