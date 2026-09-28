# Boids flocking

`abm::models::Boids` is a synchronous native reference workload using typed population
phases and continuous spatial snapshots. Its explicit steering and boundary rules
are the third canonical model contract for implementation-plan §7.4.

```sh
cmake --build build --target fathom_boids
./build/fathom_boids > build/boids-example.csv
```

The example emits 500 observations of 20 agents at ticks 0 through 24 on a 10×8
periodic domain. Records contain x, y, vx and vy. Population count, canonical
positions, finite values and the speed cap hold at every snapshot. The header is
[`include/ankurafathom/abm/models/boids.hpp`](../include/ankurafathom/abm/models/boids.hpp).

## Model contract

Every tick reads one shared population snapshot and one value-owned query index.
The index is rebuilt for each tick, so copies and retries cannot reuse stale geometry.
Neighbors are within an inclusive
vision radius, excluding self but including colocated peers. In stable ID order,
the model combines three weighted steering terms:

- Alignment: mean neighbor velocity minus own velocity.
- Cohesion: mean displacement to neighbors, using minimum images on a torus.
- Separation: mean negative displacement divided by distance² + softening² for
  neighbors within the separation radius. Colocated peers contribute zero.

The combined acceleration is capped. Velocity advances by dt × acceleration and
is capped again; position advances using the new velocity. This is semi-implicit
Euler. Empty neighbor sets contribute zero steering. There is no minimum-speed or
random-heading rule. Positive softening avoids the separation singularity.

Periodic coordinates wrap into a half-open box. An exact half-box displacement
retains its original sign. Reflecting boundaries handle multiple crossings per
tick by folding modulo twice the box length and reversing the relevant velocity
component. Exact wall hits point inward; the upper wall is represented by the next
floating-point value below the box extent.

Validation preserves membership, finite records, canonical coordinates and speed
limits. Invalid intermediate arithmetic rolls back the whole tick and its counter.
Allocation is bounded to one million agents; twice each box extent must remain
finite, and the spatial index enforces its bin-count limits. Seeded initialization
uses separate position and velocity streams and explicit
seed/scenario/replication/stable-ID addresses. Positions are uniform over the box;
each initial velocity component is uniform within ±half the maximum speed.
Evolution consumes no random draws. See [semantics](SEMANTICS.md#boids-flocking-reference-model).

This is a declared Boids variant exposed through a native API. It does not add
force expressions or boundary policies to typed ABM IR. Its tests target finite
horizons, not long-time trajectory identity in chaotic regimes.

## Validation design

The [frozen plan](../tests/oracles/abm/boids-plan.json) has four n=20 cases:
balanced periodic steering, stronger periodic alignment, stronger periodic
separation, and balanced steering with reflecting walls. Each engine runs 512
independent initializations per case, observing ticks 0, 8 and 24: **2,048 runs and
6,144 snapshots per engine**.

The independent implementation uses pinned Mesa 3.5.1 `Model`, `Agent`,
`AgentSet.do` and `ContinuousSpace`, with NumPy 2.5.3 steering calculations. One
pass computes every next state; a second pass commits positions and velocities.
Mesa supplies neighborhood queries, headings, movement and distance summaries.
Its heading helper chooses the opposite sign at an exact half-box tie, so the
adapter explicitly applies the declared tie convention. Native hand tests cover it.

**All 32 predeclared distribution gates pass.** They compare four statistics at two noninitial
times across four cases: polarization (mean unit-heading magnitude, stationary
agents contributing zero), mean speed normalized by its cap, fraction of agent
pairs within vision, and mean pair distance normalized by the domain's maximum distance.
The scorer independently derives these metrics from raw states using pairwise
geometry and verifies the stored Mesa summaries.

Each gate requires both a mean-gap and an empirical-CDF distance bound. Mean
tolerances use the larger of a fixed metric floor and six standard errors; the
fixed CDF-distance limit is .147019; the largest observed distance is .083984. Confidence intervals for mean differences
are diagnostics. The plan records a conservative per-comparison miss probability
of about .01001 for a true CDF displacement of .30. This targets material
distribution differences within the declared cases.

Paired trajectory comparisons use Mesa initial states for three replications in
each case. They check **2,880 scalar state values** against the frozen Mesa paths
with predeclared 2e-10 absolute/relative tolerance; maximum measured error is
2.665e-15. Separate Python Philox checks
cover **960 exact initializer values**; eight further golden values check upper
seed/entity bits and maximum stream/scenario/replication addresses. Statistical
ensemble runs retain independent initializations and seeds.

Hand tests cover exact alignment/cohesion/separation updates, shared snapshots,
inclusive radii, colocated agents, seam and antipodal displacement, speed and
acceleration caps, reflecting multiple/exact wall hits, empty populations,
malformed parameters/records, failed-tick rollback/retry, replay, observation
invariance, spatial-bin independence and ID permutation within rounding tolerance.
The native targets disable fast-math and floating-point contraction.

Scorer contracts reject stale/corrupt observations, invalid coordinates/speeds,
nonfinite states and wrong summaries. Frozen initial states that preserve all basic
state invariants must fail the dynamic model gates. Reference metadata records
plan, adapter and dependency hashes. Routine tests consume frozen data offline;
CI is configured to regenerate references with pinned dependencies. Regeneration
checks all 491,520 scalar state values using the same predeclared floating-point
tolerance, allowing numerical rounding differences across CPU/NumPy implementations.
Local regeneration has maximum error zero.

```sh
ctest --test-dir build --output-on-failure -R '^abm_boids'
.venv-abm-oracle/bin/python tests/oracles/abm/boids_oracle.py --verify
```

The result report is `build/abm-boids-report.json`. Current pass counts and measured
errors are recorded in [status](STATUS.md); [M4 acceptance](M4_ACCEPTANCE.md) tracks
the remaining canonical models and sync/async SIR convergence.
