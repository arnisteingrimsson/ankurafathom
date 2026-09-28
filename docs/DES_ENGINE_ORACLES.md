# Pinned SimPy and Ciw comparisons

Fathom's eight [single-station workloads](DES_STATISTICAL_VALIDATION.md), two [three-station tandem networks](DES_JACKSON_VALIDATION.md), and two [probabilistic branching networks](DES_PROBABILITY.md) now have independent SimPy/Ciw models. The comparison covers 222 metrics per engine: mean queue, mean waiting time, utilization, throughput, blocking, network cycle time, 20 finite occupancy probabilities, and 108 joint occupancy categories. All **444 native/reference gates pass**, including the two routing fractions. The newest increment adds binary probabilistic routing to the production simulator.

## Models and independence

The pinned engines are **SimPy 4.1.2** and **Ciw 3.2.7**. Their transitive dependencies are pinned in [requirements.txt](../tests/oracles/des/requirements.txt). Generation requires Python 3.12 or later (excluding 3.14.1, as required by the pinned NetworkX release); routine CTest comparisons use only the Python standard library.

The generator supplies pre-sampled arrival times and per-entity, per-station service durations to both engines. SimPy uses its own [process scheduler and FIFO resources](https://simpy.readthedocs.io/en/latest/api_reference/simpy.resources.html). A small model-level admission check implements external loss because `Resource` has an unbounded request queue. Ciw uses its own network scheduler, servers, queues, and routing, with [custom distributions](https://ciw.readthedocs.io/en/latest/Guides/Distributions/set_distributions.html) returning the pre-sampled inputs. Engine code is unmodified.

Ciw's [capacity semantics](https://ciw.readthedocs.io/en/latest/Guides/Queues/queue_capacities.html) distinguish external rejection from blocking after service at a downstream node. These oracles use finite capacity only in a one-node model and use unbounded tandem and branching stations. They do not claim equivalence between Ciw downstream blocking and Fathom rejection/rerouting.

The reference input generator uses Python `Random` with case seed bases `921001 + 1000 × case_index`, then adds the replication index. Independent service generators add 9,000,000 plus 100,000 per station. Branch routing uses a separate generator offset by 8,000,000; Ciw uses its built-in `ProcessBased` routing with the pre-sampled path. The service laws are constant, Erlang-2, two-point mixture, or exponential, matching the original plans. These seeds and RNG differ from both native Philox and the earlier planning pilots. No native code, output, or event trace is read during reference generation.

SimPy and Ciw share these sampled inputs with each other. Their service/rejection outcomes, job IDs, node IDs, entries, starts, and finishes are compared before aggregation. They are two engine implementations of the same reference sample, not independent Monte Carlo samples to be pooled. Native/reference comparisons use independent samples. Matching records do not establish arbitrary simultaneous-event semantics; native deterministic confluence tests remain the oracle for that contract.

## Measurements and gates

Each engine runs **672 replications**: 336 single-station, 216 tandem, and 120 branching. Warm-up, measurement window, and replication counts come from the unchanged original plans. Input stops at 1200; every admitted job drains. Wait cohorts enter the measured station in `(200,1200]`; cycle cohorts enter the network in that window. This avoids censoring jobs that complete later. Rejected jobs contribute to offered/lost counts and never to service statistics.

Completed histories reconstruct clipped queue and busy intervals. A separate sweep of station entries and exits produces occupancy distributions and independently checks those integrals. Every job must complete every required stage or have exactly one external rejection; capacities, nonnegative occupancy, metadata, service durations, and probability normalization are checked. Hand schedules test finite FIFO, two-server loss, tandem and branching service, excluded warm-up boundaries, and uncensored waits against explicit times and totals.

[engine-plan.json](../tests/oracles/des/engine-plan.json) was frozen before the first cross-engine comparison. It records original plan hashes, dependency pins, seeds, counts, and metric gates. Each reference estimate must pass the original analytical tolerance. A native/reference difference must satisfy `abs(native_mean − reference_mean) <= sqrt(2) × original_tolerance`. Exact-zero cases retain the numerical allowance of `2e-9`.

The `sqrt(2)` factor accounts for two independent estimates at the same sample size under the equal-variance planning approximation. It carries forward the earlier pilot-based variance budget; it is not a new power calculation based on observed discrepancies. Fixed counts, seed sets, and thresholds were retained after seeing results. Approximate 95% intervals for differences use the root-sum-square of the native and reference replication standard errors and are diagnostic only.

For the five finite-state queue cases, the report also gives the maximum difference between occupancy CDFs (a discrete KS-type distance). All finite and joint distributions include total-variation distances. These are diagnostics; no event-independent p-value or single-run KS acceptance gate is used. The gated distributions are time-weighted occupancies, not waiting-time quantiles, extremes, or service tails.

## Offline checks and regeneration

The saved [reference summaries](../tests/oracles/des/engine-reference.json) include both engines' estimates, standard errors, intervals, replication counts, package versions, plan/generator hashes, and matched record counts. Offline comparison checks complete case/metric sets, metadata, counts, finite statistics, interval consistency, normalized probabilities, and unchanged targets/tolerances. The contract test rejects 24 corruptions and a deliberately constructed pair of opposing biases that each pass their individual analytical gate but fail the pair gate.

```sh
# Routine offline check; CTest fixtures run the native statistical tests first.
ctest --test-dir build --output-on-failure -R '^des_engine_'

# Reproduce both independent engines in an isolated Python environment.
python3 -m venv .venv-des-oracle
.venv-des-oracle/bin/python -m pip install -r tests/oracles/des/requirements.txt
.venv-des-oracle/bin/python tests/oracles/des/engine_plan.py --verify
.venv-des-oracle/bin/python tests/oracles/des/adapter_tests.py
.venv-des-oracle/bin/python tests/oracles/des/engine_oracles.py --verify
```

Without `--verify`, the generator writes the reference artifact only after every engine, trace, and analytical check passes. Verification requires identical structure, metadata, and counts, with a `1e-10` relative / `1e-12` absolute allowance for floating-point regeneration. It does not update the artifact. A separate CI job installs the pinned engines and verifies regeneration; normal and sanitizer CTest jobs compare fresh native reports to the stored reference without installing the engines.

## Recorded evidence and remaining scope

The two references agree on **876,921 service records and 141,345 rejection records**, with matching timestamps within the `2e-9` absolute/relative numerical allowance. Their 444 analytical gates pass, and native/reference comparisons pass all **444 pair gates**, including **256 occupancy-probability comparisons**. The largest pair error uses **27.2%** of its tolerance (upstream-bottleneck station-0 mean wait); the largest reference analytical error uses 30.7% of its tolerance. The largest finite occupancy CDF distance is 0.007161, and the largest joint total-variation distance is 0.013993.

The routing increment passes all **91/91 tests in both normal and ASan/UBSan builds**, with identical statistical records. Adapter hand tests and reference generation pass locally. The original ten reference cases remain exactly unchanged after extending the generator.

The [recorded comparison report](../tests/oracles/des/engine-comparison-report.json) contains all values and diagnostics. The live report is `build/des-engine-comparison.json`. Current suite totals and local verification scope are in [status](STATUS.md); remote CI has not been run in this session.

This completes the pinned-engine comparison gate for these twelve workloads. M3 remains open for general/multiway routing, repeated visits, broader resource-pool disciplines, and the rest of the planned process vocabulary. Priorities, finite downstream rejection routes, hybrid feedback, and new service laws require their own cross-engine cases before extending this claim.
