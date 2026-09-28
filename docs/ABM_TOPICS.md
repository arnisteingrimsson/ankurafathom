# Population topic delivery

Typed ABM populations now connect the bounded topic broker to the population's DEVS transaction. Synchronous and asynchronous models can receive scheduled numeric messages, update recipient fields, and send replies or broadcasts through named topics.

```sh
./build/fathom lint models/typed_abm_topics.ir.json
./build/fathom run models/typed_abm_topics.ir.json
```

The [request/acknowledgement fixture](../models/typed_abm_topics.ir.json) broadcasts a request off the synchronous tick grid, collects replies, then delivers another request exactly at a tick. The topic updates run before that tick's phases.

## Model format

A population's optional `topics` list declares payload units, capacity and a handler:

```json
"topics": [
  {
    "id":"request", "capacity":4, "unit":"1",
    "assign":[{"field":"received", "expr":"received+message_value"}],
    "publish":[{"topic":"ack", "receiver":"sender", "value":"message_value*2"}]
  },
  {
    "id":"ack", "capacity":4, "unit":"1",
    "assign":[{"field":"received", "expr":"received+message_value"}]
  }
],
"publications": [
  {"time":0.25, "topic":"request", "sender":0, "sequence":0, "value":1}
],
"delivery_budget":100000
```

A scheduled publication with no `receiver` broadcasts to every live agent, including its sender. A numeric receiver selects one agent. Each publication supplies a sequence; `(time, topic, sender, sequence)` must be unique, independent of recipient. Values are numeric literals in the declared topic unit. Publication times may fall between observations but must lie within the model horizon.

A handler's optional `guard` is a dimensionless expression. Zero suppresses assignments and replies. Expressions read recipient fields, parameters and declared neighborhood queries, plus `message_value` in the topic unit and dimensionless `message_sender`. These two names are reserved when topics are declared. All assignments and reply expressions read the pre-handler snapshot. Replies select a declared topic and an expression with that topic's unit. `receiver` accepts `self`, `sender`, `broadcast`, or a numeric agent ID. Reply sender is the handling agent, with an automatically allocated sequence.

Empty handlers are valid. A zero-capacity topic rejects every attempted publication. Capacity counts publications buffered for one delivery round, before broadcast expansion. The delivery budget counts recipient callbacks, including callbacks whose guard is false; it defaults to 100000 and must be between 1 and 1000000 in IR. Capacity is between zero and 1000000 in IR.

Async topic handlers can update ordinary fields but cannot write the chart's state, entry time or generation. Receipt does not implicitly fire a chart transition. Their field changes are visible to subsequent chart guards/actions. Hierarchical topics, dynamic subscriptions, delayed replies, nonnumeric payloads, cross-population routing, and topic-handler lifecycle effects remain unsupported. [Phase and statechart publications](ABM_GENERATED_PUBLICATIONS.md) are now supported.

## Ordering and rollback

At one physical timestamp the population wrapper processes:

1. Previously due async timers.
2. Ordinary direct commands, in agent/sequence order.
3. Topic delivery rounds.
4. Buffered timer/command publications, followed by repeated immediate-timer/publication drains until quiescence.
5. For a synchronous tick, every phase followed by delivery of the full tick's buffered publications.
6. A zero-time DEVS publication of the committed result.

Within each topic round, topics sort by name, publications by sender/sequence, and broadcast recipients by agent ID. Each callback sees earlier callbacks' staged updates. Replies stay pending until the next round at the same physical time, even if their topic sorts later. There is no recursive callback delivery. Successful draining clears broker visible/pending buffers.

The population, broker, automatic reply sequences, cumulative delivery count, clock and prepared result commit together. Capacity overflow, malformed endpoints, callback/type/spatial failures, a reply loop reaching the budget, or a later timer/phase failure rolls back everything in that wrapper transition. Publication retry after a downstream failure emits the already committed result; it does not rerun handlers.

Native `PopulationAtomic` accepts topic publications on port 2 through `PopulationTopicInput<Tag>`. `configure_topics` supplies capacities, a pure handler and a delivery budget before execution. The handler returns ordinary population effects and publications; lifecycle effects are rejected, sync timers are rejected, and native async handlers may schedule/cancel timers. The default message variant now includes topic inputs alongside commands and results. Wider variants that omit topic input remain usable for existing compositions.

`PopulationResult` carries the canonical delivery trace `(time, round, topic, sender, receiver, sequence, value)`. `delivered_count()` and `next_message_sequence(agent)` expose committed counters. External explicit sequences and internally generated sequences are separate sources; broker uniqueness is scoped to one pending topic batch. Copies own broker buffers and counters independently; callbacks must remain pure.

## Evidence

Native tests exercise broadcast/reply rounds, exact traces under input permutations, reply counter allocation and clone independence, unknown namespaces and duplicate sequences, capacity failures, bounded reply cycles, callback retry, timer/command/topic confluence, later-phase rollback, time-zero configuration restrictions, unsupported lifecycle effects, and checked downstream publication retry.

A separately implemented Python integer-clock scheduler checks **24 request/acknowledgement scenarios and 840 CLI observations**, plus reversed publication/topic declarations. Its handlers and tick rule are direct recurrences; it does not reuse the C++ broker or expression implementation. Additional hand cases cover the runnable fixture, pre-handler reads, false guards, all reply-target forms, async timer confluence, observation density, payload units, reserved chart fields and runtime failures. These are deterministic protocol tests, not independent Mesa model-distribution evidence.

The same tests run under ASan/UBSan. Schema/loader conformance covers closed topic/publication objects, capacities/budgets, required bindings, units, duplicate keys and agent references. See [current status](STATUS.md) for the final check counts. The [generated-publication increment](ABM_GENERATED_PUBLICATIONS.md) adds phase/transition publishers and 400 independent observations. [M4 acceptance](M4_ACCEPTANCE.md) records the completed population/graph and canonical-model/convergence evidence and remaining extensions.
