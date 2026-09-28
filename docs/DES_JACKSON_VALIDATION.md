# Three-station Jackson validation

The CPU DES network is checked on two open three-station tandem networks, with independent Poisson input and independent exponential service at each station. Both are unbounded, FIFO, single-server networks started empty. The bottleneck is downstream in one case and upstream in the other. This exercises a bounded part of the Jackson-network acceptance gate; it does not establish general branching, feedback, or closed-network correctness.

## Analytical targets

Every job visits stations 0, 1, and 2, then exits. External arrival rate is `λ = 0.5`; traffic balance gives that rate at each station. Service rates are `(1, 1.25, 0.8)` and `(0.8, 1.25, 1)`. For each station:

```
ρ = λ/μ
utilization = ρ
mean queue = ρ²/(1−ρ)
mean wait = ρ/(μ−λ)
throughput = λ
```

Network mean cycle time is `Σ 1/(μᵢ−λ) = 20/3`. The stationary joint occupancy distribution is `P(n₀,n₁,n₂) = ∏ (1−ρᵢ)ρᵢ^nᵢ`. These assumptions and the product-form result are given in [MIT's Burke/Jackson lecture, slides 9–11](https://ocw.mit.edu/courses/6-263j-data-communication-networks-fall-2002/42cb7759de031ae20aae04632773fef4_Lecture7.pdf). Reusing a source service duration at later stations violates the independent-service assumption, so these runs use [independent station service](DES_SERVICE.md).

Each station's occupancy is categorized as zero, one, or at least two jobs, including service. Their exact marginal masses are `(1−ρ, (1−ρ)ρ, ρ²)`. The Cartesian product produces 27 disjoint joint categories covering the entire state space. The test gates all 27 masses, four metrics for each station, and network cycle time: **40 metrics per case, 80 in total**. Categories aggregate the tail; this is not a validation of every individual high-occupancy state.

## Frozen plan and independent pilot

`tests/oracles/des/jackson_plan.py` generates exact rational targets, then chooses replication counts with an independent Python `Random` pilot. Its reference computes each job's stage completion from `finish[i,j] = max(finish[i−1,j], finish[i,j−1]) + service[i,j]`, without a DEVS engine. A sorted sweep of station-entry and departure times integrates joint occupancy. Separate generators supply arrivals and each station's service.

The plan was frozen before native execution. It uses 64 pilot replications per case, seed base 811001 with case offsets of 1,000, independent service seeds offset by 9,000,000 plus 100,000 per station, warm-up 200, and measurement window `(200,1200]`. Absolute tolerances are:

| Metric | Tolerance |
|---|---|
| Station utilization | 0.035 |
| Station throughput | 0.04 |
| Station mean queue | 0.04 + 20% of target |
| Station mean wait | 0.06 + 20% of target |
| Each joint occupancy category | 0.025 |
| Network cycle time | 0.15 + 15% of target |

For pilot standard deviation `s`, each metric requires `ceil(((4+1.645) × 1.25 × s / tolerance)²)` replications. Each case takes the maximum requirement, at least 16, rounded up to a multiple of eight. That gives **112 downstream-bottleneck and 104 upstream-bottleneck replications, 216 total**. This is conservative normal-approximation planning, not a finite-sample power guarantee or a bound on initial-transient bias. Native outcomes do not select seeds, counts, or tolerances.

Native Philox uses seed 2026092402, scenarios 30 and 31, replication indices from zero, entity IDs from zero, stream 200 for interarrival durations, and streams 201–203 for station services; step and draw index are zero. The frozen JSON plan records these addresses, targets, tolerances, and pilot statistics. Its regeneration is a CTest target using only the Python standard library.

## Accounting and measurement

Every transition checks inter-station flow conservation and `accepted = completed + waiting + busy`. FIFO completion IDs must match source order at every station. A separate event observer integrates queue, busy, and joint occupancy areas over the measurement window; station integrals must match it, and joint mass must sum to one.

Input stops at time 1200 and all accepted work drains. Stage wait averages follow jobs entering that stage within the measurement window; network cycle averages follow original arrivals in that window. Future arrivals cannot delay these FIFO cohorts, so draining avoids right-censoring their outcomes. Each job's entry and completion is then checked against the max-plus recurrence. Its clipped waiting and service intervals must reconstruct measured queue and busy areas; total individual waits must match each station's accumulated wait. Source service metadata must remain unchanged. Numerical comparisons use a `2e-9` absolute/relative allowance.

Acceptance compares equally weighted replication means to the frozen tolerances. Standard errors and approximate 95% normal intervals are computed across independent replication estimates and are diagnostics only. No gate assumes individual events within a replication are independent. This finite warm-up and fixed load set does not prove steady-state convergence at arbitrary loads.

## Reproduction

```sh
python3 tests/oracles/des/jackson_plan.py --verify
cmake --build build --target des_jackson_tests
ctest --test-dir build --output-on-failure -R '^des_jackson'
```

The native test writes `build/des-jackson-report.json`, including targets, tolerances, means, standard errors, diagnostic intervals, and pass/fail results. The sanitizer build writes its own report. The subsequent [pinned SimPy/Ciw comparisons](DES_ENGINE_ORACLES.md) now cover these tandem models and their joint occupancy distributions.

## Recorded results

All **80 analytical gates pass across 216 replications** in both Debug and ASan/UBSan. Their metric records match exactly on this machine. The [recorded native report](../tests/oracles/des/jackson-native-report.json) is build evidence, not a cross-platform bitwise requirement. Runtime was approximately 52 seconds normally and 215 seconds with sanitizers; timing is informational.

| Case | Station | Mean queue | Target | Mean wait | Target | Utilization | Target |
|---|---:|---:|---:|---:|---:|---:|---:|
| Downstream bottleneck | 0 | 0.483951 | 0.500000 | 0.963949 | 1.000000 | 0.495999 | 0.500000 |
| Downstream bottleneck | 1 | 0.268537 | 0.266667 | 0.535870 | 0.533333 | 0.401497 | 0.400000 |
| Downstream bottleneck | 2 | 1.050882 | 1.041667 | 2.092453 | 2.083333 | 0.625998 | 0.625000 |
| Upstream bottleneck | 0 | 1.059948 | 1.041667 | 2.120206 | 2.083333 | 0.625162 | 0.625000 |
| Upstream bottleneck | 1 | 0.268740 | 0.266667 | 0.538582 | 0.533333 | 0.398383 | 0.400000 |
| Upstream bottleneck | 2 | 0.479442 | 0.500000 | 0.961096 | 1.000000 | 0.494251 | 0.500000 |

Mean network cycle times are 6.641174 and 6.671279 against the exact target 6.666667. The largest error is 17.4% of its tolerance, in the upstream-bottleneck joint category `(≥2,0,0)`. Four approximate 95% intervals exclude their targets: downstream `p200` and `p220`, upstream `p200` and station-2 utilization. These are diagnostic intervals as predeclared; all fixed error gates pass. The largest observed replication SD is 98.3% of its inflated pilot estimate; the largest `(4+1.645) × observed SE / tolerance` is 0.792. These diagnostics support the planning approximation for these cases without establishing a general power guarantee.

At this checkpoint, the full Debug and ASan/UBSan suites each passed **85/85 tests**; current totals are in [status](STATUS.md). Boundary tests also verify that constructing a policy does not sample an unused entity and that downstream legacy service retains its original duration. Rebuilt service and Jackson targets pass in both builds with unchanged metric records. Schema/loader conformance passes **32 valid fixtures, 143 structural invalid cases, and 66 semantic invalid cases**. Current platform totals are in [status](STATUS.md). These are local runs; remote CI was not run in this session.
