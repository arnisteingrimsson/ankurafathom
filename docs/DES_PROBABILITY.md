# Addressed probability routing and branching-network validation

Standalone DES routers now accept either an integer priority threshold or a binary probability rule:

```json
{"id":"choice", "kind":"router", "probability":{"match":0.35, "stream":301}}
```

Both `match` and `otherwise` exits remain required, including at probability zero or one. The probability must be finite and in `[0,1]`. The stream is an unsigned 16-bit value distinct from source, station-service, and other router streams. Sampled entities have 48-bit IDs. The router addresses Philox by seed, scenario, replication, entity ID, step zero, stream, and draw zero; word zero selects `match` when its uniform value is strictly less than the probability. No mutable draw cursor is used.

Choices commit at admission and publish at the same physical time in the next DEVS microstep. Reading outputs, cloning, observation frequency, and declarations do not redraw them. Invalid bags and failed checked steps roll back both decisions and publications. The existing acyclic topology, metadata preservation, confluence, and accounting rules apply; see [semantics](SEMANTICS.md#binary-probabilistic-routing) and [routing](DES_ROUTING.md). Repeated visits, mutable probabilities, multiway expressions, and hybrid graphs remain outside this increment.

## Exact checks

`des_probability` checks eight input permutations against explicit Philox addresses, both endpoints, address overflow, malformed probabilities, confluence, immutable output/clone state, variant payload errors, and downstream checked-step rollback/retry. The declarative fixture is checked at every observation against a separate FIFO completion recurrence for two replication IDs.

`des_probability_contract` adds 32 declaration permutations, three alternative observation grids, endpoint conservation, deterministic replay of eight scenario/replication combinations, distinct outcomes across addresses, maximal valid addresses, and malformed-rule/stream diagnostics. Schema conformance adds malformed probability shapes, ranges, missing fields, two simultaneous rules, source/service stream overlap, required exits, and oversized entity IDs even at probabilities zero or one.

## Frozen analytical plan

Two open three-node Jackson networks use Poisson arrival rate `λ=0.6`, station rates `(1,0.8,1.2)`, and a split after station zero. Each job visits exactly one downstream branch. Split probabilities are `p=0.35` and `p=0.65`; traffic balance gives node rates `(λ, λp, λ(1−p))`. Independent exponential services and independent Markov routing satisfy the assumptions for the [Jackson product-form solution](https://ocw.mit.edu/courses/6-263j-data-communication-networks-fall-2002/42cb7759de031ae20aae04632773fef4_Lecture7.pdf).

For node arrival rate `a` and service rate `μ`, targets are utilization `ρ=a/μ`, mean queue `ρ²/(1−ρ)`, mean wait `ρ/(μ−a)`, and throughput `a`. Network cycle time is

```
1/(μ₀−λ) + p/(μ₁−λp) + (1−p)/(μ₂−λ(1−p)).
```

The joint occupancy categories zero, one, and at least two jobs use each node's exact masses `(1−ρ, (1−ρ)ρ, ρ²)`. Their product gives 27 exhaustive categories. Four metrics per node, 27 joint masses, cycle time, and measured match fraction give **41 gates per case, 82 total**.

[branch_plan.py](../tests/oracles/des/branch_plan.py) computes rational targets and runs an independent max-plus pilot: 64 replications per case, Python `Random` seed base 1031001 plus 1000 per case and the replication index. Separate routing and service streams use offsets 8,000,000 and 9,000,000, with 100,000 per station. Warm-up is 200 and the measurement window is `(200,1200]`. Input then stops and all jobs drain.

Tolerances are 0.025 for each joint mass and match fraction; 0.035 for utilization; `0.015+0.05×target` for throughput; `0.04+0.20×target` for queue; `0.06+0.20×target` for wait; and `0.15+0.15×target` for cycle time. The pre-existing conservative planning formula `ceil(((4+1.645)×1.25×pilot_SD/tolerance)²)` selects each metric's count. The case maximum, at least 16 and rounded up to eight, gives **56 and 64 replications**. These seeds, counts, and gates were frozen before native runs and retained after observing results. This is approximate variance-based planning, not a general power or transient-bias guarantee.

Native seed is 2026092501; scenarios are 40 and 41. Entity IDs start at zero. Arrival stream is 400, service streams 401–403, and routing stream 404; step/draw indices are zero. The [frozen plan](../tests/oracles/des/branch-plan.json) records all targets, tolerances, and pilot statistics.

## Flow and measurement checks

Every native event checks station conservation and router buffer accounting. Branch completion identities must match their addressed route and preserve FIFO order among visiting jobs. A separate max-plus recurrence checks every entry and completion. Queue/busy areas from event observations, native accumulated statistics, and clipped per-job intervals must agree. Joint masses sum to one. Drained cohorts include every source arrival exactly once at a leaf; station wait cohorts use station entry, while cycle cohorts use original network arrival. Route fractions use station-zero departures in the measurement window.

The pinned SimPy/Ciw extension pre-samples independent reference routes and durations; SimPy visits the chosen resource sequence, and Ciw uses its unmodified `ProcessBased` routing class. Hand schedules verify a branching workload with unused service draws, out-of-source-order leaf completion, and uncensored waits. Their per-job traces agree before summary comparison. See [independent-engine evidence](DES_ENGINE_ORACLES.md).

All 82 native analytical gates and 164 additional native/reference pair gates pass. Full-suite verification is recorded in [status](STATUS.md). The saved [native report](../tests/oracles/des/branch-native-report.json) includes means, standard errors, diagnostic 95% intervals, and unchanged gates. Normal intervals are diagnostic; acceptance never requires every interval to contain truth.

```sh
./build/fathom run models/probability_process.ir.json
ctest --test-dir build --output-on-failure -R 'des_probability|des_branch'
```
