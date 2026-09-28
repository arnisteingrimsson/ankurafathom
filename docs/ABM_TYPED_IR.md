# Typed population DEVS and IR

`mode: "abm"` runs one typed population through the DEVS kernel. It supports ordered synchronous phases or an asynchronous flat statechart, named real/integer/boolean/string fields, unit-checked arithmetic, scenario parameter overrides and numeric observations. This is the first general typed-agent IR subset; [spatial/network neighborhood composition](ABM_INTERACTIONS.md) is now supported. [Topic delivery](ABM_TOPICS.md) is also supported. [Scheduled births and retirements](ABM_LIFECYCLE.md) are now supported; [phase/transition lifecycle](ABM_BEHAVIOR_LIFECYCLE.md) is also supported. [Mutable networks](ABM_MUTABLE_NETWORKS.md) now support scheduled edge edits. Topic-handler lifecycle, multiple populations and hybrid links remain later increments.

## Runnable examples

```sh
./build/fathom lint models/typed_abm_sync.ir.json
./build/fathom run models/typed_abm_sync.ir.json
./build/fathom run models/typed_abm_async.ir.json
./build/fathom run models/typed_abm_rates.ir.json
./build/fathom run models/typed_abm_neighbors.ir.json
./build/fathom run models/typed_abm_topics.ir.json
./build/fathom run models/typed_abm_phase_publish.ir.json
./build/fathom run models/typed_abm_transition_publish.ir.json
./build/fathom run models/typed_abm_lifecycle.ir.json
./build/fathom run models/typed_abm_behavior.ir.json
./build/fathom run models/typed_abm_replacement.ir.json
./build/fathom run models/typed_abm_network_updates.ir.json
./build/fathom run models/typed_abm_network_generator.ir.json
```

- [Synchronous workload](../models/typed_abm_sync.ir.json): exact integer work fields and boolean readiness, two ordered phases, agent and aggregate outputs. At each tick the first phase adds `gain`; the second doubles the updated work.
- [Async workflow](../models/typed_abm_async.ir.json): guarded start messages, a fixed-duration completion, and a restart message at the exact completion time. Completion runs first, so the message can restart the newly completed agent.
- [Renewal events](../models/typed_abm_rates.ir.json): a constant-rate self-transition with an increment action, independent per-agent Philox draws and replay across scenario/replication contexts.

Each document uses the existing `time`, `parameters` and `outputs` conventions and exactly one `components` entry of `kind: "population"`. Its `fields` declare name, type and unit, and `agents` contain full named records. IDs follow agent-array order. Every record must contain exactly its schema fields; there is no numeric/boolean coercion. Integers are restricted to the exact f64 range ±(2^53−1) for this expression-driven dialect. Strings are stored but do not participate in arithmetic or numeric outputs.

Synchronous populations declare `phases`, each with an `assign` array of `{field, expr}`. Every expression in a phase reads its pre-phase snapshot; phases commit in declaration order. Async populations instead declare `chart` with reserved state/entry/generation bindings, states, initial state and transitions. Initialization requires generation -1. All assignments and guards read pre-transition fields, parameters and declared query aliases; engine-owned fields cannot be assigned. Guards interpret a dimensionless result as nonzero. Boolean assignments must yield exactly zero or one, and integer assignments cannot round fractional values. Expressions support arithmetic and `NONNEGATIVE`; time/neighbor/aggregate symbols are not implicit.

Timeout `duration` and constant `rate` are expressions over parameters, evaluated once per run after overrides. Their units must match model time and reciprocal time respectively. Rate transitions require distinct 16-bit `stream` IDs. Zero disables a rate. Scheduled `messages` declare time, agent, explicit sequence and event; equal-time entries may appear in any declaration order. Outputs select `{agent, field}`, `{metric: "active"}`, `{metric: "sum", field}`, `{metric: "state_count", state}`, or `{agent, query}`. Lifecycle observations add `{agent, metric: "alive"}` and optional `inactive_value` for agent field/query outputs. Integer aggregates fail if accumulation leaves the exact numeric range.

`time.dt` controls observation spacing and synchronous ticks. It does not discretize async event deadlines. The runtime drains every event and publication microstep at or before an observation before sampling. The rate fixture's CLI experiment support uses the existing `--experiment` format. Numeric rows remain `(scenario, replication, time, output_id, value)` for experiments.

## Native DEVS wrapper

`PopulationAtomic<Tag, Payload>` owns either `TypedPopulation<Tag>` plus a tick interval, or `AsyncPopulation<Tag>`. The default payload is a variant of `PopulationCommand<Tag>`, `PopulationResult<Tag>` `PopulationTopicInput<Tag>`, `PopulationLifecycleInput<Tag>` and `PopulationNetworkInput<Tag>`; a wider variant can support explicit adapters. Input port 0 takes timestamped agent commands; port 3 takes atomic lifecycle batches and port 4 takes edge-edit batches. A pure input callback returns ordinary population effects; synchronous instances reject timer effects.

Input bags order by agent ID then explicit sequence, rejecting duplicate keys, wrong namespaces, invalid ports and mismatched timestamps. For async confluence, due timers run first, then scheduled lifecycle and edge-edit batches, then the ordered messages, then newly armed immediate timers. For sync confluence, lifecycle batches and messages run before the tick's phases. A complete transition stages all effects and prepares its result before committing. The async budget spans all timer groups, including message-generated feedback; see [generated delivery ordering](ABM_GENERATED_PUBLICATIONS.md).

Output port 1 publishes the committed store and event traces in a zero-time microstep. A simultaneous input during publication starts a new transaction without repeating the earlier tick/timers. Deep clones let checked kernel execution restore a failed receiver and pending publication. Exact absolute deadlines preserve fractional-clock behavior. The wrapper and its callbacks own no mutable external simulation state.

## Evidence and remaining gates

The native wrapper test covers sync/async confluence, input permutations, exact timestamp rejection, clone independence, combined event-budget rollback, direct async versus DEVS calendar agreement over 60 timers, and checked retry after downstream publication failure.

The CLI contract checks hand recurrences, exact completion/restart history, declaration permutations, observation-density invariance, empty populations, runtime type failures and unsupported inputs. A separately implemented Python Philox/renewal calendar checks **297 observations across nine scenario/replication runs**, including disabled rates; repeated CLI execution is identical. These deterministic checks verify addressed event scheduling, not Mesa model distributional agreement.

The closed Draft 2020-12 schema keeps existing dialects in a separate branch. Schema/loader conformance passes **51 valid fixtures plus 13 interaction and six generator cases, 377 structural invalid cases and 220 semantic invalid cases**, including the previous modes. The [interaction increment](ABM_INTERACTIONS.md) adds validated movement and 4,212 independent trajectory observations. [Topic integration](ABM_TOPICS.md) adds transactional delivery rounds and 840 independent scheduler observations. [Phase/transition publications](ABM_GENERATED_PUBLICATIONS.md) add transactional outboxes and 400 independent observations. [Scheduled lifecycle](ABM_LIFECYCLE.md) adds 480 independent observations. [Behavior lifecycle](ABM_BEHAVIOR_LIFECYCLE.md) adds 669 independent observations. [Mutable networks](ABM_MUTABLE_NETWORKS.md) add 873 independent observations. [Graph generators](ABM_GRAPH_GENERATORS.md) add 2,808 exact addressed observations and 51 predeclared statistical/analytical gates over 4,608 graphs per engine. [M4 acceptance](M4_ACCEPTANCE.md) records completed reference-model distribution and sync/async convergence gates; topic-handler lifecycle and behavior-generated edge edits remain extensions. Hierarchical states and changing hazards remain unsupported. See [current status](STATUS.md) for build totals.
