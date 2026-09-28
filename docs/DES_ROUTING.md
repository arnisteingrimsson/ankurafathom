# DES branching and rejection routing

Standalone `mode: "des"` now accepts a directed acyclic process graph with one source, one completion sink, one or more service stations, and optional priority/probability routers and discard sinks. Branches select one path per entity; merging combines arrivals without copying them. This extends the CPU reference implementation. Hybrid DES/SD models still require their existing linear process path.

## Declarative contract

A router has an integer threshold and two required exits:

```json
{"id":"triage", "kind":"router", "priority_at_most":0}
```

```json
{"from":"triage", "port":"match", "to":"urgent"}
{"from":"triage", "port":"otherwise", "to":"normal"}
```

`match` selects entities whose signed 32-bit `priority` is at most the threshold; `otherwise` selects the rest. Each input chooses exactly one exit, including when both exits target the same component. Routers preserve IDs, priority, original arrival, base service duration, and any previous completion timestamp. The alternative [probability rule](DES_PROBABILITY.md) uses an explicit addressed stream; each router requires exactly one rule. General expressions and mutable routing fields remain open.

Source output and station completion links use `port: "out"`, which may be omitted for compatibility. Station overflow may have an explicit `port: "rejected"` route to another station, a router, or a discard sink:

```json
{"from":"urgent", "port":"rejected", "to":"backup"}
{"from":"backup", "port":"rejected", "to":"lost"}
```

```json
{"id":"lost", "kind":"discard"}
```

A discard sink accepts and records arrivals without completing service. It exposes `discarded`. Router metrics are `received`, `matched`, and `otherwise`; they count committed routing decisions. Station `rejected` remains a cumulative count of failed admissions at that station, even when the rejected entity later succeeds elsewhere. Existing station and completion-sink metrics retain their meanings.

Every source and station must have exactly one `out` route. A station may have one rejection route; if omitted, overflow remains a terminal loss with its existing rejection record and no publication. A router must have exactly one route for each exit. Arbitrary fan-out from a single port is rejected: it would duplicate an entity. Multiple predecessors may merge into a station, router, or sink.

The loader validates endpoint existence, port names by component kind, required exits, reachability from the source, and acyclicity. No links may enter a source or leave a sink. It propagates completed/uncompleted path states and refuses any path that could deliver source arrivals or station rejections to the completion sink without successful subsequent service. This remains true through intermediate routers. Discard paths may receive either state. Every component must be structurally reachable; whether a branch is exercised by a particular workload is a runtime property.

Cycles, retry loops, re-entry to a station, multiple sources/completion sinks, fork/join synchronization, backpressure, and graph-based hybrid completion bridges remain outside this subset. Entity IDs cannot be reused at a station, router, or sink within a run.

## Event and ordering contract

`PriorityRouter<Message>` and `DiscardSink<Message>` are cloneable C++ atomics. They accept `Entity` directly or as a member of the existing message variant. `MultiServer` enables rejection publication with its fourth constructor argument, `emit_rejections=true`; default `false` preserves the prior API and event traces. The declarative runtime enables it only on stations with a rejection link.

A station decides admission during its existing transition. Routed rejections then emit on `MultiServer::rejection_port` (2) in a zero-time publication step at the same physical timestamp. The original rejection log remains intact. Publication does not stamp an entity as completed or reset upstream metadata. The next service completion remains at its original deadline.

Routers similarly buffer exactly one output per input and publish in a zero-time step. At confluence, the pre-transition publication is emitted, then removed before the new bag is accepted. New routed work publishes in the following zero-time step. A discard sink is passive. All three components reject invalid bags without partial state changes; checked simulator steps also restore upstream publications and downstream receiver state after a failure.

Queue admission arbitration applies to each input bag, as documented in [DES queues](DES_QUEUES.md). Paths with different counts of zero-time routing steps can reach a merge in different microsteps at the same timestamp. Later microsteps do not retroactively preempt service or displace admitted work. Simultaneous inputs within one bag use the queue's discipline. FIFO retains stable kernel order. The loader canonicalizes station topological order, router/discard IDs, and links so component/link declaration permutations do not change the trajectory. It preserves source schedule order because that order is meaningful to FIFO.

## Accounting and validation

After draining all events at an observation timestamp:

```
source emitted = completion sink count + explicit discards
               + unrouted terminal rejections + work in all queues/service slots
```

Intermediate routed rejections are failed attempts, not terminal losses. Router buffers and pending rejection publications are empty once the timestamp is drained. At an individual microstep, those buffers are additional in-flight work.

The fixture `models/routed_process.ir.json` splits urgent and normal arrivals, sends overflow to a shared bounded backup station, routes backup overflow to a discard sink, and merges successful work through a second router into the completion sink. Its eight arrivals produce six completions and two terminal losses. Completion counts at times 0, 1, 2, and 3 are 0, 1, 4, and 6; final cycle total is 10. Backup queue area is 2 and busy area is 3. There are six station rejection attempts, four of which are upstream rejections forwarded to backup; two of those four subsequently complete.

`des_routing` checks the hand trajectory, conservation at every observation, exact queue/service integrals, observation-density invariance, threshold boundaries and signed extremes, routing through message variants, rejection-publication confluence, invalid-bag rollback, and checked downstream-failure rollback/retry. `des_graph_contract` runs 32 component/link permutations, two all-match/all-otherwise cases, and nine malformed-topology diagnostics. Schema/loader checks include malformed router fields/ports and schema-valid graph errors. Existing linear process, finite queue, and hybrid tests remain regression gates.

```sh
./build/fathom run models/routed_process.ir.json
ctest --test-dir build --output-on-failure -R 'des_routing|des_graph_contract'
```

The graph checkpoint passed **78/78 CTest tests** in Debug and ASan/UBSan; current totals are in [status](STATUS.md). Schema/loader conformance passes **30 valid fixtures, 122 structural invalid cases, and 60 semantic invalid cases**. These are local results; remote CI was not run in this session.

M3 remains open for broader routing/distribution support, resource-pool disciplines, broader network validation, and additional independent comparisons as new graph behavior is added.

[Independent station service](DES_SERVICE.md) and [three-station tandem Jackson validation](DES_JACKSON_VALIDATION.md) are now available; [probabilistic branches](DES_PROBABILITY.md) are now supported, while revisits remain outside this graph contract.

[Pinned SimPy/Ciw comparisons](DES_ENGINE_ORACLES.md) now cover the unbounded tandem and probabilistic branching graphs; priority branches and finite downstream rerouting remain outside those reference models.
