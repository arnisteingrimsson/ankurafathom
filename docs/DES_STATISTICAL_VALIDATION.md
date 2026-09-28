# M/G/1 and finite-buffer validation

This M3 increment tests the CPU DES station against stationary analytical targets, with startup excluded from time averages and accepted arrivals followed through completion. It adds validation and repairs absolute-clock handling in the DES process components. It adds no production distributions, routing policies, or accelerator code. Subsequent [Jackson validation](DES_JACKSON_VALIDATION.md) and [pinned SimPy/Ciw comparisons](DES_ENGINE_ORACLES.md) provide the corresponding network and cross-engine evidence.

## Cases and analytical targets

All cases use FIFO, independent Poisson arrivals, mean service duration 1, and fixed server counts. Queue capacity means waiting places; the total finite system capacity is `K = servers + queue_capacity`.

| Case | Arrival rate | Servers | Waiting places | Service distribution | Replications |
|---|---:|---:|---:|---|---:|
| M/D/1 | 0.5 | 1 | Unbounded | Constant 1 | 24 |
| M/E2/1 | 0.5 | 1 | Unbounded | Sum of two independent rate-2 exponentials | 40 |
| Mixed M/G/1 | 0.5 | 1 | Unbounded | 0.25 with probability 0.8; 4 with probability 0.2 | 72 |
| M/M/1/1 | 0.7 | 1 | 0 | Rate-1 exponential | 48 |
| M/M/1/4, balanced | 1 | 1 | 3 | Rate-1 exponential | 56 |
| M/M/1/3, overloaded | 2 | 1 | 2 | Rate-1 exponential | 40 |
| M/M/2/2, loss system | 3 | 2 | 0 | Rate-1 exponential | 16 |
| M/M/2/5, overloaded | 2.4 | 2 | 3 | Rate-1 exponential | 40 |

For unbounded M/G/1, the Pollaczek–Khinchine target is `Wq = λ E[S²] / (2(1−ρ))`, with `ρ = λ E[S]`, `Lq = λ Wq`, utilization `ρ`, and throughput `λ`. The three service second moments are 1, 1.5, and 3.25, giving mean waits 0.5, 0.75, and 1.625. This exercises the effect of service variability while keeping mean work and utilization fixed. See [Stanford's M/G/1 lecture notes](https://web.stanford.edu/class/ee384x/EE384X/handouts/H10.pdf).

Finite targets use birth–death weights `w0 = 1`, `wn = w(n−1) λ / (min(n,c) μ)` for `n = 1..K`, normalized to probabilities `pn`. Blocking probability is `pK`; admitted throughput is `λ(1−pK)`. Mean queue length is `Σ max(n−c,0) pn`, utilization is `Σ min(n,c) pn / c`, and admitted mean wait is queue length divided by admitted throughput. The recurrence handles balanced load without a singular geometric-series formula. See [MIT's finite-storage and loss-system lectures](https://www.mit.edu/~medard/6.02s/6.02sday2.pdf) and [Whitt's birth–death analysis](https://www.columbia.edu/~ww2040/FitBD021112.pdf).

The plan generator calculates targets using exact Python fractions and checks rate balance exactly. Balanced M/M/1/4 has `pn = 1/5`, utilization and throughput 0.8, queue length 1.2, and admitted mean wait 1.5. Finite systems remain well-defined above unit offered utilization because they reject excess arrivals.

## Measurement contract

Each replication starts empty, warms up for 200 time units, measures over `(200,1200]`, then stops arrivals and drains every admitted job. Time-weighted statistics use only the 1,000-unit measurement interval. Waiting time is averaged over accepted arrivals in that interval, including jobs still waiting or serving at the cutoff. Draining does not bias these FIFO waits: later arrivals cannot delay an already admitted job.

Departure throughput counts completions during the window. Rejection probability uses rejected/offered arrivals in the window, never rejected/accepted. Every finite case also measures the complete occupancy distribution by integrating time in each state. No regularly sampled grid approximates occupancy or utilization.

Independent consistency checks run within each replication:

- Offered = accepted + rejected, and accepted = completed + waiting + busy, after every event.
- Waiting and busy counts stay within configured limits; completion and rejection IDs never duplicate or overlap.
- Rejection publications have drained at both measurement boundaries. All finite cases enable the rejection port, exercising its zero-time events under random load.
- Independently integrated queue and busy areas agree with the station's cumulative statistics after subtracting warm-up contributions.
- After the final drain, all accepted jobs have completed. The measured arrival cohort has exactly the accepted count from the observation window.
- Individual waiting and service intervals, clipped to the measurement window, reconstruct both time integrals. Total job waiting time agrees with the station accumulator.
- Unbounded single-server completions agree with an independent absolute-time Lindley recursion. Finite occupancy areas sum to the window length.

Floating identities use a `2e-9` absolute/relative comparison allowance. Zero-wait/zero-queue and unbounded zero-rejection targets use only a `2e-9` numerical allowance, with no statistical tolerance.

## Clock regression found by the workload

The first native mixed-service run failed in replication 33 at time `6.4067953695328468`, before statistical acceptance was evaluated. A minimized prefix has arrivals at `1.8319702089718573`, `2.0199542415267091`, and `6.4067953695328468`, each with service duration 0.25. After the second completion, reconstructing the third arrival as `last_clock + elapsed` rounds down by one floating-point step, causing strict arrival validation to reject a valid entity.

Scheduled sources now expose their exact absolute deadlines. Single- and multi-server stations store fixed absolute completion deadlines and use the kernel's timestamp-aware external-transition callback. Routers, discard sinks, and completion sinks also preserve the supplied event clock. External arrivals no longer repeatedly subtract remaining service time, and unrelated events cannot move the completion deadline. Positive service durations whose completion cannot be represented at the current clock are rejected transactionally. Direct elapsed-time calls remain supported with representability checks.

`des_clock` covers the three-arrival failure through a router into each server implementation, exact source/completion publication timestamps, unchanged deadlines across intervening external events, inconsistent-clock rejection, and rollback on unrepresentable service time. The original seed set and statistical tolerances were retained after this fix.

## Frozen statistical plan

`tests/oracles/des/queue-plan.json` fixes the eight cases, seed, targets, tolerances, and replication counts before the first native statistical run. `queue_plan.py` reconstructs it with standard-library Python; it does not call Fathom or import its code.

The planning pilot uses 64 replications per case with Python `Random`, separate arrival/service generators, an absolute-deadline heap, and a FIFO deque. Its seed base is 701001, with case offsets of 1,000. Native tests instead use Philox seed 2026092401, scenario IDs 20–27, replication IDs from zero, and entity-addressed streams 100 for arrivals, 101 for first service draw/mixture choice, and 102 for the second Erlang phase. Native outcomes do not select seeds or sample sizes.

Fixed absolute acceptance tolerances are:

| Metric | Tolerance |
|---|---|
| Utilization | 0.035 |
| Throughput | 0.015 + 5% of target |
| Mean queue length, nonzero target | 0.04 + 20% of target |
| Mean admitted wait, nonzero target | 0.06 + 20% of target |
| Blocking probability, finite system | 0.02 |
| Each finite occupancy probability | 0.035 |

For each metric, pilot standard deviation `s` gives the planning requirement `n = ceil(((4 + 1.645) × 1.25 × s / tolerance)²)`. A case uses the largest requirement, at least 16 replications, rounded upward to a multiple of eight. This yields 336 native replications. The 25% variance inflation, four-standard-error tail allowance, and additional 1.645 power margin are conservative normal-approximation planning choices: they budget sampling error within the tolerance and support detecting a bias of twice the tolerance with at least approximately 95% power under that approximation. They are not a finite-sample theorem or a bound on startup bias.

The acceptance gate compares the mean of independent replication estimates with the fixed analytical target. Standard errors and approximate 95% normal intervals across replication estimates are diagnostics; intervals are not independent-event intervals and are not required to contain the target. Estimates are equally weighted by replication, including within-run blocking and cohort waiting ratios. Autocorrelation within a replication is reflected in pilot and reported between-replication variance. Stationary claims still depend on adequate warm-up; tests do not establish all-load convergence, extreme-tail behavior, or arbitrary service-law correctness.

## Reproduction and evidence

```sh
python3 tests/oracles/des/queue_plan.py --verify
cmake --build build --target des_statistical_tests
ctest --test-dir build --output-on-failure -R '^des_statistical'
```

The C++ test writes `build/des-statistical-report.json` (or the corresponding sanitizer build path), including each mean, target, tolerance, standard error, approximate 95% interval, replication count, and pass/fail result. Plan verification is a separate CTest target and runs in existing CI without additional Python dependencies. The pilot is a planning/reference implementation; the separate [pinned-engine gate](DES_ENGINE_ORACLES.md) now supplies SimPy/Ciw comparisons for these workloads.

## Recorded native results

The checked native run passes all 60 analytical metric gates across 336 replications. The largest nonzero-gate error is 32.1% of its permitted tolerance (blocking in balanced M/M/1/4). Three approximate 95% intervals exclude their targets (balanced-case blocking, full-state probability, and mean wait); these remain diagnostic as predeclared, and all three means pass their fixed error gates. The largest observed replication standard deviation was 12.3% above its inflated pilot estimate, reinforcing that pilot-based power planning is approximate. Every observed standard error nevertheless fits the planned `(4 + 1.645) × SE` tolerance budget; the largest budget fraction is 0.910. The recorded [native report](../tests/oracles/des/native-report.json) includes all occupancy probabilities and diagnostic intervals; it is evidence from this build, not a requirement for bitwise equality on other platforms.

| Case | Measured mean wait | Target wait | Measured blocking | Target blocking |
|---|---:|---:|---:|---:|
| md1 | 0.504200 | 0.500000 | 0.000000 | 0.000000 |
| me2_1 | 0.716600 | 0.750000 | 0.000000 | 0.000000 |
| mixed_mg1 | 1.643493 | 1.625000 | 0.000000 | 0.000000 |
| mm1_k1 | 0.000000 | 0.000000 | 0.414327 | 0.411765 |
| mm1_k4_balanced | 1.529317 | 1.500000 | 0.206419 | 0.200000 |
| mm1_k3_overloaded | 1.425408 | 1.428571 | 0.531443 | 0.533333 |
| mm2_k2_loss | 0.000000 | 0.000000 | 0.530015 | 0.529412 |
| mm2_k5_overloaded | 0.793552 | 0.800738 | 0.261346 | 0.263875 |

At this checkpoint, full normal and ASan/UBSan builds each passed **81/81 CTest tests**; current totals are in [status](STATUS.md). All 60 metric records in their statistical reports match exactly on this machine. The statistical test took approximately 195 seconds normally and 821 seconds with sanitizers; runtime is recorded, not gated. Checkpoint schema/loader conformance was 30 valid fixtures, 122 structural invalid cases, and 60 semantic invalid cases. Remote CI was not run in this session.

The subsequent [Jackson increment](DES_JACKSON_VALIDATION.md) gives each station independent service draws. Reusing an entity's source duration, even with per-station scaling, correlates tandem service times and does not satisfy the Jackson product-form assumptions.
