# M2 SD validation progress

The nonlinear, analytic and assessable-corpus validation areas of M2 now pass.
There are **72 independent nonlinear-reference trajectories** and **56 additional
closed-form convergence trajectories**, plus discrete and boundary checks.
The [acceptance review](M2_ACCEPTANCE.md) records three §7.3 validation areas
passing and the function-semantics area partial. M2 remains open for unresolved
source-function contracts, especially history-based DELAY N timing.

## Models and reference equations

All parameters and grids are stored in
[the independent reference artifact](../tests/oracles/sd/reference.json). The
native models use stock/flow primitives; the Python oracle uses direct ODEs.

| Model | Equations and initial state | Horizon; base timestep; sample interval |
|---|---|---|
| SIR | `S' = −βSI`, `I' = βSI−γI`, `R' = γI`; β=0.6, γ=0.2; `(0.99, 0.01, 0)` | 32; 1/4; 1/2 |
| Lotka–Volterra | `x' = αx−βxy`, `y' = δxy−γy`; α=1, β=0.5, δ=0.25, γ=0.75; `(4, 2)` | 8; 1/16; 1/4 |
| Bass | `F' = (p+qF)(1−F)`; p=0.03, q=0.7; F(0)=0 | 16; 1/2; 1/2 |
| Logistic | `x' = rx(1−x/K)`; r=0.8, K=1; x(0)=0.1 | 12; 1/2; 1/2 |
| Damped oscillator | `x'=v`, `v'=−2dv−ω²x`; d=0.2, ω=1; `(1, 0)` | 12; 1/8; 1/4 |
| Time-dependent growth | `x'=tx`; x(0)=1; exact solution `exp(t²/2)` | 2; 1/8; 1/8 |

The oscillator uses signed stocks and nonnegative opposing inflow/outflow pairs.
Their net derivative remains the smooth oscillator RHS. The time-dependent case
specifically exercises intermediate stage times, which autonomous models cannot
validate. Each timestep is halved three times. All grids are binary-exact and
end exactly at the horizon; no shortened final step or time tolerance is used.

## Gates

At the same fixed observation times for every refinement, the test computes
`E(h) = max |native − reference| / stock_scale` across all stocks and times.
Scales are one for SIR, Bass, logistic, and the oscillator; `(4,2)` for
Lotka–Volterra; and eight for time-dependent growth. RMS scaled error is recorded
as a diagnostic. Errors at initial time are included; convergence is never judged
from the endpoint alone.

Error must decrease at every halving. The last two halving pairs must meet the
following predeclared order bands, using `log2(E(h)/E(h/2))`. The first pair is
reported but may still be outside the asymptotic regime. The finest trajectory
must also meet an independent accuracy ceiling.

| Integrator | Observed order band | Finest maximum scaled error ceiling |
|---|---|---|
| Euler | 0.85–1.15 | 0.05 |
| Midpoint | 1.8–2.2 | 0.003 |
| RK4 | 3.6–4.4 | 0.000005 |

These are suite-specific acceptance limits, not general solver error guarantees.
The ceiling prevents a large but correctly scaling error from passing. The order
bands tolerate finite-step corrections while distinguishing the three methods.
The finest error must also remain greater than 100 times the largest observed
reference discrepancy (or machine epsilon), divided by the smallest stock scale,
so the measured order is separated from the observed oracle numerical floor.
Cross-solver agreement alone does not establish a formal reference error bound.

All integrated states must be finite. Population models remain nonnegative;
Bass/logistic trajectories remain monotone and bounded. At every native step,
SIR population is conserved within `5e-13`, susceptible population cannot rise,
and recovered population cannot fall. Lotka–Volterra populations stay positive;
drift in `δx−γ log(x)+βy−α log(y)` is recorded as a diagnostic, not constrained to
zero for finite-step integrators.

The oscillator additionally checks Euler against its exact discrete solution,
not just the continuous oracle. With `w=sqrt(ω²−d²)` and
`z=(1−dh+iwh)^n`, `x_n=Re(z)+(d/w)Im(z)` and
`v_n=−(ω²/w)Im(z)`; both agree within `2e-12` at every observation.
Seven injected failures cover every evaluation stage of Euler, midpoint, and
RK4, requiring stock state to remain unchanged after each failed step.

## Evidence

The [saved CSV](../tests/sd/convergence-baseline.csv) contains all 72 results.
Its [metadata](../tests/sd/convergence-baseline.metadata.json) records environment,
flags, reference/test/kernel hashes, and the CSV hash. The existing integrator
implementations passed; this increment adds validation without changing them.

Observed order on the final halving in the local Debug build:

| Model | Euler | Midpoint | RK4 |
|---|---:|---:|---:|
| sir | 1.0005 | 1.9904 | 3.9877 |
| lotka_volterra | 1.0202 | 2.0021 | 4.0024 |
| bass | 1.0003 | 1.9657 | 3.9615 |
| logistic | 1.0075 | 1.9720 | 3.9699 |
| oscillator | 1.0272 | 2.0045 | 4.0055 |
| time_growth | 0.9534 | 1.9711 | 3.9745 |

Verification at the nonlinear-suite checkpoint on macOS arm64 / Apple Clang 21.0.0:

- Normal Debug build: **58/58 CTest tests pass**.
- ASan/UBSan Debug build: **58/58 pass**.
- Stored references match independent regeneration with the pinned SciPy stack.
- The largest local reference cross-check gap is below `9e-13` absolute.

CI is configured to verify references on macOS and Linux; no remote CI result is
claimed here. CTest writes `build/sd-convergence.csv`; only the declared numerical
gates are enforced, not byte equality against the saved CSV.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target sd_nonlinear_tests
ctest --test-dir build --output-on-failure -R '^sd_nonlinear$'
```

Reference generation and verification instructions are in the
[oracle README](../tests/oracles/sd/README.md).

## Function and import increment

`sd_functions` adds exact and neighboring half-step boundary tests for
STEP/PULSE/RAMP, unit inference and nested table checks, smoothing initialization
and hand recurrences, analytic first-order convergence for both smoothing
orders, and failure-state isolation. `ir_functions` checks the full ten-row
hand-derived trajectory, invalid expressions and reserved IDs, and rejection
of tick-dependent functions in off-grid hybrid flows. Unary negation now
preserves dimensional units.

The initial scalar Euler [XMILE importer](XMILE_IMPORT.md) covered arithmetic,
acyclic auxiliaries, constant initialization, and stock/flow wiring. It requires
an explicit metadata-only source-unit policy. Three pinned file variants (two
models) pass **38,894 stock-value comparisons** against historical Stella and
Vensim exports; separate tolerances account for their differing precision.
[The corpus report](../tests/oracles/xmile/README.md) documents these gates,
source hashes, and the initial 67-file inventory with 3 accepted and 64 rejected. The next increment below extends that evidence.
A clipping-enabled variant is an explicit rejection test. This is initial
coverage, not completion of the 80% eligible-corpus acceptance gate.

## Lookup and stateful XMILE increment

`xmile_functions` checks named and embedded graphical functions, explicit knots,
custom separators, xscale-generated points, clamping/extrapolation, table-based
initialization, and SMTH1/SMTH3/DELAY1/DELAY3 mappings. Every stateful call owns a
native delay component; aliases share state and separate calls remain distinct.
Hand-derived recurrences cover defaults, signed smoothing, nested delays,
feedback, and failed initialization. This checkpoint used constant durations; the next increment below extends validation to variable durations.

Native flows now support explicit `non_negative: false` while retaining the
original nonnegative default. C++ tests cover direction reversal, conservation
across all three integrators, stock bounds, and nonfinite-stage rollback. IR
checks cover standalone, DES/SD, ABM/SD, and staffed-delivery SD runners.

The pinned legacy lookup model contributes **362 comparisons** against both
Stella and Vensim stock trajectories, including its negative-flow interval.
Two custom XMILE fixtures contribute **1,376 comparisons** against independently
generated PySD 3.14.3 results, including reversal of source variable ordering.
The generator reads source XMILE directly and never calls Fathom. Stored source
hashes and pinned package versions make the evidence reproducible offline.
Named extrapolation agrees with PySD; its inline-extrapolation limitation is
recorded in the [oracle README](../tests/oracles/xmile/README.md), with our inline
path checked against the specified policy and an exact hand trajectory.

The full upstream audit now records **4 importable files and 63 rejections**;
all four accepted files have complete historical stock-trajectory checks. The
two custom fixtures are separate from that denominator. Total stock-value
comparisons are **40,632**, including repeated file/order variants. This does
not meet the M2 eligible-corpus gate by itself.

## Variable-duration increment

Native `delay.duration` now accepts a time-valued expression. Each tick evaluates
all durations against one committed stock/time snapshot before output and Euler
updates; material pipeline quantities persist while output rates respond to the
new duration. Scenario overrides affect initial pipeline construction. Duration
expressions cannot depend on delay outputs, and invalid durations fail explicitly
without order reduction, including at the final observation.

`ir_variable_delays` verifies the hand-calculated output jump, information-state
continuity, third-order lag, source-order invariance, stock feedback, override
initialization, and initial/late/horizon errors with exact diagnostic pointers.
The independent schema contract also covers the new numeric-or-expression field.
C++ tests check material conservation across changing durations, bounded
information state, failed-step isolation, and Euler convergence to an analytic
variable-duration step response for both supported orders.

The new source XMILE fixture has 12 stocks over 33 times. Its full native
trajectory, also with reversed variable order, matches directly generated pinned
PySD results: **792 additional comparisons**. Across all historical and custom
XMILE checks, the total is **41,424 comparisons**. The upstream coverage count
remains **4/67**; the new fixture is separate authored evidence, not a corpus
coverage claim. Cross-tool validation applies to the declared finite, stable
Euler subset; broader delay families and implicit duration dependencies remain open.

Variable-duration checkpoint verification on macOS arm64 / Apple Clang 21.0.0:

- Normal Debug: **63/63 CTest tests pass**.
- ASan/UBSan Debug: **63/63 pass**.
- Independent schema contract: **20 valid fixtures, 47 structural and 29
  semantic invalid mutations** pass.
- Pinned PySD reference regeneration passes locally. CI is configured to repeat it.
- Python-only importer/contracts also pass under the local Python 3.14.2;
  CTest selected Python 3.11.4. No remote CI run is claimed.

## General-order and fixed-delay increment

Euler material/information delays and smoothing now support integer orders
1–255. Fixed signal delays preserve an exact integer lag of 1–1,000,000 ticks.
Native IR and schema expose `type: "fixed"` with `order: 1`; fixed durations
freeze at initialization after scenario overrides. Fractional lag, invalid order,
nonfinite values, and unstable Euler stages fail explicitly.

Importer 0.4 maps SMTHN, constant-duration DELAYN, and whole-tick DELAY.
An independent fourth PySD fixture adds 1,666 matching comparisons. Combined
historical and authored evidence now totals **43,090 matching stock values**.
A nested fixed-delay discrepancy in PySD is retained and documented in the
[oracle report](../tests/oracles/xmile/README.md); its 98 native observations
use an exact sum-of-lags hand oracle and are excluded from matching counts.

C++ tests verify exact N-stage pulses through order 255, variable-duration
material balance, and first-order convergence at orders 2, 5, and 8. Fixed-delay
tests cover one-tick lag, repeated ring wrapping, signed history, copying, and
failed-step isolation. IR/source tests cover aliases, defaults, nesting, feedback,
reordered components, frozen expressions, scenario reinitialization, error
pointers, and failure without partial CSV publication.

Local verification: **63/63 normal and 63/63 ASan/UBSan CTest tests pass**;
schema contract passes **21 valid fixtures, 54 structural and 29 semantic
invalid mutations**; pinned PySD regeneration verifies. The test target count
is unchanged because existing suites gained cases. The full corpus audit
remains **4/67 importable**, with no modified upstream assets.

## Independent corpus eligibility and expression outputs

The complete pinned corpus now ships as unmodified XMILE sources so inventory
regeneration runs offline in CTest. Eligibility policy v1 inspects equations
independently of importer success, records structural features, and retains
structurally unsupported models with supported equations in the denominator.
An injected importer-failure test proves that regressions cannot improve
coverage by shrinking that denominator. Source hashes, missing/extra files,
reference availability, and stale reports are checked.

Of 67 files, **8 are assessable and equation-eligible**, **10 use unsupported
equations**, and **49 have malformed XML and remain unassessable**. The
importer now accepts **5/8 eligible files (62.5%)**, or 5/67 across the full
inventory. Three eligible structural gaps remain: clipping; RK4 selection;
and a nonzero-start fixed-delay model with clipping and vendor settings.
See the [coverage report](XMILE_COVERAGE.md) for the versioned policy and
file-level evidence. This does not meet the 80% M2 target.

Standalone IR gains unit-checked pure expression outputs and permits models
without stocks. Importer 0.5 uses these for stock-free scalar auxiliaries,
retaining explicit delay state when needed. The fifth accepted upstream model
checks left-associative arithmetic against its original Stella CSV: two exact
auxiliary observations. Existing stock-comparison totals remain 43,090.

New contracts verify time/parameter dependence, scenario overrides, table/stock/
delay observations of the committed tick, output-order invariance, source
auxiliary expansion and cycle rejection, units, malformed expressions, and
horizon failures without partial CSV output.

Local validation: **65/65 normal and 65/65 ASan/UBSan CTest tests pass**.
The independent schema suite passes **22 valid fixtures, 60 structural and
32 semantic invalid mutations**. Offline audit regeneration matches the
clean pinned upstream checkout. No remote CI result is claimed.

## Declarative and source RK4 increment

Native standalone SD accepts `integrator: "rk4"`, selecting the existing
classical four-stage kernel. Omission keeps Euler. Other runtime modes, delay
components under RK4, and RK4 flow expressions using the Euler tick-specific
STEP/PULSE/RAMP functions are rejected explicitly. Expression outputs read
committed observation states; flow expressions use each stage's stock/time
snapshot and the full nominal dt.

Native tests check an exact one-step `y'=t*y` calculation, a coupled oscillator,
lookup queries at stage times, overrides, component-order invariance, fourth-order
convergence, and a division-by-zero failure that occurs only at the midpoint.
Invalid methods and unsupported mode/component combinations have structured
loader errors and independent schema checks.

The XMILE mapping preserves Euler/RK4 selection and translates an auxiliary
explicitly named in a stock edge as one shared signed native flow. Tests cover
source/destination conservation and duplicate/invalid endpoint rejection. Two
authored RK4 source fixtures (SIR and time-dependent growth) score **1,272
observations** against independent pinned SciPy references across three
resolutions and both variable orders. Error ratios must be 13–19 at each
halving and finest-resolution absolute error below 1e-6.

The newly imported upstream `zeroled_decimals` model declares RK4 but ships an
Euler historical export. Its six RK4 stocks have **132 closed-form checks**
including reordered source. The diagnostic Euler rerun matches the export,
but is excluded from source-faithful historical coverage. Import coverage
is **6/8 eligible files (75%)**; compatible complete historical coverage stays
**5/8 (62.5%)**. The 49 malformed files remain unassessable. M2 is still open.

Local verification: **67/67 normal and 67/67 ASan/UBSan CTest tests pass**;
schema contract: **23 valid fixtures, 71 structural and 33 semantic invalid
mutations**. Stored independent SD references regenerate within their existing
gates using DOP853, tighter DOP853, Radau, and analytic checks. No remote CI
result is claimed.

## Bounded Euler clipping increment

The kernel, native IR/schema, and XMILE importer now implement the explicit
[Euler clipping contract](SEMANTICS.md#bounded-euler-clipping). Requested rates
read pre-tick state; outgoing amounts are limited in explicit priority order,
using current incoming amounts. One amount is applied at both endpoints.
Candidate stock state is committed only after all evaluation and validation pass.

`sd_clipping_tests.cpp` checks competing priorities, exact depletion, shared-flow
conservation, same-tick cascades in reverse declaration order, old-state callbacks,
roundoff, negative-rate filtering, overflow/nonfinite rejection, invalid graphs,
and rollback. `clipping_tests.py` checks native and imported trajectories,
component/source permutations, parameter overrides, and structured rejections.
Cyclic clipped-stock graphs, signed incident flows, source equations reading
clipped flows, non-Euler integrators, and hybrid clipping remain outside scope.

The pinned diagram teacup now contributes **482** matching observations against
its two historical exports. Total matching cross-tool stock comparisons are
**43,572**, plus two auxiliary observations. Its clipping limits never bind;
active-limit correctness rests on the hand-derived allocation tests. This is
not evidence of arbitrary source-filter or cross-engine clipping equivalence.

Coverage is **7/8 imported (87.5%)**, but **6/8 historical-compatible (75%)**.
The structural threshold is met; historical agreement and M2 remain open.
The RK4/Euler export conflict and 49 unassessable malformed files are unchanged.

Local verification: **69/69 normal and 69/69 ASan/UBSan CTest tests pass**.
Schema contract: **24 valid fixtures, 83 structural and 38 semantic invalid
mutations**. The stored corpus report is regenerated and verified offline.
Earlier sections preserve the evidence recorded at prior increments.

## Absolute-clock fixed-delay source increment

Standalone SD now preserves optional `time.start` with an elapsed `horizon`.
Initial duration validation, runtime TIME, RK4 stages, and observations use the
absolute clock. Source stock and default delay initializers use source start.
Degenerate/nonfinite tick grids and indistinguishable RK4 stages reject at lint.
Other modes reject the new start field until their schedule semantics support it.

The importer accepts the pinned fixed-delay source's bounded vendor metadata
and references to sign-filtered flows without a limiting source stock. These
references lower to a finite, unit-preserving pure `NONNEGATIVE` expression;
stock-limited flow references remain rejected. `--outputs all` enables complete
source-variable comparison without changing default stock-only output behavior.

The unmodified source has **208 matching historical observations** across all
eight variables, 13 ticks and two source orders. The reference README does not
identify a generating simulator. The full matching counts become **43,598
stock, 158 auxiliary and 26 flow values**. New hand tests cover active sign
filtering in both directions, alias/default initialization, fixed-lag release,
negative starts, source TIME initialization, units, parameter overrides and
native RK4 stage timing. Pure-filter tests include nonfinite rejection and signed
zero agreement with kernel clipping.

All **8/8 assessable eligible files import**; **7/8 (87.5%)** have compatible
complete historical references, passing the 80% threshold under policy v1.
The RK4/Euler export conflict and 49 malformed unknown files remain explicit.
This closes the assessable-corpus coverage gate, not all M2 requirements.

Local verification: **70/70 normal and 70/70 ASan/UBSan CTest tests pass**.
Schema contract: **25 valid fixtures, 91 structural and 44 semantic invalid
mutations**. Vendored source/reference hashes and the regenerated corpus audit
verify against the clean pinned checkout. No remote CI result is claimed.

## Analytic acceptance increment

`tests/sd/analytic_tests.cpp` supplies independent discrete and continuous
formulas, evaluated without repeating the simulator's per-stage update loop.
The simulation kernels did not change for this increment.

For exponential growth/decay, `x'=r*x`, use `x(0)=3`, `r=±0.5`, horizon 4,
and timesteps 1/4, 1/8, 1/16, 1/32. Every tick must match the discrete solution
`x[k]=3*R(r*h)^k`, where R is `1+z` for Euler, `1+z+z²/2` for midpoint, and
`1+z+z²/2+z³/6+z⁴/24` for RK4. Continuous errors compare against `3*exp(r*t)`
on the same quarter-unit observation grid at each refinement. All trajectories
must remain positive and monotone in the expected direction.

For material and information delays of order 1 and 3, stage duration is 2
(total duration `2*order`). Inputs step from 2 to 5 and from 5 to 2; horizon is
8 with the same four timestep resolutions. With `a=h/2`, stage m's discrete
step response at tick k is
`F_m(k)=1−sum(j=0..m−1, C(k,j)*a^j*(1−a)^(k−j))`, with zero response for k<m.
The expected signal is `initial+(input−initial)*F_m(k)`; material stages store
twice that value. The continuous response replaces F with
`1−exp(−t/2)*sum(j=0..m−1, (t/2)^j/j!)`. Every stage, the final output and the
material pipeline have discrete checks at every tick. Material balance and
signal bounds are checked separately. Sixteen additional trajectories use
`h=1` and `h=2`, including the Euler stage-duration stability boundary.

The constant-duration delay error is normalized by the step amplitude (3).
Exponential error is normalized by its initial value (3). Maximum error is
measured across common observation times, not only the endpoint. The final
cascade output has its own error/order gate, in addition to the maximum over
all stages, so a correct first stage cannot hide a defective final output.

| Family/method | Required order on last two halvings | Finest scaled error ceiling |
|---|---|---|
| Exponential Euler | 0.9–1.1 | 0.13 |
| Exponential midpoint | 1.9–2.1 | 0.001 |
| Exponential RK4 | 3.9–4.1 | 2e-8 |
| Both delay kinds, Euler | 0.9–1.1 | 0.004 |

Errors must decrease at every halving and remain greater than `100*epsilon`
at the finest grid to separate measured orders from roundoff. Discrete agreement
uses `|actual−expected|/max(1,|expected|) <= 512*epsilon`; this permits floating
roundoff, not discretization error. Material balance uses the analogous bound.
`epsilon` is the native double machine epsilon. Closed forms use standard-library
long-double arithmetic where available; no extra precision is assumed on
platforms where long double equals double.

For logistic growth, the Euler case `r*h=1` has a useful exact discrete form:
`x[k]=K*(1−(1−x[0]/K)^(2^k))`. Five initial conditions (0, 2, 6, 8, 10) with
K=8 give 45 observations, including both equilibria and an initial state above
capacity. This is a special-step discrete oracle, not a claim of an elementary
closed form for arbitrary Euler logistic steps. The existing nonlinear suite
already validates logistic convergence at general timesteps and all three methods.

The [56-row baseline](../tests/sd/analytic-baseline.csv) and
[provenance](../tests/sd/analytic-baseline.metadata.json) record the new results.
Final measured orders are about 0.974–1.010 for exponential Euler,
1.983–2.017 for midpoint, 3.981–4.019 for RK4, and 1.010 for both delay orders
and directions (including final output). The largest scaled discrete discrepancy
is below `1.90e-14`; material balance drift is below `1.78e-14` in quantity units.

Full normal and ASan/UBSan suites pass **71/71 tests**; the strengthened analytic
target also passes in both builds. Existing source/schema contracts are unchanged.
The independent source corpus remains 8/8 imported and 7/8 historical-compatible;
this increment does not enlarge its denominator or claim a remote CI result.

## Explicit DELAYN cascade and XMILE inputs (importer 0.9)

`xmile_dialects` adds 1,386 independent comparisons (660 stock and 726 auxiliary
observations, including reversed source order) for explicit variable-duration
DELAYN cascade imports. PySD 3.14.3 runs a separate model with literal material
stocks and flows, so its DELAYN implementation is not the oracle for the cascade.
Orders 1/2/3/5, abrupt/gradual changes, default/explicit initial values,
stock-driven duration, feedback, nesting and aliases agree at 2e-12 relative or
absolute tolerance. A two-stage hand oracle checks stage contents, current
duration timing, integrated output and conservation. Failure and scenario
override checks exercise the imported model and the native initialized pipeline.

The unexpanded PySD DELAYN trajectory is retained as a history-dialect
diagnostic, excluded from matching counts. Source hashes, pinned package
versions and generator hash accompany both trajectories. CI regenerates them;
routine CTest runs offline against the saved artifact. This policy is explicit
in the CLI and metadata; default imports continue to require constant duration.

Source STEP/RAMP/PULSE now map to separate native XMILE functions. Complete
hand trajectories cover .1/.125/.5/1 dt, starts 0/10/−2, signed values, variable
height, one-shot/periodic pulses, and the final observation. Pulse quantity is
independent of dt. Additional checks cover initialization, nested delays,
before-start schedules, overrides, units, arity, reserved identifiers and
rejection of off-grid schedules, coarsely resolved clocks, negative/fractional
intervals, variable schedules, RK4 imports and hybrid flow usage.

Both normal and ASan/UBSan suites pass **72/72 tests**. Schema checks pass
**26 valid fixtures, 91 structural and 50 semantic invalid mutations**. The
oracle regenerates with pinned dependencies; the refreshed corpus audit agrees
with both vendored files and the clean pinned checkout. No remote CI run is
claimed. See [import details](XMILE_IMPORT.md) and [oracle provenance](../tests/oracles/xmile/README.md).

## Bounded history delays (importer 0.10)

`xmile_history` verifies 101 historical Vensim output observations for a native
projection of the pinned second-order DELAY N component, plus 1,386 direct PySD
source observations across both declaration orders. These are separate from the
earlier current-duration cascade comparisons. C++ hand tests check the two
stage quantities, previous-duration output timing, repeated observations,
signed balance, fixed-dt validation, overflow and rollback. Six zero-input
drain tests check conservation at every tick and complete release.

The implementation is intentionally order 2 only. The new pinned report retains
24 PySD conservation probes: 12 pass (constant-duration controls and all order-2
cases), while changing-duration cases for orders 1/3/5 fail. An integer-duration
truncation diagnostic is retained too. `--verify` reproduces this evidence;
`--check-all-orders` exits 1 because the broader acceptance gate is unsatisfied.
Neither diagnostic counts as a passing production implementation.

Normal and ASan/UBSan suites pass **73/73 tests**. Schema checks pass **27 valid,
95 structural-invalid and 50 semantic-invalid cases**. The new pinned oracle
regenerates and CI includes regeneration; no remote CI run is claimed. The
source corpus remains 8/8 assessable imports and 7/8 historical-compatible;
the MDL projection is separate from XMILE corpus coverage. See
[history contract and evidence](DELAY_HISTORY.md).

## Explicit off-grid input policy (importer 0.11)

`xmile_inputs` checks 288 authored source cases against independently enumerated
exact-rational event schedules and Euler stock sums. All 17 rate and stock
observations pass in both source orders: **19,584 comparisons**, at 2e-12 relative
or absolute tolerance. Cases span four dt values, three absolute origins,
off-grid starts, signed inputs, one-shot pulses, fractional intervals and
multiple pulses per tick. The oracle generator uses event enumeration and
rational ceiling rather than the runtime's cumulative-floor algorithm.

The explicit `next_tick` policy preserves pulse quantities through shared
integer window boundaries. Extra tests cover exact and neighboring boundaries,
roundoff snapping, long decimal clocks, observation purity, initialization,
delays, dynamic quantities, overrides, units, reserved IDs and invalid schedules.
Default strict-grid and existing native function semantics remain unchanged.

Normal and ASan/UBSan suites pass **74/74 tests**. Schema validation passes
**28 valid fixtures, 95 structural and 55 semantic invalid mutations**. The new
rational artifact regenerates exactly; CI includes that verification. Corpus
audits match both vendored bytes and the clean pinned checkout, with unchanged
coverage. No remote CI run is claimed. See [timing contract](OFFGRID_INPUTS.md).

## Remaining M2 work

The [requirement-by-requirement review](M2_ACCEPTANCE.md) separates the completed
analytic, nonlinear-reference and assessable-corpus gates from the partial
function-semantics gate. The explicit cascade and grid-aligned input contracts
and second-order history are implemented, and an explicit off-grid next-tick
policy is now validated. Other history orders still require authoritative
evidence; universal vendor off-grid behavior is not inferred from our chosen
policy. Eligibility policy v2 adds STEP/RAMP/PULSE without
changing the pinned corpus counts.

Additional limitations include applied references to availability-limited flows,
cyclic clipping, delay-output-dependent durations, fractional fixed delays,
RK4 discontinuity/delay handling and hybrid clipping. The new tests do not imply
stiff solvers, adaptive error control, or general XMILE conformance. Phase 1 and
M2 remain in progress.
