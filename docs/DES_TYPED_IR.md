# Declarative typed process graphs

Standalone `mode: des` models can declare an `entity_type` and compose separate source, queue, delay, seize/release, selection, and sink blocks. The existing server-based dialect remains supported. Typed graphs use runtime real/integer/boolean/string columns, immutable records during execution, and value-owned process state with checked DEVS steps.

```sh
./build/fathom lint models/typed_resource_process.ir.json
./build/fathom run models/typed_resource_process.ir.json
./build/fathom run models/typed_queue_process.ir.json
./build/fathom run models/typed_routing_process.ir.json
./build/fathom run models/typed_stochastic_process.ir.json
./build/fathom run models/typed_capacity_process.ir.json
```

The first fixture supplies three jobs to one priority resource. Completion times are 1, 3, and 6 days; total cycle time is 9.5 days and total queue time is 3.5 days. Fields include duration, integer rank, an urgency flag, and a label. A dimensionless `speed` parameter changes delivery duration through `duration / speed`.

## Vocabulary

| Kind | Configuration | Key observations |
|---|---|---|
| `entity_type` | Ordered `fields`: `id`, `type`, numeric `unit` | Defines storage; not an atomic |
| `source` | `entity_type`, exactly one `schedule` or `generator`, optional `priority` expression | `emitted` |
| `queue` | `entity_type`, optional waiting `capacity`, `discipline`, optional priority expression | `waiting`, `accepted`, `released`, `rejected`, `queue_mean`, `wait_total` |
| `delay_block` | `entity_type`, time-dimensioned `duration` expression or exponential policy, optional positive `capacity` | `active`, `accepted`, `completed`, `rejected`, `in_process_mean`, bounded `utilization` |
| `resource_pool` | Capacity integer or expression, `max_request_units`, discipline, optional capacity schedule | `capacity`, `allocated`, `waiting`, `utilization`, `queue_mean`, `allocated_mean` |
| `seize` | `entity_type`, `pool`, dimensionless integer `units` expression, optional priority, `preempt: false` | `received`, `waiting`, `granted` |
| `release` | `entity_type`, `pool` | `released` |
| `select_output` | `entity_type`, ordered conditional branches and `otherwise`, or probability branches and a stream | `received`, `routed` with named `port` |
| `sink` | `entity_type` | `completed`, `cycle_total`, `cycle_mean` |

Each output has `id`, `component`, and `metric`; a `routed` metric also requires `port`. Zero elapsed time and no completions return zero for otherwise undefined means. Counts and statistics are sampled after all events at the observation time.

Numeric expressions read declared parameters and numeric fields. Dimensions are checked at load. Booleans and strings are preserved but do not implicitly convert to numbers. Integer storage is signed 64-bit; integers entering floating-point arithmetic must lie within ±2^53. Priority expressions must produce signed 32-bit integers, and units/capacity expressions must produce bounded integers at runtime. Provider results are cached at admission.

## Arrivals and random service

Explicit source entries are `{arrival, values}`. Values must contain exactly the declared fields. A generator instead declares `count`, optional `start`, one typed `values` record, and `interarrival`:

```json
{"count": 24, "values": {"duration": 1, "rank": 0},
 "interarrival": {"kind": "exponential", "rate": "arrival_rate", "stream": 100}}
```

Constant interarrival uses `{kind: constant, interval: expression}`. The first arrival follows one interval from start. Generated work beyond the horizon remains unemitted. Delay service can use `{kind: exponential, rate: expression, stream: 101}`. Rates have inverse-time units, and intervals have time units. Source rate/interval expressions use parameters only; service rates may also read entity fields.

All stochastic blocks own distinct explicit streams. Draws are addressed by seed, scenario, replication, entity ID, and stream, preserving replay and observation-grid independence. Entity type namespaces, source allocation order, pool binding, and component execution order follow sorted identifiers, making component/link declaration order irrelevant. Record order within one source remains meaningful.

## Wiring and ownership

Entity links have `from`, `to`, and optional named `port` (`out` by default). A queue connects to one bounded delay, which must receive all its tokens from that queue; the loader adds reverse slot credits. Finite queues and directly fed bounded delays require explicit rejection paths. Every selection branch requires a destination, even when its probability is zero. Merges preserve the entity type and cannot duplicate identities.

Seize/release blocks name a resource pool; the loader assigns unique request namespaces and adds protocol links automatically. Multiple sources and entity types can share a pool. Static analysis rejects cycles, fan-out, missing exits, incompatible types, unreachable blocks, inconsistent held-resource sets at merges, and sink paths with unreleased leases. Nested acquisitions must follow increasing pool IDs to prevent circular hold-and-wait.

Pool capacity schedules inject changes at exact times. A reduction below allocated units fails, including when a delivery completes at that time but its release is still traveling through zero-time blocks. Resource utilization divides allocated-unit time by actual capacity-time, including zero-capacity intervals and safe resizes.

## Evidence and limits

The declarative queue and resource graphs pass **120 pinned SimPy/Ciw cases and 76,576 sampled observations**, with maximum absolute difference about **2.84e-14**. Expected queues, busy times, and cycle sums are reconstructed from independent per-entity reference histories. Hand tests cover finite-queue rejection, direct loss service, shared pools across two entity namespaces, condition/categorical routing, admission priority overrides, capacity changes, and generated schedules. Stochastic typed queues match the existing validated one-/two-server path across experiment addresses. Tests also check declaration permutations, denser observation grids, replay, parameter overrides, structured errors, and invalid topology/type/unit combinations.

The [JSON Schema](../ir/schema/ankurafathom-ir.schema.json) independently checks structural shapes. Semantic checks remain in the loader; schema-valid documents can still fail unit, record, graph, and ownership checks. This dialect does not yet provide loops/revisits, mutable entity assignments, cancellation, preemption, named random streams, general random expressions, or typed hybrid graph integration. Those are explicit extensions, not silently approximated behaviors. See [semantics](SEMANTICS.md#declarative-typed-des-graphs) and [current verification](STATUS.md).
