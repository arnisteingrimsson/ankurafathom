# DES events to population lifecycle

`EventToLifecycle<Event, Tag, Message>` maps typed events into explicit birth and
retirement batches. The population remains the sole owner of agent IDs, fields,
network membership and timers. The bridge owns only event replay tracking and
pending publication. Its callbacks can read immutable event records; they receive
no mutable population pointer.

Bridge messages carry the destination namespace and schema. The population rejects
misrouted batches and repeated bound event sequences, including replay from another
bridge. Legacy direct lifecycle messages retain their existing optional-target
contract. Schema, duplicate retirement and configured batch-limit checks happen
before publication; live-reference checks and ID exhaustion happen at the owner.

The [onboarding example](../examples/typed_lifecycle.cpp) connects typed DES delay
completions to newborn agents. At .5 a previously born agent participates in the
population phase; a birth published by the bridge in a later microstep at .5 joins
the next phase. The bridge never retroactively reruns a committed phase or timer.

```sh
cmake -S . -B build
cmake --build build --target hybrid_lifecycle_bridge_tests hybrid_lifecycle_bridge_oracle_tests fathom_typed_lifecycle
ctest --test-dir build -R '^hybrid_lifecycle_bridge' --output-on-failure
./build/fathom_typed_lifecycle
python3 tests/oracles/hybrid/lifecycle_bridge_oracle.py --verify --contract
```

## Evidence

The frozen [plan](../tests/oracles/hybrid/lifecycle-bridge-plan.json) covers eight
cases, synchronous and asynchronous populations, all 24 component declaration orders
and 21 horizons. The graph composes typed DES delay, lifecycle bridge, population
owner and result publisher. An independent [scheduler](../tests/oracles/hybrid/lifecycle_bridge_oracle.py)
tracks allocation, per-agent values, live flags, timer cancellation, edge removal
and separate same-time population commits.

- **384 configurations and 8,064 snapshots pass.**
- **146,304 agent scalar comparisons match exactly**, alongside exact identities,
  liveness, network edges, completion counts, clocks and publication revisions.
- Thirteen corruption controls reject altered evidence.
- All **17 focused tests pass in normal and ASan/UBSan builds**, including existing
  population, lifecycle, network, topic, behavior, generator and declarative checks.
- Normal/sanitizer trajectories and reports are byte-identical.
- Hand tests additionally cover 336 exact DES/sync/async snapshots, immutable clones,
  no-op events, canonical birth ordering, full uint64 event keys, schema/namespace
  failures, cross-producer replay, batch limits, exhausted IDs and failed downstream
  delivery followed by retry without duplicate allocation.

Trajectory SHA-256:
`4c2b27f0756f579afa9bc0e67748616f0762cf44fec86879396aaafec0ecfd23`.

## Scope

Birth IDs are available in committed population results, not predicted by the
bridge. Retiring an agent does not retire a separate DES entity record. Stateful
process/entity identity needs the separate entity-agent owner. Distinct bridge
producers share one unique event-sequence namespace. Seen keys are retained for
the simulation lifetime. This native adapter does not add declarative graph wiring.
See [semantics](SEMANTICS.md#typed-event-to-population-lifecycle-publication) and
[M5 tracking](M5_ACCEPTANCE.md).
