# Shared DES entity and ABM agent identity

`EntityAgentAtomic<Tag>` owns a typed population and its process ledger. DES tokens
refer directly to the population's stable `(store, id)` identity. Statecharts and
population rules update the sole authoritative record while the token travels
through a DES process. A return becomes a population command at its exact event
time, so the action sees current agent state.

Each dispatched token carries a value-owned snapshot of its schema, fields and
dispatch time. DES duration and selection callbacks can use these fields without
borrowing a mutable store. Reference queue, delay, selection, seize and release
blocks preserve the optional snapshot. Existing tokens without snapshots retain
their prior behavior.

A waiting agent launches when a pure eligibility rule becomes true. The ledger
then tracks it as in-flight until a valid return. Returns must preserve the issued
identity, priority and snapshot and have no outstanding resource leases. Duplicate
returns and retirement while in-flight reject transactionally. A return action may
retire its own agent; waiting and returned agents can retire normally.

The [example](../examples/entity_agent.cpp) has agent behavior update work while
DES delivery is underway; completion increments the current work record. This
shows why the dispatch snapshot must remain distinct from mutable agent state.

```sh
cmake -S . -B build
cmake --build build --target hybrid_entity_agent_tests hybrid_entity_agent_oracle_tests fathom_entity_agent
ctest --test-dir build -R '^hybrid_entity_agent' --output-on-failure
./build/fathom_entity_agent
python3 tests/oracles/hybrid/entity_agent_oracle.py --verify --contract
```

## Evidence

The frozen [plan](../tests/oracles/hybrid/entity-agent-plan.json) has six cases,
all 120 owner/seize/pool/delay/release declaration orders, and 25 observation
horizons. Cases vary pool capacity, initial population, readiness lag, heterogeneous
service, births and retirement on return. An independent
[Python scheduler](../tests/oracles/hybrid/entity_agent_oracle.py) combines FIFO
multi-server accounting with explicit statechart transitions and shared identities.

- **720 configurations and 18,000 snapshots pass exactly.**
- **1,057,680 agent and dispatch-snapshot field comparisons** agree, alongside
  exact owner clocks, queue/allocation counts and launched/returned counts.
- Twenty-one corruption controls reject altered evidence, including boolean/ID
  type substitution, changed statecharts, resource accounting and dispatch fields.
- All **19 focused tests pass in normal and ASan/UBSan builds**: the five new checks,
  lifecycle/pulse regressions, native reference process/resource checks and the
  existing typed DES CLI contract.
- Trajectories and reports are byte-identical across builds.
- Hand checks cover all 120 resource graph orders, before/on/after-timeout returns,
  current-state return actions, newborn charts, immutable dispatch data, zero-time
  initialization, initial lifecycle confluence, reserved completion sequencing,
  in-flight retirement rollback and downstream dispatch retry without duplicate work.

Trajectory SHA-256:
`2a49c65c01ccb9f3876d9d475c54ffb8de8aa9fdbfc17fd88b1fc2f499dca281`.

## Boundaries

This initial owner supports one process journey per agent. Re-entry/revisits,
process cancellation/preemption and mutation of dispatch snapshots are unsupported.
Completion command sequence UINT64_MAX is reserved; ordinary same-agent commands
in a bag execute before the completion action. The owner emits population results
on port 1, dispatches on port 5 and accepts returns on port 6. All callbacks must
be pure and own captured data. General declarative shared-owner graphs remain open.
See [semantics](SEMANTICS.md#shared-entity-agent-ownership) and
[M5 acceptance](M5_ACCEPTANCE.md).
