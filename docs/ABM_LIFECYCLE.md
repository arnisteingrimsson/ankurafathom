# Scheduled agent births and retirements

Typed synchronous and asynchronous ABM models support scheduled population
changes through `lifecycle`. Run the [replacement fixture](../models/typed_abm_lifecycle.ir.json):

```sh
./build/fathom lint models/typed_abm_lifecycle.ir.json
./build/fathom run models/typed_abm_lifecycle.ir.json
```

Each population entry contains full typed records and explicit stable IDs:

```json
"lifecycle": [
  {"time": 1, "sequence": 0, "retire": [0],
   "births": [{"work": 10, "ready": true, "role": "new hire"}]}
]
```

All four properties are required. Empty retirement/birth arrays are legal.
Sequences must be unique within each timestamp. Initial IDs follow the initial
agent array; newborn IDs follow chronological `(time, sequence, record index)`
order, regardless of declaration order. IDs are never reused. Initial agents
plus all scheduled births cannot exceed `agent_limit` (default one million).

## Timestamp contract

Already-due async timers execute first. All lifecycle entries at that timestamp
then apply as one batch: retirements first, followed by births in sequence order.
Direct messages and external topics follow. Generated messages and immediate
timers drain according to the existing publication contract; synchronous phases
run last. A newborn receives same-time messages and participates in a coincident
tick. Initial agents may retire at time zero. Scheduled newborns cannot retire
at their own birth timestamp, even through another entry.

Retirement cancels future timers. Async newborns enter the declared initial state
at birth time, with entry generation zero. Their input records must have generation
-1; timeout/rate timers use the new ID and current seed/scenario/replication
context. Chart initialization emits no publications. Observation spacing does
not change event deadlines.

Spatial validation examines the complete resulting batch. A birth may occupy a
cell released by any retirement in that batch; collisions and invalid coordinates
fail. [Population-owned networks](ABM_MUTABLE_NETWORKS.md) remove retired vertices and
incident edges; newborns are initially isolated and may be connected by a scheduled
edit at birth. Self inclusion remains available. Population
and spatial queries read the current live membership.

Scheduled direct-message and topic endpoints must be live after that timestamp's
lifecycle batch; lint checks these lifetimes. Behavioral publications retain their
runtime liveness checks. A due timer that queues a publication to/from a retiring
agent causes the transition to fail: queued messages are never silently discarded.

## Observations and native API

`{"id":"present", "agent":2, "metric":"alive"}` returns zero before birth
and after retirement, and one while live. Per-agent field/query outputs accept
an optional finite `inactive_value`; without it, an inactive read remains an error.
Aggregates count only live records and never read tombstones.

`PopulationAtomic::LifecycleInput` is accepted on port 3. Its `retirements` use
typed, namespaced references and its `births` are full typed records. Native
`configure_births` installs an optional pure initializer before execution. It sees
all raw newborn records after all retirements and returns the newborn record plus
absolute wakeups. Each initializer sees the same raw snapshot; initializers cannot
observe one another's returned updates. Sync populations reject newborn timers.

Native lifecycle inputs share a batch within one DEVS input bag. As with other
wrapper inputs, later same-time microsteps are separate transactions. The IR
schedule injects all entries for a timestamp together.

The wrapper stages lifecycle effects together with timers, messages, population
state, spatial validation, counters and output publication. A failure restores the
whole transition and its ID allocation; retry receives the same IDs. Copies own
independent state. Wider native message variants that omit lifecycle inputs retain
their existing behavior.

## Validation and remaining work

Native checks cover sequence permutations, release/reoccupy across entries,
nonzero namespaces, monotonic IDs, clone independence, same-time tick/message/timer
ordering, newborn initialization, future timer cancellation, malformed records,
duplicate/foreign/dead references, wrong ports/times, initializer failure,
sync timer rejection and queued-endpoint retirement rollback.

An independent Python recurrence/calendar checks **480 CLI observations**:
six birth times including zero/off-grid/horizon cases, an async timeout/message
history, and newborn rate histories across nine seed/scenario/replication contexts
including zero rates. Additional cases check empty initial populations, declaration
permutations, network isolation, spatial replacement, inactive outputs, topic
broadcasts, replay and observation-density invariance. Closed schema/loader cases
cover the new structures and semantic lifetime failures. Current regression totals
are in [status](STATUS.md).

[Phase/transition lifecycle actions](ABM_BEHAVIOR_LIFECYCLE.md) are now supported.
Scheduled births cannot coexist with behavior birth declarations; scheduled
retirements retain runtime liveness checks. Topic-handler lifecycle, behavior-generated network edits,
graph-generator/model population distributions and sync/async convergence
remain M4 work. These calendar checks do not establish the full canonical ABM model acceptance gates.
