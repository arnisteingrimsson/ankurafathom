# Typed workforce pools

`hybrid/typed_agent_pool.hpp` adds one authoritative typed population, integer
resource pool, assignment ledger and bounded broker. `typed_agent_pool_atomic.hpp`
connects this owner to typed DES seize/release and the population aggregate/SD path.
It complements the existing bounded declarative `agent_pool` models; it does not
silently broaden their schema.

The capacity projection reads each live agent's record. A reserved integer field
records allocated units and is written only by the owner. Grants take available
agents in ascending stable-ID order and may span agents. Capacity projections must
be pure and independent of allocations. Changes to capacity caused by allocation
writeback reject. Total capacity uses checked size_t arithmetic; each agent's
contribution fits int64.

`transact(Change)` applies releases, unassignment notifications, full-record
updates, retirements, births and optional registered Jacobi phases, then resizes
the resource pool and grants demand. Allocation fields are updated before the
assignment broker batch. Explicit updates contain the desired **post-release
record**, including any other fields changed by unassignment handlers; they are
full replacements, not patches. An update must preserve the post-release allocation
field. Birth records start with zero allocation. Busy retirement or capacity
reduction below assigned shares rejects, unless the relevant requests are released
in that transaction. Phase registration freezes after the first successful change.

Requests and releases within a transaction are sorted by request ID. FIFO, LIFO
and stable-priority queue behavior then follows the existing resource pool;
head-of-line blocking is preserved. Lifetime duplicate requests and preemption
reject. Assignment and unassignment notifications are canonical by request and
agent ID. Optional handlers may change other fields and see earlier handler
updates. They cannot change allocation or any agent's projected capacity. Bounded
topic overflow, schema failure and callback failure roll back population, IDs,
resource queue, grants, assignments, broker state, accounting and time together.
Callbacks must own their captures and have no external side effects.

The atomic accepts `Seize`/`Release` on port 0 and one `TypedPoolControl` on port 2.
A workforce control carries a timestamped `Change` and a strictly increasing
revision from a single producer. Resource inputs merge into that transaction.
The atomic has no automatic behavior clock; controls explicitly request phases.
A committed result publishes in a following zero-time step:

| Output | Port |
|---|---|
| Routed grant | `1 + (request_id >> 48)` |
| Complete result and immutable population snapshot | 65537 |
| `PopulationResult` for `PopulationResultPublisher` | 65538 |
| Individual broker notifications | 65539 |

A failed downstream publication preserves the pending committed result. Checked
retry republishes it without assigning resources or running handlers twice.
Capacity-hours, allocated-unit-hours, waiting-request-hours and live-agent-hours
use preceding committed levels; read-only horizon statistics do not advance the
model. Event confluence and simultaneous workforce/resource updates have explicit
transaction boundaries. Separate DEVS microsteps at one timestamp are separate
transactions.

The independent Python scheduler uses integer queues, assignments and rational
area integration. Its frozen cases cover all three disciplines, disabled/empty
workforces, hires, departures, capacity phases, queued demand, split grants,
notifications and agent IDs near 2^48. Across 24 graph orders and reversed input
bags, **432 configurations, 21,168 snapshots and 1,489,920 scalar comparisons agree
exactly**. Population snapshots feed reducers and three SD stocks, whose integrated
capacity, allocation and headcount agree with independent rectangle accounting.
Sixteen deliberate report corruptions reject. Normal and ASan/UBSan artifacts have
identical bytes; trajectory SHA-256:
`a64dc156c694875d185d993cea806dce6c0175b641630ee9bed8d6fc772c1ef5`.

Native hand checks add all 24 staffing-process orders, static ResourcePool trace
equality, Jacobi phases, full size_t capacity, topic/handler rollback, identity
exhaustion, reserved-field/capacity guards, control replay and publication retry.

```sh
cmake --build build --target hybrid_typed_pool_tests hybrid_typed_pool_oracle_tests fathom_typed_agent_pool
ctest --test-dir build -R '^hybrid_typed_pool' --output-on-failure
./build/fathom_typed_agent_pool
```

This contract has one pool and allocation field per population, integer units,
explicit phase controls and no preemption. General declarative hybrid graphs,
automatic population clocks and multi-pool staffing remain separate extensions.
