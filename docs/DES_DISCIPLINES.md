# Queue and resource disciplines

The process station now supports `discipline: "lifo"` alongside FIFO and non-preemptive priority. The C++ resource pool supports all three disciplines through `ResourcePool(capacity, max_request_units, discipline)`. Existing callers retain FIFO behavior.

LIFO station admission follows stable input-bag order, preserving already accepted work. Dispatch chooses the newest admitted waiter; a simultaneous bag therefore starts its last admitted entity first. Overflow still rejects later admissions. Completion frees slots before a confluent arrival is admitted, and active service is never interrupted. The [LIFO fixture](../models/lifo_process.ir.json) completes IDs `3,5,2,1` at times `1,2,3,4`, rejects IDs `4,6`, accumulates queue area 5, and records cycle total 9. It produces the same completion pulses in the linear DES/SD hybrid.

Resource requests now carry signed integer priority and a `preempt` flag, both defaulting to zero/false. Preemption requests fail atomically. Releases and a safe capacity change precede admission and dispatch. Priority sorts a new bag by priority then request ID, and preserves older admissions among equal priorities across transitions. A new urgent request can overtake an older less urgent waiter at a release. LIFO chooses the newest request; FIFO chooses the oldest. All disciplines stop at a selected request that cannot fit; smaller requests cannot bypass it. Pending grant publication, confluence, and cloned state preserve their existing contract. See [semantics](SEMANTICS.md#queue-and-resource-arbitration).

## Evidence

- Exact hand traces check LIFO admission, loss, confluence, non-preemption, time integrals, duplicate rollback, resource head blocking, capacity changes, cloned pending grants, and rejection of unsupported preemption.
- Twenty-four request-bag permutations check priority grants. Ninety-six independent integer-time resource schedules compare every grant and capacity invariant across FIFO/LIFO/priority, varying request sizes, priorities, releases, and capacity.
- The station reference now contains 96 integer-time schedules: the original 64 FIFO/priority cases plus 32 LIFO cases, covering one/two servers and finite/unbounded waiting rooms.
- The declarative LIFO contract checks the complete hand trajectory, observation-grid invariance, priority independence, and hybrid completion pulses.
- Ninety-six frozen non-simultaneous queue workloads run in the native engine, pinned SimPy, and pinned Ciw. All **1,382 service records and 922 rejection records** match, including identities, entries, starts, and finishes within `2e-9` absolute/relative numerical tolerance. The offline contract rejects 13 corrupted evidence variants.

The independent implementations use SimPy's FIFO/PriorityResource scheduler and Ciw's [service disciplines](https://ciw.readthedocs.io/en/latest/Guides/Services/service_disciplines.html). LIFO uses negative arrival identity as SimPy's priority for these strictly increasing, one-node arrivals, and Ciw's built-in LIFO selector. Integer priority uses SimPy's priority resource and a Ciw selector over the entity's declared priority. Neither engine is modified. These trace cases intentionally avoid simultaneous arrivals and do not assert that other engines implement Fathom's input-bag arbitration conventions. Those conventions have separate exact native tests.

The [input plan](../tests/oracles/des/discipline-plan.json) fixes 24 jobs in each combination of three disciplines, one/two service slots, waiting capacities zero/one/three/unbounded, and four arrival/service patterns. These are deterministic event-trace gates, not stochastic stationary estimates. [Saved reference traces](../tests/oracles/des/discipline-reference.json) record dependency pins and hashes of the plan, generator, and shared engine adapters. Every native report includes its full input specification, which the comparison verifies before checking traces. Pinned generation reads no native code or output. Dedicated CI regeneration and normal/sanitizer offline comparisons are configured.

```sh
./build/fathom run models/lifo_process.ir.json
ctest --test-dir build --output-on-failure -R 'des_discipline|des_lifo|des_bounded_queue'
.venv-des-oracle/bin/python tests/oracles/des/discipline_oracles.py --verify
```

Current full-suite status is recorded in [STATUS.md](STATUS.md). General declarative resource nodes, expression-based priorities, standalone process queues, typed entity references, and the remaining process vocabulary are still open M3 work. This increment does not claim those features or general priority-network distributional validation.
