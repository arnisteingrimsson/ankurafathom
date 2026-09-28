# Continuous agents with discrete lifecycle and pulses

`AgentStocks::apply(Change)` stages nonstock field updates, signed stock pulses,
retirements and births at the committed clock. `DynamicAgentStocksAtomic` adds
revisioned controls, the existing typed event/lifecycle bridge, exact absolute
Euler ticks and population snapshots for aggregate-driven SD.

The transaction order is deliberate:

1. Integrate the old population to the event timestamp.
2. Apply ordinary field updates, preserving integrator-owned fields.
3. Sum each stock's pulses and apply the net amount simultaneously.
4. Retire agents and record their departing stock quantities.
5. Allocate newborn records, validate stock domains and publish.

A newborn starts accumulating after birth; a departing agent accumulates through
its retirement timestamp. Retirement is a boundary removal recorded in the
receipt. It does not silently transfer stock into a global reservoir. A model can
explicitly pulse the departing stock into a reservoir before retirement. Receipt
vectors follow the configured bound-field/global-stock orders and report birth,
retirement and pulse amounts separately.

Ordinary updates identify individual schema fields and reject writes to bound
stock fields. Duplicate writes or retirements reject. Pulse references must be
live before the transaction; newborn IDs cannot be guessed and pulsed in the same
change. PopulationStore maintains network membership through retirement/birth.
Pulses are signed, finite and checked after netting; there is no clipping. Records,
global stocks, topology, IDs, time, revisions and receipts commit together.

`AgentStockInput` enters port 0 with a configured channel name, timestamp, revision
and change. Channels have independent producer/revision checks and merge lexically.
Bound `PopulationLifecycleInput` enters port 3 with matching store/schema and
lifetime-unique sequence keys. These inputs merge after stock channels, sorted by
sequence. Output port 1 carries the population snapshot; port 2 carries global
stocks, revision and an optional discrete receipt. Integration-only ticks have no
receipt. Initial state publishes at revision zero.

Off-grid inputs split an Euler interval, leaving regular tick deadlines unchanged.
Observation horizons never split integration. Exact tick/input confluence integrates
once before applying the change. A lifecycle bridge's publication step is real:
if its input arrives at a due tick, the lifecycle change occurs in a subsequent
microstep at the same physical time. The independent oracle models this second
commit explicitly. A failed downstream publication can be retried without repeating
integration, birth allocation or pulse application.

The frozen rational recurrence checks four agent/SD clock pairs, all 24 declaration
orders and both injection orders: **192 configurations, 7,872 snapshots and 818,496
scalar comparisons**, with zero observed gap. Cases include old-global-state flow
rates, availability changes, off-grid births, signed pulses, retirements, delayed
lifecycle publications, discrete inventory receipts and IDs near 2^48. Sixteen
corruption controls reject altered evidence. Native hand cases also cover empty
populations, network membership, reordered bindings, schema/domain errors,
cross-channel conflicts, exhausted IDs, clock overflow, observation invariance and
checked publication retry.

The existing `AgentStocks::step(dt)` retains its arithmetic. `step_to(target)`
provides an exact absolute target clock. Existing autonomous and declarative
agent-stock modes remain supported, with their previous numerical contracts.
General declarative lifecycle/pulse graph bindings and higher-order agent-stock
integration remain separate work.

```sh
cmake --build build --target hybrid_dynamic_agent_stocks_tests hybrid_dynamic_agent_stocks_oracle_tests fathom_dynamic_agent_stocks
ctest --test-dir build -R '^hybrid_dynamic_agent_stocks' --output-on-failure
./build/fathom_dynamic_agent_stocks
```

Normal and ASan/UBSan artifacts match byte for byte. Trajectory SHA-256:
`f5828ce33d4619773a6cdb87eaff4589902f786a7d253ab624c5799be4cd46d9`.
Twenty affected/new tests pass in each build; existing autonomous agent-stock
trajectory hashes remain unchanged.
