# DEVS conformance fixtures

These fixtures exercise independent schedulers with shared mathematical atomic
behavior. `harness.hpp` adapts that behavior to native AnkuraFathom and the pinned
[adevs reference](../../../third_party/adevs/UPSTREAM.md). It does not share event
queues, routing, scheduling, or transition selection between the engines. Hand
schedules and analytic message counts supplement the differential comparisons;
agreement alone is not an independent validation of the shared atomic behavior.

## Trace contract

Every event step records its exact physical timestamp, atomic outputs
`(component, port, value)`, root outputs, and each atomic's transition kind,
elapsed time, input bag, post-transition state, and next time advance. Integer
values and double timestamps compare exactly. Distinct steps at the same time
remain distinct: zero-time microsteps are never coalesced.

Within a step, outputs and transitions are sorted by stable fixture component ID.
Input bags and root outputs are sorted by port/value, retaining duplicates. These
fixtures define their inputs as unordered bags; even the FIFO processor orders
simultaneous arrivals by port/value before enqueueing. Adevs component iteration
order is not AnkuraFathom's insertion order. Accordingly this is equality of
canonical event traces, not a claim that arbitrary order-sensitive user models
have identical raw callbacks on both engines. Input source IDs are not compared
because the adevs input interface exposes pins rather than native source IDs.

`dynamic.cpp` separately checks native raw output and input order with distinct
payloads: injection-call order, then source ID, atomic output-vector position,
and destination-port order. It also checks that an attempted disconnect inside a
transition is rejected without removing the edge from the following step.

For each static fixture, three executions must agree: native nested compilation,
an independently expanded flat native graph, and adevs pin routing through the
unflattened boundary graph. Traces are compared in memory; failures identify the
case and first differing step. The CSV retains counts and timing, not full traces.

## DEVStone provenance and workload

Topology and queue semantics are adapted from
[SimulationEverywhere/devstone at 02fa99a8fe76bc61718a936089de3d63c1abbe71](https://github.com/SimulationEverywhere/devstone/tree/02fa99a8fe76bc61718a936089de3d63c1abbe71):
`src/dynamic/{LI,HI,HO,HOmod}_generator.cpp` and
`src/cadmium-devstone-atomic.hpp`. Copyright notices and BSD terms are retained in
[DEVSTONE_LICENSE.txt](DEVSTONE_LICENSE.txt). The upstream repository is a source
reference, not a new build dependency; the tests need no network access.

Depth includes the leaf. With width `w`, each nonleaf LI/HI/HO level contains
`w−1` local atomics and one nested level. LI broadcasts without lateral links;
HI adds a forward chain of local atomics. HO separates local input/output ports
from the nested primary path and adds the local chain. HOmod uses columns of
length `2, …, w`, both ends of each column receive secondary input, and the first
atomic in each column feeds the nested secondary input. The leaf has one atomic
on its primary port.

Each DEVStone atomic queues the number of arriving messages, emits constant `1`
for one queued item per internal transition, and resets its time advance to the
configured period after external input. Confluence performs internal then
external. There is no Dhrystone payload; this is a correctness workload. Periods
zero and one exercise microsteps and timed confluence respectively. Each fixture
receives two stimuli at time zero and one at time one, on the primary port and,
for nontrivial HO/HOmod hierarchies, the secondary port.

The 48 cases cover four families, periods `{0, 1}`, and `(width, depth)` pairs
`(1,1), (2,2), (3,3), (5,3), (3,6), (8,4)`.

Let `n = w−1`, `d = depth`, and `S = 3` stimuli. Besides trace equality, tests
check these counts after complete drainage:

| Family | Atomic count | Atomic outputs = internal/confluent transitions = received messages |
|---|---|---|
| LI | `1 + n(d−1)` | `S[1 + n(d−1)]` |
| HI, HO | `1 + n(d−1)` | `S[1 + n(n+1)(d−1)/2]` |
| HOmod | `1 + n(n+3)(d−1)/2` | `S[1 + n(n+5)/2 × Σ(k=0…d−2)(2n)^k]` |

The empty sum for depth one is zero. Every fixture produces `S` primary root
outputs; nonleaf HO also produces `S n(n+1)/2` secondary root outputs.

## Other declared cases

`models.cpp` contains 30 static cases: GPT; two explicit confluent policies;
ping-pong; a 64-atomic zero-time chain; two distinct boundary paths delivering
the same payload; and 24 deterministic larger graphs (8, 32, or 128 echo nodes,
each with two generators, under eight seeds). Every larger-graph atomic is
activated. The graphs combine nested boundaries, fan-out, shortcuts, feedback,
and simultaneous input; decreasing payloads bound the zero-time feedback.

`dynamic.cpp` compares two runs that retire a busy processor at time two or
three, covering removal before or just after a due output. It also adds a
periodic source at a nonzero clock, replaces a processor, disconnects and
reconnects routes, removes a source, and cancels an imminent zero-time output.
Native edits occur after `step`; adevs edits are queued for commitment after the
same step's transitions. This file validates flat between-step changes.

`hierarchy.cpp` adds four cases. Three static cases compare native nested,
independently flat, and adevs execution for direct root passthrough, nested empty
routes, duplicate payload paths, and same-time confluence. A fourth scenario
replaces a busy nested subtree, inserts another nested branch, rewires root
inputs/outputs, and recursively retires subtrees. Native nested execution agrees
with an independently expanded flat graph and adevs routing through the original
boundary pins. Both independent adapters use the reference edge specification;
neither invokes the production topology flattener. Hand-derived processor
completions, redirected sink bags, and boundary-only events additionally gate the
dynamic scenario. Inputs queued before an edit must use the delivery-time routes,
and stored boundary outputs must remain unchanged after later edits.

`tests/devs_coupled_tests.cpp` also checks native root injection/path order,
duplicate-edge disconnection, failed-edit isolation, invalid insertion schedules,
stable group and atomic IDs, preserved clocks, callback edit guards, cancellation
of direct injections into a removed subtree, consumption of disconnected queued
root inputs, and checked-step rollback/retry of root events.

Native-only checks cover duplicate edge declarations, missing-edge errors,
removed endpoints, stable IDs, discarded pending injections, raw ordering, and
the in-transition mutation guard. A native duplicate edge represents another
delivery path. Adevs reference-counts repeated identical edge declarations, so
the cross-engine multiplicity fixture uses two distinct boundary paths instead.

Run all kernel checks with:

```sh
ctest --test-dir build --output-on-failure -R '^(devs_|adevs_reference_schedule)'
```

The separate `devs_allocation_bench` executable builds `devstone.cpp` with scoped
allocation instrumentation and retains the same trace and analytic oracles.
`devs_allocation_report` writes allocation requests/bytes for all 48 cases in
CTest; `allocation_counter` verifies the instrumentation against hand counts.
Neither recorded allocation volume nor timing is compared to a performance gate.
These repeated executions do not add distinct differential cases.

See the [acceptance report](../../../docs/KERNEL_CONFORMANCE.md) and
[measurement notes](../../../bench/README.md).
