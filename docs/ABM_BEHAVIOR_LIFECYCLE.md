# Births and retirements driven by agent behavior

Typed synchronous phases and asynchronous statechart transitions now support
conditional births and self-retirement. These actions share the existing rollback
boundary with records, spatial occupancy, timers, publications and ID allocation.
[Scheduled lifecycle](ABM_LIFECYCLE.md) remains available for external workforce
changes.

```sh
./build/fathom run models/typed_abm_behavior.ir.json
./build/fathom run models/typed_abm_replacement.ir.json
```

The [phase fixture](../models/typed_abm_behavior.ir.json) replaces agents as they
age, copies parent-derived values into children, and updates children in a second
phase. The [rate fixture](../models/typed_abm_replacement.ir.json) replaces agents
at independently addressed random deadlines while preserving two live agents.

## Declarative contract

A phase or transition accepts an optional `lifecycle` object:

```json
"lifecycle": {
  "retire": "age",
  "births": [{
    "guard": "age",
    "record": {"age": 0, "lineage": 0, "work": 0},
    "assign": [
      {"field": "lineage", "expr": "lineage+1"},
      {"field": "work", "expr": "work*2"}
    ]
  }]
}
```

`retire` is a dimensionless expression: nonzero means retire the current agent.
Omitting it preserves the parent. `births` is an ordered list; each birth requires
a complete literal record matching the population schema. An optional nonzero
`guard` enables that birth. Optional assignments override numeric/boolean fields
using unit-checked expressions. String fields are literal template values.
Expressions read the parent and queries from the pre-action snapshot, not the
birth template or earlier assignments. A false birth guard skips assignments.
An empty lifecycle object is invalid; an explicit empty birth list is legal.
Synchronous phases still require `assign`, which may be an empty array.

A retiring parent does not execute ordinary assignments, emit publications, or
enter the transition target. Its birth actions still execute, allowing replacement.
A survivor executes ordinary assignments/publications with their existing snapshot
rules. Retirement cannot invalidate endpoints of publications queued by earlier
work: that condition rejects the transaction instead of dropping messages.

## Membership and timing

A synchronous phase evaluates only agents live at phase entry, in increasing ID
order. It collects updates, retirements and births before applying the complete
batch. Newborn IDs follow parent order and birth-list order. Newborns join subsequent
phases, including later phases of the same tick, but never execute their own birth
phase. A replacement can reuse its parent's cell; all resulting positions must
satisfy the population's spatial constraints. Failure in any phase restores the
whole tick. Deferred broadcasts from surviving parents include agents live at
delivery, including children born during that tick.

Async lifecycle actions execute only on the selected, enabled message/timeout/rate
transition. False guards, stale timers, unselected transitions and chart initialization
have no lifecycle effects. Each child starts in the chart's declared initial state,
at the event timestamp, with generation zero and a new ID. Template generation must
be -1; birth assignments cannot write chart-owned state, entry or generation fields.
The chart overwrites template state/entry fields during initialization. Child timers
use the existing seed/scenario/replication/ID/generation/stream addressing scheme.

Parent retirement cancels all its remaining timers. Immediate newborn timers join
the timestamp closure, bounded by the existing cumulative event budget. All effects
of a failed timestamp roll back, including children and timer identities. Native
statechart lifecycle callbacks return raw records; the constructor's optional
`birth_initial` argument selects their initial state. Requesting births without it
fails explicitly. Native phase callbacks are the third `add_phase` argument and
return `TypedPopulation::Lifecycle {retire, births}`. Callbacks must remain pure.

## Limits and references

Optional population `agent_limit` defaults to one million and must be between one
and one million. It bounds total allocated IDs, including retired agents, so repeated
replacement also consumes the limit. Initial and scheduled allocation is checked at
lint; behavioral allocation is checked inside the transaction at runtime.

Scheduled births and behavior birth declarations cannot coexist: behavioral events
would invalidate predicted scheduled IDs. This is checked even for currently disabled
birth guards. Scheduled retirements/messages referencing initial agents remain
supported, but behavior can retire those agents earlier; such stale external inputs
fail at runtime. Initial-agent `alive` and `inactive_value` outputs retain their
semantics. Dynamically born agents are observed through aggregates and live queries;
self/broadcast publications can reach them. There is no forward numeric-reference
syntax for behavior-assigned IDs in this increment.

[Population-owned networks](ABM_MUTABLE_NETWORKS.md) remove retired vertices and
incident edges; newborns are initially isolated. Scheduled edge edits connect
known live IDs, while spatial/population queries track current membership.
Topic handlers do not yet accept lifecycle actions. Multiple populations,
behavior-generated network edits and the canonical ABM distribution/convergence
gates remain M4 work.

## Evidence

Native tests cover phase snapshots, deterministic IDs, parent action/publication
suppression, newborn participation in later phases, atomic cell replacement,
collision rollback, clone isolation, failure/retry after a later phase, queued endpoint
protection, parent timer cancellation, newborn chart initialization, invalid template
epochs, missing birth-state configuration and bounded immediate reproduction.

Independent Python recurrences and event calendars verify **669 observations**:
twelve synchronous replacement/reproduction cases, a timeout replacement history,
and rate replacements across nine seed/scenario/replication contexts, including
zero rates. Additional checks cover message transitions, stale timers, false guards,
losing transitions, string/boolean records, spatial/network composition, newborn
broadcast delivery, allocation limits, unsupported schedule combinations, replay
and observation-density invariance. Schema/loader tests cover types, units, reserved
fields and references. These are deterministic scheduling checks, not acceptance of
the full Mesa model-distribution gates. See [status](STATUS.md) for build results.
