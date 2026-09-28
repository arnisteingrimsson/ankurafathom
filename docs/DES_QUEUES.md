# Finite queues and deterministic priorities

The CPU process station supports finite waiting capacity and non-preemptive integer priority in standalone DES and the existing DES/SD hybrid subset. The default remains unbounded FIFO. This is an M3 increment; it does not complete the DES milestone.

## Contract

C++ uses `MultiServer(capacity, service_scale, QueueOptions{queue_capacity, discipline})`. `queue_capacity` is optional; `nullopt` means unbounded. `QueueDiscipline` is `fifo`, `lifo`, or `priority`. Use `MultiServer(1, ...)` for a bounded single-server station; the original `SingleServer` reference primitive remains FIFO and unbounded.

Declarative server fields are `queue_capacity` (integer 0–1,000,000, omitted for unbounded) and `discipline` (`fifo`, `lifo`, or `priority`, omitted for FIFO). A scheduled entity may have `priority`, a signed 32-bit integer defaulting to zero. Exponential and hybrid-generated entities retain priority zero. Priority travels with the entity through every station. No expression evaluation, aging, or dynamic priority changes are included.

- Capacity counts waiting places, excluding jobs in service. A zero-capacity queue admits work only when service slots are available.
- At confluence, all due service completions release their slots first. Admission then reserves space for already accepted waiting work and admits as many new entities as the remaining service-plus-waiting capacity allows. Dispatch follows admission. New work never evicts accepted work.
- FIFO admission and dispatch preserve the kernel's stable input-bag order and place existing waiting work first.
- Priority admission ranks a simultaneous input bag by ascending priority, then ascending entity ID. Lower numbers are more urgent. Dispatch selects the lowest priority among all accepted waiting work. Equal priorities preserve admission order across transitions; new ties in one bag use entity ID. An urgent new arrival may overtake lower-priority waiting work if admitted.
- Service is never interrupted. Idle service slots are filled in slot-index order. Simultaneous completions still emit in slot-index order.
- Priority arbitration covers one DEVS input bag. Later zero-time microsteps at the same physical timestamp are later admissions; there is no future-event lookahead or eviction to combine them into one global batch.
- Every arrival is validated before any admission or rejection is committed. Malformed ports, times, durations, or duplicate IDs are errors even when the queue is full. Internal, external, and confluent transitions commit through a candidate copy; failed validation or statistics overflow restores the previous station state.

## Rejection and accounting

Without a rejection route, overflow is a terminal loss at that station. It produces no completion and no downstream event or SD pulse. The [routing extension](DES_ROUTING.md) optionally publishes rejected entities to a backup station, router, or discard sink in standalone DES. `rejected_count()` and the declarative `rejected` metric report cumulative station rejection attempts. Without a rejection route these are terminal losses. C++ `rejected()` records each original entity and its station-local rejection timestamp, preserving an upstream completion timestamp when present. Rejected IDs remain in the station's seen-ID set; retrying an ID is an error. Standalone acyclic rerouting and rejection ports are available under the graph contract; retry loops, backpressure, and preemption remain future work.

After each successful transition:

```
unique arrivals seen = accepted + rejected
accepted = completed + waiting + busy
waiting <= queue_capacity                 (when bounded)
busy <= service capacity
```

Queue area, service occupancy, and waiting time include accepted work only. Queue waiting time starts at entry to each station; sink cycle time starts at original source arrival. Observations include every event through the sample timestamp. Changing the observation grid does not change the standalone process. Rejection records and seen IDs are retained for the full run; storage optimization is deferred.

## Evidence

`des_bounded_queue` tests:

- A hand schedule with two waiting places: 7 arrivals, 5 accepted/completed, 2 rejected; completion IDs `4,5,6,1,2` at times `1,2,3,6,8`; queue area and total waiting time 9, busy area 8, sink cycle total 17.
- All 24 permutations of its initial simultaneous bag produce the same priority result.
- 64 independent integer-clock traces, each checked at 61 timestamps, cover one/two servers, FIFO/priority, zero/one/two/three waiting places, and unbounded controls. The reference uses absolute completion deadlines and one-unit area integration without the event kernel. Comparisons cover completion IDs, rejection IDs, occupancy, counts, conservation, wait totals, and queue/busy integrals.
- Equal-priority stability across transitions, non-preemption, no eviction, full-queue duplicate replay, invalid-bag rollback, and zero-buffer simultaneous completions.
- A checked two-stage process confirms priority propagation, downstream terminal loss, original entity metadata, and same-time completion/admission.
- Statistics overflow rolls back an internal transition; maximal C++ queue capacity does not overflow admission arithmetic.
- Declarative observations match the hand totals and remain identical on a denser sample grid. The hybrid process matches standalone metrics and adds SD pulses only for completed work.

The finite-queue checkpoint passed 76/76 Debug and ASan/UBSan tests, with 29 valid schema fixtures, 111 structural invalid cases, and 55 semantic invalid cases. Current platform totals are in [status](STATUS.md).

The existing single-server equivalence, process schedules, hybrid regressions, and seeded M/M/1 and M/M/2 oracles remain applicable to the unchanged defaults. Schema/loader conformance includes malformed capacities, disciplines, and priority values.

Run the example:

```sh
./build/fathom run models/bounded_priority_process.ir.json
ctest --test-dir build --output-on-failure -R 'des_bounded_queue|ir_bounded_priority_cli'
```

The subsequent [graph increment](DES_ROUTING.md) adds bounded acyclic branching and rejection routing. Remaining M3 work includes broader routing and resource-pool disciplines, broader network topologies, and distribution comparisons against pinned independent engines. The subsequent [statistical validation](DES_STATISTICAL_VALIDATION.md) checks stationary finite-buffer loss probabilities and M/G/1 means. The complete planned process vocabulary remains open.

The subsequent [independent-service increment](DES_SERVICE.md) adds station-local exponential policies and [three-station Jackson evidence](DES_JACKSON_VALIDATION.md). Queue admission, priority, and accounting rules are unchanged.

[Pinned SimPy/Ciw references](DES_ENGINE_ORACLES.md) now validate the five finite-buffer statistical configurations. They do not yet cover priority or finite downstream rerouting.

[LIFO and resource arbitration](DES_DISCIPLINES.md) now add LIFO station dispatch and priority/LIFO C++ resource grants, with 96 native/SimPy/Ciw exact queue traces.
