# Pinned XMILE import references

Source: [SDXorg/test-models](https://github.com/SDXorg/test-models), commit
`21aab02739dc5187bc9564e4d3de14e575905d2f`. This is the test-model corpus used
by the PySD ecosystem. `manifest.json` records SHA-256 hashes of the vendored,
unmodified files. MIT license and author notices are in `corpus/`.

The initial slice imports plain teacup, SIR, and reciprocal-dt SIR. The second increment adds the signed-flow lookup model. Every stock
at every tick is compared with both upstream `output_stella1006.csv` and
`output.csv`: **38,894 comparisons** across three file variants (two distinct
models). The upstream READMEs attribute these exports to Stella 10.0.6 and
Vensim DSS 6.3. Neither application is run by our tests.

Value gates use `abs(a-b) <= max(atol, rtol*max(abs(a),abs(b)))`:

| Reference | Relative tolerance | Absolute tolerance | Time tolerance |
|---|---:|---:|---|
| Stella 10.0.6 | 5.1e-12 | 1e-13 | 0.00050000001 absolute (3 decimal places) |
| Vensim DSS 6.3 | 1e-5 | 1e-13 | 5.1e-6 relative or 1e-9 absolute (6 significant digits) |

Fathom output times must also equal `index*dt` exactly for these binary-fraction
steps. Stella's stock exports retain about 12 significant digits. The legacy
Vensim SIR export differs by up to `5.9991e-6` relative from our trajectory,
slightly beyond a six-significant-digit rounding-only bound; we therefore use
it as a separate, lower-precision cross-check. Stella's maximum relative SIR
gap is below `4.93e-12`. These observations do not establish the internal
precision or arithmetic order used by either historical application.

The diagram teacup variant now imports with explicit Euler clipping. Its 241
stock observations match each historical export, adding 482 comparisons. Limits
do not bind on this trajectory; separate hand-derived tests validate active
priority allocation and conservation. Rejected models are not silently repaired.

Run offline with:

```sh
python3 tests/xmile_import_tests.py build/fathom
```

`coverage.json` audits **all 67 .xmile files** at the pinned commit. All original
XMILE sources are now vendored for offline regeneration. Importer and audit
hashes bind the report to their implementations. Current results: **8 importable,
59 rejected; 8 equation-eligible, 10 outside the equation subset, 49 unassessable
because XML is malformed**. Eligibility is evaluated independently of import
success; structural limitations remain in the denominator. Coverage of
assessable eligible files is **8/8 = 100%** structurally; **7/8 = 87.5%** have
compatible complete historical references. One import has the RK4/Euler conflict
below. The import and 80% historical-agreement thresholds are met for the assessable subset. Other M2 contracts remain open.

The fifth accepted file is `tests/eval_order/eval_order.xmile`, a stock-free
auxiliary model. Both output values match its original Stella 10.0.6 CSV exactly.
The vendored README records authorship and source tool versions. These are two auxiliary comparisons, separate from the current 43,598 matching
stock comparisons. See [eligibility policy and remaining gaps](../../../docs/XMILE_COVERAGE.md).

```sh
python3 tests/oracles/xmile/audit.py --verify
python3 tests/xmile_coverage_tests.py
```

To reproduce the complete audit using a clean local checkout:

```sh
git clone https://github.com/SDXorg/test-models /tmp/fathom-test-models
git -C /tmp/fathom-test-models checkout 21aab02739dc5187bc9564e4d3de14e575905d2f
python3 tests/oracles/xmile/audit.py /tmp/fathom-test-models --out build/xmile-coverage.json
```

## Lookup/stateful increment

The unmodified `tests/lookups/test_lookups_no-indirect.xmile` now imports,
including its legacy `discrete="false"` encoding, xscale-derived points, and
signed flow. All 181 stock observations match both `output_stella1006.csv`
and `output.tab` (**362 comparisons**). Stella uses relative/absolute tolerance
`2e-12`; the Vensim tab export uses relative `1e-5`, absolute `2e-12`. Time values
are exact quarter steps in these files. Hashes and upstream attribution are
included in `manifest.json` and the vendored README.

`fixtures/stateful.xmile` and `fixtures/lookups.xmile` are authored here and are
**not upstream corpus files**. They contain 688 stock observations per run.
`generate.py` reads these XMILE sources directly with pinned PySD 3.14.3 and
never imports Fathom code or reads Fathom output. It stores stock values, source
hashes, times, and numerical/translation package versions in
`pysd-reference.json`. The interpreter uses Euler with each source's dt.

For the first two fixtures, CTest compares every stored stock/time, then reverses
each fixture's source variable order and compares again: **1,376 comparisons**, using relative and
absolute tolerance `2e-12`. Hand-derived tests separately verify first/third-order
responses, default initialization, state sharing, nested delays, feedback,
signed values, interpolation knots, custom point separators, constant lookup
initialization, and explicit rejection paths. Material-delay tests use
nonnegative inputs; those first two fixtures do not establish variable duration
or signed material semantics. The third fixture below adds variable-duration evidence.

PySD 3.14.3's inline lookup builder drops the extrapolation policy and emits
`np.interp`. For example, x=[1,2,3], y=[1,3,5], queried at time 0 with linear
extrapolation should return -1; that inline PySD path returns 1. The named-table
path preserves extrapolation. Our PySD fixture therefore uses the named path;
both native named and embedded forms are independently checked against the
hand trajectory [0,-1,0,3,8] at dt=1 through t=4. This oracle limitation is
explicit, not a widened tolerance or a change to the specified semantics.

Reference generation requires Python 3.12+ with the pinned package set; routine
CTest execution remains offline using the stored results and Python 3.10+.
CI is configured to regenerate on macOS and Linux; no remote result is claimed.

```sh
python -m pip install -r tests/oracles/xmile/requirements.txt
python tests/oracles/xmile/generate.py --verify
python tests/xmile_function_tests.py build/fathom
```

Combined historical and new checks currently make **43,090 matching stock-value
comparisons**. Counts include reciprocal-dt and reordered variants; they do not
represent that many distinct models. M2 remains open.

## Variable-duration increment

`fixtures/variable_delays.xmile` adds 396 stock observations (12 stocks × 33
times). The independent PySD generator reads the source directly. The fixture
covers first- and third-order material/information delays, explicit/default
initialization, shortening and lengthening averaging times, nested calls, a
stock-driven duration, and duration feedback from cumulative delivery. As with
the earlier fixtures, the native importer is run in both original and reversed
variable order, adding **792 comparisons** at relative/absolute `2e-12`.

Across the three custom fixtures, that is **2,168 comparisons**. The full upstream
corpus remains **4/67 importable**: this feature adds validation breadth through
a separate authored fixture, not another accepted upstream file. Existing
upstream assets and their provenance remain unchanged.

Independent hand oracles check the output jump at a duration change, preservation
of material quantity, information-state bounds, and stage simultaneity. A closed
continuous solution for unit-step smoothing with `tau(t)=2+t/2` checks first-order
Euler convergence for both one- and three-stage variants. These checks do not
use PySD-generated values. Bounds require at least one dt per stage; neither
dynamic order reduction nor invalid-duration behavior is copied from PySD.

## General-order and fixed-delay increment

`fixtures/extended_delays.xmile` adds 18 stocks over 49 times. Twelve stocks
exercise SMTHN/DELAYN at orders 2, 5, and 8 with explicit/default initialization.
The others cover one-/three-tick fixed delays, default initial values, a changing
duration frozen at time zero, nested fixed delays, and variable-duration signed
SMTHN. Both original and reversed source ordering are checked.

Seventeen stock trajectories agree with pinned PySD at the existing `2e-12`
tolerances: **1,666 additional matching comparisons**. The nested fixed stock
is deliberately excluded from that count. PySD's sequential `DelayFixed.update`
calls allow an outer delay to read the inner delay's newly updated value. For
`DELAY(DELAY(4+TIME,.25,0),.5,0)` integrated into a stock at dt=.125, PySD reports
.5 at t=.75; the shared-snapshot sum-of-lags contract requires 0 there and .5
at t=.875. The PySD trajectory remains stored, regenerated, and its discrepancy
is asserted; **98 native stock values** are checked against the exact discrete
integral instead. No tolerance widening or source-order dependence is accepted.

Total matching PySD comparisons across four authored fixtures: **3,834**.
Combined with the historical 39,256 comparisons: **43,090**. Additional hand
checks verify fixed feedback, cascades, ring wrap, signed inputs, initialization,
scenario overrides, rejected fractional lags, and failed-step history isolation.
Native N-stage tests cover material conservation with changing durations and
Euler convergence to the analytic Erlang step response at orders 2, 5, and 8,
plus exact discrete pulse propagation at orders 2, 5, 8, and the 255-stage cap.

PySD's `DelayN` carries a history of stage durations. This differs from the
instantaneous stage-duration cascade used by native material delays and PySD's
`Delay` (DELAY1/3). Thus XMILE DELAYN currently requires provably constant
duration; variable-duration native cascades do not imply that mapping is verified.
Stella also documents silent truncation/order bounds and delay stretching in
some cases; Fathom rejects unsupported orders and unstable dt explicitly.
See [source delay semantics](https://iseesystems.org/resources/help/v3/Content/08-Reference/07-Builtins/Delay_builtins.htm).

The full upstream audit remains **4 importable / 67 files**. New authored
fixtures do not change the upstream denominator or complete M2.

## RK4 source mapping and reference conflict

Importer 0.6 preserves `method="RK4"` and handles stock-linked auxiliary rates
from the legacy Vensim export. Unlinked auxiliaries stay algebraic; promotion
IDs appear in the metadata. `fixtures/rk4_sir.xmile` and
`fixtures/rk4_time_growth.xmile` are scored against the already pinned independent
SciPy references in `../sd/reference.json`, not against the Euler PySD runner.
Tests cover three step sizes and both variable orders, totaling **1,272 scored
observations**. Maximum error must fall by a factor between 13 and 19 on each
step halving, with final error below 1e-6. Existing C++ solver tests retain
their broader convergence and invariant gates.

The unmodified upstream `tests/zeroled_decimals/test_zeroled_decimals.xmile`
now imports, including all ten auxiliary-labeled rate equations explicitly
referenced by stock edges. All six stocks (11 observations each) match
closed-form RK4 solutions; reversed ordering doubles these to **132 checks**.
The key equation is `d(stockmixed)/dt = -.6777 - t`, initially zero. At t=10,
RK4 yields -56.777. The historical `output.tab` yields -51.777, exactly the
Euler discretization. Every positive-time value differs by t/2. This is a
method mismatch, not an export-rounding error.

The historical table and its attribution README are pinned unchanged. Tests
verify the difference and run an explicitly labeled Euler diagnostic variant
that matches all 66 historical values, in both source orders. Neither that
variant nor partial matching stocks increases complete source-faithful
historical coverage. The audit stores this conflict and separately reports
structural imports and historical-reference-compatible imports. The original
43,090 matching stock comparisons plus two auxiliary comparisons remain the
existing historical/PySD baseline; RK4 convergence and closed-form checks
are reported separately.

## Euler clipping increment (importer 0.7)

The unmodified diagram teacup is the seventh import. It contributes 482 historical
comparisons, taking the original teacup/SIR suite to **39,376**, and the full
matching historical/PySD stock count to **43,572** (plus two auxiliary values).
The RK4 checks and nested-fixed hand oracle remain separate. Earlier totals
above describe their implementation checkpoints.

Active clipping tests use independent arithmetic expectations for competing
outflows, reversed priority, same-tick upstream/downstream allocation, shared
transfer conservation, negative uniflow filtering, and rollback. The historical
teacup trajectory does not activate its limits, and no general cross-engine
clipping equivalence is claimed. See the [contract](../../../docs/SEMANTICS.md#bounded-euler-clipping).

## Absolute-clock fixed-delay increment (importer 0.8)

`tests/delay_xmile/test_delay_xmile.xmile` is the eighth import. Its original
`output.tab` and `README.md` are now vendored and SHA-256 pinned. The source
uses start=1, stop=13, dt=1, a nonnegative boundary inflow, and five fixed-delay
calls. `--outputs all` exposes every stock, auxiliary and flow. All eight
variables match all 13 reference ticks in both source orders: **208 comparisons**
(26 stock, 156 auxiliary, 26 flow). The predeclared gate is 1e-12 relative or
1e-12 absolute; time labels must match exactly. The README attributes the files
to Eneko Martin but leaves the generating software unspecified. We do not
attribute these results to a particular commercial engine.

Combined matching counts are now **43,598 stock**, **158 auxiliary**, and
**26 flow** values. These counts include source-order repetitions, not distinct
independent trajectories. RK4/SciPy and nested-fixed hand-oracle counts remain
separate. The new historical trajectory keeps its source inflow positive;
hand-derived variants independently test sign filtering across zero in both
directions, default delay initialization, aliases and lag-release boundaries.

```sh
python3 tests/xmile_delay_source_tests.py build/fathom
```

## Explicit cascade dialect increment (importer 0.9)

`fixtures/delayn_variable.xmile` contains eight variable-duration DELAYN calls
plus a shared alias. `fixtures/delayn_cascade_expanded.xmile` independently
expresses the corresponding material pipelines as ordinary stocks and flows.
Both use Euler with dt=.125 from 0 to 4. Orders 1/2/3/5, explicit/default initial
outputs, abrupt and gradual duration changes, a stock-driven duration,
accumulated-output feedback, nested calls and aliases are included.

`generate_delayn.py` reads both XMILE files directly through pinned PySD 3.14.3;
it does not import Fathom code. `delayn-reference.json` records 33 observations
of 21 columns, source hashes, generator hash and all pinned package versions.
The expanded stock model is the oracle for the explicitly selected
`--delayn-policy cascade`. Full comparisons in both source orders total
**1,386** (660 stock, 726 auxiliary), with a gate of 2e-12 relative or absolute.

The unexpanded PySD DELAYN values are saved under `history_diagnostic`. They
disagree with the cascade under changing duration. Regeneration requires that
discrepancy to remain visible, but those values are not a Fathom pass oracle and
never enter matching counts. The direct stock expansion proves the declared
current-duration cascade; it does not prove all Stella/Vensim dialect behavior.
Default conversion still rejects variable DELAYN duration without an explicit
cascade policy. See [semantics](../../../docs/XMILE_IMPORT.md#stateful-functions).

The matching legacy suite remains 43,598 stock, 158 auxiliary and 26 flow
observations; this increment adds 660 stock and 726 auxiliary observations.
Counts include repeated source orders. Source input STEP/RAMP/PULSE trajectories
use independent hand formulas and are not included in these PySD totals.

```sh
.venv-sd-oracle/bin/python tests/oracles/xmile/generate_delayn.py --verify
python3 tests/xmile_dialect_tests.py build/fathom
```

CI installs the existing pinned XMILE requirements and regenerates the new
artifact; normal CTest needs only the saved reference and Python standard library.
Eligibility policy v2 adds STEP/RAMP/PULSE to the reviewed lexical inventory.
All 67 upstream files retain their original bytes, and the corpus coverage counts
remain unchanged. Authored fixtures remain outside the corpus denominator.

## Second-order history evidence (importer 0.10)

`generate_history.py` independently runs pinned PySD on authored
`fixtures/delayn_history2.xmile` and the original upstream
`corpus/tests/delays/test_delays.mdl`. `history-reference.json` records source
hashes, generator hash, pinned versions and trajectories. The new fixture
contains eight second-order calls and an alias; it uses a floating-point
duration-stock initializer to avoid PySD's integer-history truncation.

The native `history2` contract matches 1,386 authored-fixture observations in
both source orders (660 stock, 726 auxiliary), at 2e-12 relative or absolute.
A projection of the upstream DELAY N component separately matches 101 Vensim
historical observations at 1e-5 relative or 5e-6 absolute tolerance. Source,
output and author README are pinned byte-for-byte in the manifest. This is not
an MDL importer or an additional passing XMILE corpus model.

The report also contains 24 direct PySD zero-input conservation probes and an
integer-duration truncation diagnostic. Twelve changing-duration probes for
orders 1/3/5 fail; second order and constant-duration controls pass. These
diagnostics do not count as kernel passes. CI verifies regeneration, while
`--check-all-orders` exits 1 for the unresolved all-order gate. Full recurrence,
provenance, limits and reproduction: [history acceptance](../../../docs/DELAY_HISTORY.md).

## Off-grid input policy evidence (importer 0.11)

`generate_inputs.py` uses only Python's standard library and exact fractions.
It enumerates nominal event times and assigns them by rational ceiling to the
first tick on or after each event. The runtime instead subtracts cumulative
counts at adjacent integer tick boundaries. This provides an independent
contract oracle; neither vendor equivalence nor historical matching is claimed.

`input-reference.json` embeds source XML and source/generator SHA-256 hashes,
plus every sampled rate and Euler stock value. The 288 cases span three origins,
four dt values, four start offsets and four pulse interval choices (with STEP
and RAMP cases as well). Both source orders produce **19,584 matching observations**
at 2e-12 relative or absolute tolerance. These authored cases remain outside the
upstream corpus denominator and separate from the historical/PySD totals.

```sh
python3 tests/oracles/xmile/generate_inputs.py --verify
python3 tests/xmile_input_tests.py build/fathom
```

CI regenerates the reference exactly. The [policy contract](../../../docs/OFFGRID_INPUTS.md)
defines initialization, sub-dt pulse counts, rounding and bounds, source units,
and final-observation semantics.
