# Typed events to stock pulses

`EventToStockPulse<Event, Message>` maps a typed event payload to a signed vector
of stock amounts. Its pure callbacks provide an event key and amount expression;
keys determine canonical evaluation order and prevent replay. The bridge publishes
a value-owned `StockPulse` in a following zero-time DEVS step.

`SignalSD` accepts named pulse channels alongside its existing scalar inputs.
It integrates to the event timestamp using the old scalars, combines simultaneous
channel amounts, checks the resulting stock domains and then latches new scalars.
The entire transition is staged. Tick confluence, callback failure and checked
kernel retry cannot partially apply a pulse or consume an unpublished batch.

The [typed DES example](../examples/typed_event_pulse.cpp) passes engagement tokens
through `ReferenceDelay`, looks up hours and rates in an immutable owned record
snapshot, and posts completed hours and revenue to SD. Three completions give
3/5/9 cumulative hours and 300/600/1100 cumulative revenue at .375/.5/1.

```sh
cmake -S . -B build
cmake --build build --target hybrid_typed_pulse_tests hybrid_typed_pulse_oracle_tests fathom_typed_event_pulse
ctest --test-dir build -R '^hybrid_typed_pulse' --output-on-failure
./build/fathom_typed_event_pulse
python3 tests/oracles/hybrid/typed_pulse_oracle.py --verify --contract
```

## Evidence

The frozen [plan](../tests/oracles/hybrid/typed-pulse-plan.json) has twelve cases,
three SD steps, six component orders and two event insertion orders. It combines
payload-dependent quantities/prices, multiple event producers, zero/on/off-grid
pulses, scalar changes, stock-dependent flows and uint64 event keys above 2^63.
An independent [rational oracle](../tests/oracles/hybrid/typed_pulse_oracle.py)
computes flows and posted quantities directly over the union of event clocks.

- **144 configurations, 3,024 snapshots and 12,096 scalar comparisons pass.**
- Exact clock, per-channel revision and event-count agreement.
- Maximum absolute discrepancy **2.84e-14**; fourteen corruption controls reject
  altered evidence.
- **22 focused tests pass in normal and ASan/UBSan builds**, including the five new
  checks and all aggregate, agent-stock publication, ABM adapter and bounded
  declarative agent-stock regressions.
- Normal/sanitizer trajectories and reports are byte-identical. Existing aggregate,
  agent-stock publication and declarative oracle hashes are unchanged.
- Final hand tests add typed DES completion composition across all six component
  orders, rollback after downstream failure, replay rejection, stable event-key
  summation, simultaneous offsetting amounts, independent clones, invalid schemas,
  ownership/revision checks, domain and arithmetic/deadline overflow checks.

Trajectory SHA-256:
`4dc2837fc36ce0299abbe9d6611fdff0488bfc8afccce5938c3be2bbd5927774`.

## Boundaries

Native callbacks own their data; a pointer to an externally mutated store is not
covered by rollback. Pulse vectors use consumer stock order, and the native API
does not infer units. Each named channel has a single pinned producer. Different
publication microsteps remain separate transactions even at the same timestamp;
a later deposit cannot rescue an earlier invalid withdrawal. The bridge retains
seen event keys for its lifetime. General declarative event bindings remain open.
See [semantics](SEMANTICS.md#typed-event-pulses-and-scalar-sd) and
[M5 acceptance](M5_ACCEPTANCE.md).
