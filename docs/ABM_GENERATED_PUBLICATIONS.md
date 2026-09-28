# Publications from agent behavior

Synchronous phases and flat statechart transitions can now publish numeric topic messages through the population's transactional outbox. This extends [topic handlers and replies](ABM_TOPICS.md) without external mutable broker state.

```sh
./build/fathom run models/typed_abm_phase_publish.ir.json
./build/fathom run models/typed_abm_transition_publish.ir.json
```

The [phase fixture](../models/typed_abm_phase_publish.ir.json) adds ten to each agent's value, doubles it in a second phase, and publishes each phase's input value to the same agent. Delivery follows the complete tick, giving the recurrence `next = 4*previous + 30`. Starting values 1 and 2 therefore become 34 and 38 at the first tick.

The [transition fixture](../models/typed_abm_transition_publish.ir.json) publishes credits from direct-message and timeout transitions. A transition can update its event counter while publishing a value computed from the previous counter. Rate transitions use the same publication path and existing addressed Philox scheduling.

## Declarative contract

Both phase and transition objects accept an optional `publish` array:

```json
"publish": [
  {"topic":"credit", "receiver":"self", "value":"count+1"}
]
```

The topic must be declared on the population. `value` is an expression over the sending agent's fields, parameters and neighborhood query aliases; it must have the topic's payload unit. Receivers can be `self`, `broadcast`, or a numeric existing agent ID. Broadcast includes the sender. A phase keeps its required `assign` array; an empty assignment list permits a publication-only phase.

Topic-handler-only context is unavailable: `receiver:"sender"`, `message_value` and `message_sender` are rejected in phase/transition publishers. String payloads, delayed delivery, dynamic subscriptions and cross-population routing remain unsupported.

Every live agent publishes in each configured synchronous phase. The publisher and all assignment RHSs read that phase's input snapshot. Publications accumulate in phase order, then stable agent-ID order, then list order. No publication is delivered between phases. Capacity applies to the complete pending batch, so messages from multiple phases to the same topic count together.

Only a selected statechart transition publishes. Priority/guard selection, stale generation checks and timeout/rate scheduling are unchanged. The publisher reads pre-transition fields, before the action and engine-owned state/entry/generation changes. Initialization does not publish. False guards, stale timers and unselected transitions do not evaluate publication expressions.

## Delivery and transaction boundary

At a wrapper timestamp:

1. Already-due timers and direct commands update staged state and buffer their generated publications.
2. External topic inputs and their reply cascades drain.
3. The buffered timer/command publications and their replies drain.
4. Newly due same-time timers run and their publications drain, repeating until no such work remains.
5. For a synchronous tick, all phases run, then their accumulated publications and replies drain once the tick is complete.
6. The wrapper commits and publishes its result through the usual zero-time DEVS step.

External explicit sequences and generated sequences occupy separate batches. This prevents an external sequence zero from colliding with a generated sequence zero from the same sender. Generated publications and replies share one monotonically increasing counter per agent, allocated when they enter the broker. Within a batch, canonical topic/sender/sequence/recipient ordering remains unchanged. Round numbers increase across all nonempty delivery batches in the transition.

Both budgets span the complete wrapper transaction: the async timer limit counts all timer drains, and the delivery budget counts all recipient callbacks. A message→timer→message loop cannot evade these limits by starting a new batch. Failure restores records, phase results, timers, outboxes, message IDs, broker buffers, delivery counters and clock together. Result publication and checked downstream retry retain the existing behavior.

## Native interfaces

`PopulationEmission<Tag>` holds a topic, optional receiver reference and numeric value. The owning phase/statechart supplies the sender. `PopulationPublication<Tag>` additionally holds that sender and is carried in `AsyncPopulation::Effects::publications` and the population outbox.

- `TypedPopulation::add_phase(rule, publisher)` accepts an optional pure publisher alongside the existing record rule.
- `Statechart::Transition::publish` is an optional pure publisher for the selected transition.
- Sync `apply` accepts publications as its fourth argument; async effect batches carry them alongside record/calendar effects.
- Both population types expose read-only `pending_publications()` and mutating `take_publications()`. Taking the outbox transfers its contents and clears it.

Publication endpoints are namespace-checked and must remain live while queued. Retiring a queued sender/receiver fails; drain the outbox before retiring it. Payloads must be finite and topics nonempty. Topic declaration/capacity validation happens at the broker owner. A population with an undrained outbox cannot be inserted into `PopulationAtomic`, since those messages have no owner delivery timestamp. Standalone callers explicitly consume their outboxes. Copies own independent buffers, and failures leave previous buffers unchanged.

Native topic handlers can also return effect publications. Those join the next pending round before that handler's explicit reply list. Existing handlers and models without generated publications retain their results.

## Evidence and remaining work

Native tests check pre-phase/pre-transition snapshots, delayed delivery until tick completion, outbox transfer/copy/liveness validation, phase and broker failures, deterministic retry, explicit/generated sequence isolation, same-time timer/publication feedback, cumulative budget rollback, effect-publication/reply ordering, initialization, stale timers and false guards.

The CLI contract compares **400 observations** with independently written recurrences/calendars: twelve synchronous self/broadcast cases, a message/timeout calendar with a coincident external publication, and rate-driven credits in the default context plus nine seed/scenario/replication contexts, including rate zero. It also checks deterministic replay, declaration permutations, async observation density, competing transitions, stale timeout suppression, runtime expression/capacity failures and unit rejection. These are deterministic protocol and scheduling checks, not Mesa model-distribution evidence.

Closed-schema/loader tests cover all publication contexts and reject missing fields, invalid targets, unknown topics/agents, unavailable sender context and wrong payload units. See [status](STATUS.md) for the current normal/sanitizer totals.

[Scheduled lifecycle support](ABM_LIFECYCLE.md) now composes with publications and rejects retirement of queued endpoints transactionally. [Phase/transition lifecycle](ABM_BEHAVIOR_LIFECYCLE.md) now suppresses a retiring parent's publications and preserves queued-endpoint validation. Topic-handler lifecycle and behavior-generated graph edits remain extensions. [M4 acceptance](M4_ACCEPTANCE.md) records the completed random-graph, canonical-model and sync/async convergence evidence.
