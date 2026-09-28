# Mutable networks in typed populations

Typed populations now own their network alongside agent records. Scheduled edge
edits, births and retirements participate in the same DEVS transition rollback
boundary, and every network query reads the current staged topology.

```sh
./build/fathom run models/typed_abm_network_updates.ir.json
```

The [fixture](../models/typed_abm_network_updates.ir.json) rewires collaborations,
retires an agent, connects its replacement at birth and propagates values over
the resulting graph.

## Declarative schedule

A population with a `network` declaration can include:

```json
"network_updates": [
  {"time": 1, "sequence": 0, "add": [[1,2]], "remove": [[0,1]]},
  {"time": 2, "sequence": 0, "add": [[0,3]], "remove": []}
]
```

All four properties are required. Empty arrays are legal. Sequences must be unique
within a timestamp. All entries at that timestamp form one edit batch: removals
and additions must be unique, disjoint, and valid against the graph after that
time's scheduled lifecycle changes. An existing addition or missing removal fails.
An undirected pair and its reverse are the same edge; directed pairs retain their
orientation. Self-loops are rejected. Batch behavior is independent of declaration
order. Removing and re-adding the same edge in one batch is an overlap error.

Endpoints may reference initial or scheduled-born IDs. Lint checks scheduled
lifetimes, self-loops, ranges and duplicate sequences. Runtime checks actual live
membership and edge existence, because agent behavior can retire an endpoint or
remove its incident edges before a scheduled edit. Behavioral newborn IDs cannot
be forward-referenced numerically in IR; native inputs can address them once live.

## Membership, queries and ordering

Every live agent is a graph vertex. Births add isolated vertices; retirements
remove vertices and all incident incoming/outgoing edges. A replacement receives a
new ID and no inherited edges. No stale edges or tombstone vertices remain in the
population graph. Initial configuration must contain exactly the live IDs.

At an event timestamp:

1. Already-due async timers execute against the current graph.
2. Scheduled retirements and births apply.
3. The combined edge-edit batch applies.
4. Direct messages and external topics run, followed by the existing generated
   publication/immediate-timer closure and synchronous phases.

Thus an edit can connect an agent at its scheduled birth time. Later callbacks,
phases and observations see the changed graph. Phase-driven membership changes
commit after that phase's snapshot evaluations; subsequent phases see the removed
edges and isolated children. Old queued publications retain their liveness rules.
Later same-time DEVS microsteps are separate transactions.

Errors in edits, custom population validation, commands, topic delivery or later
phases restore the complete owning transition. Already-due timer effects, records,
allocated IDs, topology, queues and output publication roll back together. A failed
downstream result receiver retains the committed result for checked retry without
repeating the original lifecycle or edge changes.

## Native API

`PopulationStore<Tag>` composes the existing `RuntimeEntityStore<Tag>` with an
optional value-owned `CsrNetwork`. Its record API preserves exact field types,
namespace checks, monotonic IDs and tombstones. Existing record stores can initialize
it. A read-only conversion exposes the underlying record store to spatial helpers;
no mutable underlying store is exposed. Copies own independent records and graph
state, and successful membership/edge changes invalidate prior graph row views.

Configure a store's graph with `configure_network(CsrNetwork(...))` before inserting
it into a population. Use `network()` to inspect its optional read-only graph.
Sync/async population `edit_network(additions, removals)` accepts pairs of typed,
namespaced references, stages the change, runs the custom validator and commits
only on success. Reentrant mutation from callbacks remains rejected.

`PopulationNetworkInput<Tag>` is a new default payload alternative accepted by
`PopulationAtomic` on port 4. It carries time, sequence, additions and removals.
The payload extractor also supports wider message variants that omit this input.
`PopulationResult::snapshot` now includes the graph through `PopulationStore`.
Neither query closures nor the wrapper keep an external mutable graph cache.

## Evidence and remaining work

Native tests cover directed/undirected membership, nonzero IDs/namespaces, invalid
configuration, isolated births, incident-edge cleanup, empty graphs, copies,
custom-validator rejection/retry, phase snapshots, timer/lifecycle/edit/command
ordering, input permutations, duplicate/overlapping edits, missing edges, malformed
ports/times, failed-batch rollback and checked downstream publication retry.

An independent Python edge-set/event recurrence agrees on **873 observations**:
twelve evolving directed/undirected graph cases with value propagation and scheduled
replacement, plus an async timer/message/topic history. Additional checks cover
reversed undirected declarations, replay, observation-density invariance, time-zero
edits, phase-driven retirement, stale scheduled endpoints and invalid edit batches.
The pre-existing pinned Mesa/NetworkX comparisons remain part of the affected
regression suite. These new recurrences establish the declared edit semantics;
they do not establish the full canonical ABM distribution/convergence gates.

[Declarative graph generators](ABM_GRAPH_GENERATORS.md) now initialize population-owned networks.
Behavior-generated edge edits, topic-handler lifecycle,
multiple populations and remaining reference-model acceptance remain open. The
implementation rebuilds graph storage for correctness; performance work comes later.
See [status](STATUS.md) for current local validation results.
