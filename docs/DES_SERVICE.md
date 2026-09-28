# Independent station service

`MultiServer` can draw an independent exponential service duration for each entity at each station. This is available in standalone DES graphs and the existing linear DES/SD hybrid subset. Omitted service policies preserve the original source-duration behavior.

## Contract

```json
{
  "id": "review", "kind": "server", "capacity": 1,
  "service": {"kind": "exponential", "rate": 1.25, "stream": 201}
}
```

`rate` is finite and positive, in reciprocal model-time units. `stream` is an explicit unsigned 16-bit address. An optional positive `service_scale` multiplies the drawn duration, so the mean service time is `service_scale / rate`. Without `service`, the duration remains `Entity::service_duration × service_scale`.

The C++ API appends an optional policy to the existing constructor:

```cpp
des::ExponentialService service{1.25, seed, scenario, replication, 201};
des::MultiServer<> station(1, 1, {}, false, service);
```

The policy addresses Philox with `(seed, scenario, replication, entity ID, step=0, stream, draw_index=0)` and uses word zero through the existing inverse-CDF exponential transform. Scenario and replication are each 16 bits; entity IDs are 48 bits. The address has no service-slot, admission-order, or queue-position field. Validating an arrival and starting it therefore produce the same draw; no mutable RNG cursor is consumed. Rejected work, cloning, and retried transitions cannot move another job's service draw. As before, re-entry or reuse of an entity ID at a station is rejected; this policy does not define repeated visits.

The source's positive base service duration remains mandatory metadata and is never overwritten. A later legacy station still uses that original duration. `entered_at` and `completed_at` describe the current/completed station; the message does not carry a new accumulated service history. Service deadlines and time-integrated statistics use the sampled duration internally. Invalid or unrepresentable service times fail transactionally.

The loader reserves separate streams for each sampled station, both streams of an exponential source, and both streams of a hybrid rate source. It rejects collisions even when entity-ID ranges are disjoint. Declaration order cannot allocate or change streams. Fixed schedules that reach a model with sampled station service must use 48-bit IDs. Purely legacy fixed schedules retain their existing unsigned-ID contract. Direct C++ composition requires callers to assign distinct station/source streams; a station cannot inspect its neighbors.

## Evidence

`des_service` checks exact addressed draws, service scaling, simultaneous-arrival reordering, preserved source metadata, clone/confluent retry, rejected-job isolation, invalid rates/address widths, maximum addresses, changed RNG context fields, and unrepresentable-deadline rollback. It also compares every observation in standalone and hybrid fixtures against a separate three-stage max-plus recurrence under two replications and a nonzero seed/scenario. Hybrid stock totals must equal completed jobs.

`des_service_contract` checks 32 component/link permutations, changed source-duration metadata, source/station/rate-source stream conflicts, ID boundaries, and a valid hybrid rate source with independent station service. Independent JSON Schema and loader conformance checks cover malformed policies and semantic stream collisions. Existing queue, routing, clock, and hybrid tests retain the default-path regression coverage.

```sh
./build/fathom run models/independent_service_process.ir.json
./build/fathom run models/hybrid_independent_service.ir.json
ctest --test-dir build --output-on-failure -R '^des_service'
```

Only exponential station policies are implemented here. Other service distributions, visit-addressed redraws, and probabilistic routing remain separate work. [Jackson validation](DES_JACKSON_VALIDATION.md) uses this policy on three independent stations.
