# Correctness build status

The acceptance target is the Phase 1 CPU platform in `IMPLEMENTATION_PLAN.md`. An item is complete only when the named conformance and analytic tests pass.

## September 30: level-based T&R monthly pilot

The new [150-person monthly example](../examples/tr_pilot/README.md) uses seven
billable levels (135 initial employees) plus 15 practice support staff. Native SD
now exercises loaded compensation/overhead, expected-FTE workforce policies,
level-specific AI, fee/value backlogs, price erosion, approval/disallowance/
holdback/collection/deposits, senior BD and exogenous demand. Synthetic operational
estimates feed typed native inputs; commercial and workforce policies remain
assumptions. This is a bounded example, not full acceptance of the
[T&R specification](TR_PILOT_SPEC.md).

All **34 five-year scenarios** pass **506,056** independent public monthly-value
comparisons and **47,702** native declared-check evaluations. Maximum absolute
oracle difference is 3.5763e-7 (tolerances: abs 2e-6, rel 2e-10). All twelve batches
pass exact 1/8-thread and portable-bundle replay. A separate observed-filings input
demonstration passes 1,952 comparisons and 184 native checks; its demand elasticity
is synthetic and no real-practice backtest is claimed.

The two new CTest suites pass **27 focused tests** (17 native mechanisms and 10
imperfect-data adapter tests). All 17 native fixtures also pass ASan/UBSan. An
additional three-month/eight-level sanitizer run passes 2,928 comparisons and 276
native checks. These are focused results; the historical full-platform normal and
sanitizer checkpoints below are unchanged. No production C++ engine source changed.

[Recorded results](../examples/tr_pilot/RESULTS.md) explain matched workforce-policy
comparisons. [Compact evidence](../examples/tr_pilot/run-evidence.json) records the
receipts and hashes; full local artifacts are under
`artifacts/tr-pilot-150-validated-20260930/`. Integer-person behavior, contract-level
contingencies, actual Ankura calibration, acquisition economics and business
acceptance remain open. FTI supplies external comparison figures only.

## September 30: synthetic T&R decision test

The current user-specified case is **150 employees**, modeled as 150 full-time
paid staff, superseding an interrupted 200-person attempt. The
[150-person run](../artifacts/ai-decision-tr-150-20260930/validation.json) passes
the same 983 scenarios and 719,556 independent forecast comparisons, historical
reconciliation, and exact 1/8-thread and portable replay. Nine adapter tests pass.
Pipeline and acquisition costs scale 7.5-fold from the original 20-person case;
fee rates and other per-person assumptions remain synthetic and unchanged.
No FTI operating datapoints calibrate either case. The original evidence below
remains historical evidence for the 20-person run.

The [AI decision example](../examples/ai_decision/README.md) now generates 36 months
of operational Parquet exports, recovers 55 planted parameter/seasonality/utilization
values, and executes 983 five-year scenarios through the existing native SD runtime.
All 719,556 forecast stock values agree with an independent Decimal ledger within
the declared mixed absolute/relative tolerance; 444 historical stock values and
72 transaction-reconciliation values also pass. The historical exercise is an
in-sample reconstruction, not predictive validation or a public-peer backtest.
Native forecast declarations pass 773,621 rule evaluations; exact result replay
passes at 1 and 8 threads, including a portable bundle. A deliberately incorrect
T&M formula passes accounting checks but is rejected on 59 revenue observations.

Nine focused Python adapter tests pass, including nondefault parameter recovery,
calibration without the truth file, row-order invariance and corrupt-data rejection.
The forecast, historical and negative-control model files pass IR JSON Schema
validation. [Local evidence](../artifacts/ai-decision-tr-validated-20260930/validation.json)
and [results](../artifacts/ai-decision-tr-validated-20260930/results.txt) retain the
inputs, assumptions and provenance. All business inputs and intervention effects
are synthetic. No production engine sources changed, no fresh full regression or
sanitizer run is claimed, and platform milestone acceptance remains unchanged.

## Prior platform evidence

The current priority is **independent benchmarks, worked examples and result validation**,
with reporting expansion deferred. The [benchmark campaign](../tests/benchmarks/README.md)
excludes GUI/display testing by explicit user direction. Acceptance concerns engine
results and numerical correctness; display features do not block it. The campaign
adds C22, Life, random walk, exact finite percolation/Ising, off-grid SD/DEVS
coupling, StupidModel computational versions 1–16, C21 event-contact ball,
and a bounded C17R hexagonal epidemic workload.
All seven native programs have passing normal and ASan/UBSan execution evidence with
identical validation results. StupidModel adds 43 cases/789,942 comparisons;
C21 adds eight cases/12,388 comparisons and rejects a deliberately quantized
event implementation. C17R adds 12,417 local cases, 30 spatial trajectories,
2,995,770 state/rate comparisons, and twelve independent ODE refinements.
Coarse timesteps fail its fast-spread accuracy gate; dt=1/64 and finer pass
without relaxing the fixed accuracy target. These do not close the external suites' remaining scope.
The [coverage inventory](../artifacts/benchmark-c17-coverage.json)
records 49 requested workload entries: 35 have passing execution evidence within
their declared scope; 14 have no adapter/data execution yet. Several passing
entries retain explicit coverage gaps. This is not whole-catalog acceptance.

[Five worked cases](../examples/worked_validation/README.md)
cover T&M/fixed-fee pricing under spare/full capacity and a staffing-to-revenue
queue. They compare all sampled outputs with exact rational calculations or a
hand-scheduled event ledger. A wrong T&M formula passes internal accounting but
fails the independent oracle, and a shifted completion is caught despite unchanged
final revenue. No simulation-engine changes were needed; checkpoint 36 records
local evidence and the scope.

The latest full normal regression passes **282/282 tests** (546.46 seconds).
The first campaign run exposed three XMILE failures: generic observation validation
rejected valid negative source-clock times. That restriction is fixed; finite-value,
name and uniqueness validation remain enforced. Seven affected sanitizer tests
pass (77.75 seconds), including imported models and CSV/Arrow publication.
The latest **full sanitizer** run remains **231/231 tests** (2439.76 seconds) at
an earlier source checkpoint; no fresh full sanitizer result is claimed. Later
and affected changes have focused sanitizer coverage, including 20 agent-stock
checks, 47 interpreter/runtime regressions, and eight experiment/design checks.
The Arrow-enabled suite now has **282 tests** (277 in the CSV-only build), with the optional Python binding enabled. Scenario-design reports and trajectories
match between normal/sanitizer builds; the parallel runner also passes ThreadSanitizer.
IR schema conformance now passes **55 valid fixtures plus 13 interaction and
six generator cases, 412 structural invalid cases and 248 semantic invalid cases**;
all three data-binding shapes also pass their schema/CLI contracts. The experiment
schema has separate design validation. These are local results; remote CI has not run.
The latest benchmark increment changes no production headers/runtime sources;
the full 282-test result above is retained from checkpoint 37, not reported as a
new full rerun. Seven relevant primitive tests were rerun and pass in both builds
(0.94 seconds normal, 4.96 seconds sanitized).
Checkpoint 39 adds adapter-only C17R execution in both builds; it reuses the
prior six adapters and the full 282-test normal baseline as historical evidence.
No new full CTest execution or production-runtime change is claimed.
The [current session log](SESSION_2026_09_28.md) records checkpoint 39 and earlier evidence;
[earlier checkpoints](SESSION_2026_09_26.md) retain their original evidence.

The latest functional increment is a [working synthetic Ankura preview](../artifacts/pilot-diagrams/results/report.md):
two practices, five years and all sixteen intervention combinations through the
standalone SD runtime. Typed ABM now composes parameter tables with population
initialization; SD now supports unit-checked auxiliary DAGs and pure MIN/MAX/
IF_POSITIVE functions. The pilot matches the separate Phase A implementation on
24,960 monthly/yearly values, sixteen NPV/payback results and 1,920 accounting/
capacity checks. Zero-demand/capacity controls and fee-model comparisons pass.
A 1,000-scenario workload has 732,000 observations per practice, identical at
1/8/64 threads; these repeat deterministic intervention combinations, not
independent Monte Carlo draws.

Nineteen affected tests pass in normal and ASan/UBSan builds; sixteen pass in
the CSV-only build. Expanded auxiliary coverage subsequently passes all three.
These are focused results, not new full-suite acceptance. The saved preview
includes annual metrics, native observations, provenance and replayable input
bundles. CLI and Python trajectories match exactly. Inputs are synthetic; real
data calibration, uncertainty modeling and broader M7 validation/explain/viz/report
remain open. M6 and M8 are not accepted. The [M6 audit](M6_ACCEPTANCE.md) records broader
binding and cross-platform gaps; reporting expansion is deferred at the user’s request. Current priority is worked
examples with independently calculated answers, followed by workforce/deal integration. Prior fuzz results remain historical: current changes
have not completed a fresh timed campaign. The root editable package was removed
from the development environment to prevent its import hook from shadowing
explicit build-directory extensions; use the preview runner's documented
`--package build-arrow/python` option.

[Native declared validation](DECLARED_VALIDATION.md) now provides `fathom check`
for standalone SD conservation, bounds, monotonicity and arithmetic assertions.
The pilot passes 25,184 declared rule evaluations with validation/run input and
result identities equal. `run --require-check` refuses publication unless checks
pass on the captured model and exact requested experiment. Reports distinguish
sampled behavior from unassessed M7 requirements. The
[updated review artifact](../artifacts/pilot-diagrams/results/report.md) retains
per-practice validation reports and replayable bundles. The 78-check validation
contract and unchanged pilot reference/API comparisons pass. Seventeen affected
normal/sanitizer tests and fifteen CSV-only tests pass at the initial checkpoint;
a final tolerance-overflow correction passes the affected validation/model/pilot
checks again, including three sanitizer checks. Checkpoint 33 records the distinct
runs; no new full-suite acceptance is claimed.

[Verified SD explanations](EXPLANATIONS.md) now reconstruct unclipped Euler/RK4
flow accounting from a file-backed run after exact input/build/result verification.
Passive integral tracking must leave original observations bit-identical. JSON
and self-contained HTML retain contribution signs, residuals, dependency values,
parameter overrides and source hashes. All eight saved pilot explanation pages
match independent annual summaries. Focused regressions pass 10/10 normal, 10/10 ASan/UBSan and 9/9 CSV-only;
checkpoint 34 records the evidence and scope. Browser visual review was blocked
by local-file URL policy; static HTML/escaping checks pass. Clipped-flow/bridge
attribution, hybrid diagrams/general report and M7/M8 acceptance remain open.


[Native SD structure diagrams](VISUALIZATIONS.md) now provide stock/flow and
output-focused dependency views in DOT, rendered SVG and Mermaid, plus schema-
validated graph JSON and a local review page. Every view carries the model hash;
optional manifest linkage verifies exact inputs/build and explicitly does not
claim result verification. The pilot links both practice diagrams and retains
source receipts. The final 29-case visualization contract passes normal,
ASan/UBSan and CSV-only builds; seven related normal/sanitizer regressions and
seven CSV-only regressions pass in separate runs. Checkpoint 35 records a corrected
invalid test fixture and the final reruns. Offline Bass and pilot layouts were
inspected; the large profit dependency view needs SVG zoom. No new full-suite,
remote CI, timed-fuzz, HTML browser review or M7/M8 acceptance is claimed.

The [Arrow/Parquet output increment](RUNTIME_OUTPUTS.md) passes 31 focused runtime,
experiment and CLI checks in normal and ASan/UBSan builds, plus ten CSV-only
runtime checks. Its cross-reader contract verifies 622,546 rows at 1/8/64 threads,
finite float64 bit preservation, typed schemas and failure-safe publication.
The prebuilt Arrow/Parquet SDK itself is not sanitizer-instrumented. These focused
results do not constitute a new complete-suite regression.

The [native data snapshot foundation](RUNTIME_DATA.md) adds exact local-file schema
validation, unique-key sorting, typed lookup and file/canonical SHA-256 hashes.
Its independent Python oracle checks 55 valid tables and 62,037 typed cells;
37 invalid cases cover the failure contracts. SHA-256 passes four known-answer
vectors and 141 binary patterns in seven update layouts, checked against hashlib.
The three new checks pass in normal, ASan/UBSan and CSV-only builds (the latter
checks that readers are unavailable without Arrow). The three native binding adapters and standalone SD parameter-table IR are now
implemented; broader declarative integration remains open. No new complete-suite regression is claimed.

[Transactional population initialization](POPULATION_DATA_BINDING.md) now maps
validated rows into native typed agents and returns source-key/hash receipts.
Four focused binding/data/ABM checks pass in normal and ASan/UBSan builds.
Independent source-row comparison covers 9,252 agents and 55,512 fields across
36 formats/order configurations, plus nine invalid bindings. Native checks add
120 mapping permutations, 48-bit ID limits, topology, rollback and ABM execution
at 1/8/32 threads. Declarative typed ABM population binding is now implemented.

[Native series and parameter inputs](DATA_INPUT_BINDINGS.md) now pass five focused
data/binding checks in normal and sanitizer builds. Independent references check
7,968 series values (maximum gap 1.78e-15), 696 keyed parameter values and 18 invalid
requests. Real SD trajectories agree with an exact grid-sum calculation and across
1/8/32 threads. All three M6 binding kinds now have native consumers; standalone SD parameter-table/series and typed ABM population IR are now
implemented. General binding composition remains open.

[Declarative parameter tables](PARAMETER_DATA_IR.md) now connect local data to
standalone SD model defaults with typed keys, dimensional checks, source receipts
and scenario override precedence. The increment passes 28 affected checks in
normal and ASan/UBSan builds, plus 27 CSV-only checks. Its public contract covers
63 CLI cases; native tests retain source snapshots over 192 trajectories at
1/8/32 threads. The existing schema suite also passes after the extension. This
is focused evidence, not a new full-suite run. General binding composition remains open.

[Declarative exogenous series](SERIES_DATA_IR.md) now supply unit-checked
`series_id(t)` functions at actual Euler/RK4 evaluation times, with hold/linear
interpolation, endpoint holding, owned snapshots and source hashes. The public
contract passes 103 CLI checks and 4,320 independent rational comparisons across
24 format/policy/integrator/grid configurations, with maximum gap 3.55e-15.
Native ownership and override checks cover 192 trajectories at 1/8/32 threads.
Thirty affected tests pass normal and ASan/UBSan; 29 pass in the CSV-only build.
These are focused regressions; M6 and the full-suite acceptance remain open.

[Declarative population initialization](POPULATION_DATA_IR.md) now maps required
local tables into sync/async typed ABM populations with deterministic IDs, exact
field conversions, dimensional checks and source-key/hash receipts. Twelve existing
ABM fixtures produce 7,872 identical observations over 72 format/order configurations;
the public contract passes 118 CLI checks. Native checks add a hand-computed phase
recurrence and 192 trajectories at 1/8/32 threads after source replacement. All
32 affected tests pass in normal and ASan/UBSan builds; 31 pass in CSV-only builds.
All three M6 input kinds now have bounded declarative support. General binding
composition, optional fallbacks and cross-platform package qualification remain open;
the scripting interfaces are implemented below.

[Run manifests and strict local replay](RUN_MANIFESTS.md) now record captured
model/experiment identities, binding hashes, expanded scenarios/effective parameters,
seed, build policy and ordered IEEE-754 numeric identity. Explicit sidecars and
`fathom replay` pass 134 CLI checks over 19 manifests in Arrow-enabled builds and
67 checks over ten manifests without Arrow. An independent hashlib oracle checks
32 numeric identities / 768 adversarial float values plus an empty result.
Version 0.2 manifest-backed observations now carry manifest IDs and effective
scenario parameters; Arrow/Parquet embed the full manifest, including for empty
tables. Replay preserves original provenance across format/path/thread changes;
version 0.1 replay retains its original result schema. Nine native artifacts are
independently decoded, including signed-zero parameters, extreme float bits and
empty trajectories. Native tests pass 97 rejection/publication controls (33 CSV-only).
All 40 affected checks pass normal (40.08 seconds), ASan/UBSan (116.15 seconds) and
CSV-only builds (11.06 seconds). The prebuilt Arrow SDK is not sanitizer-instrumented.
The manifest schema validates emitted manifests. Replay requires original local inputs and matching
build policy; only thread/output settings may change.
Full provenance acceptance remains open; file-backed bundles are implemented below. No new full-suite
regression or remote CI is claimed.

[`verify-results`](RUN_MANIFESTS.md#verify-a-saved-artifact) now checks schema 0.2
CSV/Arrow/Parquet artifacts against an explicit manifest without re-execution.
Arrow/Parquet also support `verify-results results.parquet --embedded`, using
their own manifest from the same decoded table without a sidecar. Verdicts name
the manifest source. The CLI applies the same manifest-envelope validation in both modes.
It validates numeric bits, schema, canonical row lineage, embedded provenance and
the complete trajectory catalog, including empty trajectories. The new contract
passes 226 CLI checks with Arrow (95 embedded) and 64 without it, including independently encoded
65,539-row artifacts, corruption and unchanged-input checks. Verification passes
after source deletion and across the local normal/sanitizer/CSV-only builds;
strict replay still rejects build drift. This is decoded artifact integrity, not
an accuracy or authenticity verdict. A consistently resealed artifact can pass
embedded verification while failing against a separately retained sidecar.
Legacy verification and embedded-manifest replay remain open. The C ABI foundation supporting the Python interface is now implemented below.

The [C ABI 1 foundation](C_API.md) builds `libankurafathom` with fourteen public C
functions: JSON/file loading, single/ensemble runs, independent model/result
ownership, ordered observations, numeric identity and thread-local diagnostics.
A C11 caller checks the Euler recurrence, lifetime rules, worker failure, unchanged
failure outputs, concurrency and truncated diagnostics. A second C-compiled consumer
passes 79 comparisons / 10,989 observations against CLI bits and independent hashlib
in Arrow-enabled builds (55 / 9,923 without Arrow), at 1/8/32 workers and across
explicit/grid/LHS/Sobol scenarios. The shared JSON/experiment loaders pass the
existing IR schema contract. CPU Arrow C Stream export is now implemented: streams
own schema 0.1 observations independently of the results, and returned arrays/schemas
outlive the stream. A C11 consumer verifies 80,015 exact rows, multiple batches,
release order, destination preservation and independent exports. The SDK/PyArrow
contract passes 43 roundtrips / 889,541 observations across SD, DES, ABM, hybrid,
data bindings, scenario designs, 1/8/32 workers and finite binary64 edge values.
The two stream checks pass normally (11.88 seconds) and with ASan/UBSan (36.29
seconds); six existing C ABI/output regressions also pass in each build. All seven
CSV-only affected checks pass (3.08 seconds), including explicit export-unavailable
behavior. The SDK itself remains uninstrumented; no new full-suite run is claimed.
M6 remains in progress. The nanobind Python package and execution callbacks are now implemented below;
broader platform release qualification remains open. Allocation-failure translation is implemented but
has not been fault-injection tested in this checkpoint.

The optional [nanobind Python package](PYTHON_API.md) now exposes `Model.from_json`,
immutable `Experiment` specifications, `run() -> pyarrow.Table` and loader-backed
`lint()`, using only the C ABI. Native work releases the GIL; result ownership moves
through Arrow capsules. Structured exceptions preserve diagnostics across later
calls and concurrent threads. The Python contract passes 112 C-ABI comparisons /
82,931 exact observations and 64 controls in normal and ASan/UBSan builds; CSV-only
Python passes 34 controls, including structured unavailable-export errors. Final
boundary regressions (C ABI, Arrow C Stream and Python) pass 3/3 normal (7.24 seconds),
3/3 ASan/UBSan (23.52 seconds) and 3/3 CSV-only (2.44 seconds). Python and Arrow SDKs
are prebuilt and not instrumented; nanobind/the extension use the sanitizer flags.
This adds build-directory scripting support, not a distributable wheel or new
scientific acceptance. Local installation and packaging are now implemented below.
M6 remains open.

[Local distribution packaging](PACKAGING.md) now produces a production-only source
archive and a platform-specific wheel containing the Python extension and private
Fathom C library. Relative loader paths resolve the pinned sibling PyArrow package;
licenses/notices and dependency metadata are included. A separate CMake install
provides the native C SDK/CLI with `AnkuraFathom::c_api`. An offline fresh-environment
install, moved runtime packages, and a moved CSV-only SDK prefix pass 112 comparisons /
82,931 exact Python observations plus 64 controls, the installed C11 contract and
11 analytic CLI observations. Local wheel: CPython 3.14/macOS 26/arm64. The package
is not published or qualified for other platforms. Final boundary checks pass 3/3
normal (13.56 seconds), 3/3 ASan/UBSan (29.88 seconds) and 3/3 CSV-only (2.63 seconds).
Packaging acceptance runs separately from CTest; that checkpoint had 269/266 checks.
C/Python run-provenance and manifest access are now implemented below.

[API run provenance](API_PROVENANCE.md) now exposes a stable C manifest getter and
schema-0.2 Arrow export; Python selects it with `run(..., provenance=True)`. Receipts
retain captured input/data identities, effective scenarios/seed, build policy and
the ordered numeric hash. Version 0.3 explicitly represents memory inputs/output;
legacy file manifests and default schema-0.1 exports retain their contracts. New
checks pass 72 receipts / 96 saved-artifact verifications / 14 controls in normal
and ASan/UBSan builds, plus 30 receipts / nine controls without Arrow. Existing
C/Python/output/provenance regressions also pass: all 14 affected Arrow checks and
13 CSV-only checks are covered, with the corrected checksum test rerun separately.
JSON schema validation is included in Arrow builds. Preserved source snapshots,
invalid resealed envelopes, tampered results and unsupported replay are checked.
[C/Python execution callbacks](API_EXECUTION_CALLBACKS.md) now expose canonical
trajectory progress on the calling thread and cancellation without partial results.
The C ABI remains version 1 with one additive entry point. Python preserves callback
exception objects and tracebacks after workers join; nested and concurrent calls
keep their own contexts and final diagnostics. Normal and sanitizer checks cover
36 mode/parity cases and 59 controls; CSV-only covers 24 cases and the same 59
controls. All eight affected normal/sanitizer checks and seven CSV-only checks pass
(the two corrected normal checks were rerun separately). Installed-wheel callback
checks and relocated native SDK checks also pass. The C11 consumer additionally exercises every cancellation boundary for
single/ensemble runs at 1/8/32 workers. Cancellation remains between trajectories;
active trajectories finish before return. No latency guarantee is claimed.

[Default CLI file manifests](RUN_MANIFESTS.md) now write `<output>.manifest.json`
and schema-0.2 lineage for `run --out PATH`. Explicit `--manifest` changes the
location; `--no-manifest` selects legacy schemas. Stdout and replay retain their
prior contracts. Existing sidecar entries, including dangling symlinks, reject
before output replacement. Default and explicit outputs use the same alias
protection and staged publication; the receipt commits last, with no claim of
a two-file atomic transaction. Legacy schema/determinism tests use explicit opt-out.
Final affected checks pass 14/14 normal (61.04 seconds), 14/14 ASan/UBSan
(200.89 seconds), and 13/13 CSV-only (9.62 seconds). The new contract covers 54
manifested runs / 352 CLI checks / 25 controls with Arrow, and 12 runs / 100 checks /
26 controls without it. Installed SDK/CLI default-sidecar verification and replay,
plus wheel numeric/callback/provenance regressions, also pass. No full-suite or
remote-CI acceptance is claimed.
[Portable replay bundles](PORTABLE_REPLAY.md) now capture byte-identical model,
experiment and data files into a staged directory. Replay after relocation uses
fixed local members, preserves the original receipt/ID, and validates raw/canonical
identities, effective scenarios, strict build policy and the ordered numeric hash.
It rejects unexpected members, symlinks, special files, oversized inputs and outputs
inside/aliased to its input bundle. File-backed manifest versions 0.1/0.2 are
supported; API memory receipts and embedded-manifest replay remain outside this
increment. Bundle creation captures inputs without claiming a successful numeric
replay. The bundle contract passes 35 relocated runs / 306 CLI checks / 42 controls
with Arrow and 15 runs / 134 checks / 34 controls CSV-only. All 13 selected
regressions pass normally (66.92 seconds), under ASan/UBSan (214.38 seconds), and
CSV-only (10.31 seconds). This is focused coverage, not a new full-suite run. The installed and
relocated native CLI also replays a moved bundle after deleting its original
inputs; installed Python numeric/callback/provenance checks remain passing.
Next: M6 acceptance audit and remaining data-binding gaps. Broader binary
qualification remains open; M6 is not complete.

Earlier increment: [declarative continuous agents and SD](AGENT_STOCK_SD_IR.md)
adds unit-checked per-agent stock expressions, five filtered aggregates and held
scalar inputs to SD, with independent agent/SD clocks. Twenty-four generated models
pass 2,728 exact-rational scalar comparisons (maximum gap 1.78e-15), and six
experiment trajectories pass 300 comparisons. Normal/sanitizer reports and CSV
hashes agree exactly; fifteen corruption controls reject altered evidence.

The [M1 kernel report](KERNEL_CONFORMANCE.md) records 153 differential cases against pinned adevs. M4 foundations include 7,504 exact Mesa/NetworkX neighborhood comparisons, 4,212 interaction observations, independent statechart/publication/lifecycle/network histories, and 51 graph-generator gates over 4,608 runs per engine. [M4 acceptance](M4_ACCEPTANCE.md) collects the evidence and supported scope.

Previous increment: [typed agent continuous stocks](HYBRID_AGENT_STOCKS.md) pass
all eight focused checks in both builds (four new and four existing regressions).
Independent closed forms validate 108 snapshots and 720 agent values, with three
first-order refinement curves; 576 homogeneous snapshots also agree with pure SD.
The maximum conservation gap is 1.43e-14. Normal/sanitizer trajectories and reports
are byte-identical. The subsequent DEVS publisher and bounded declarative mode are recorded below;
general lifecycle/pulse wiring remains open.

Previous increment: [typed snapshot aggregates](HYBRID_TYPED_AGGREGATE.md) pass all
ten focused checks in both builds (five new and five existing regressions).
Five reducers feed latched SD flow inputs through cloneable DEVS endpoints;
1,008 snapshots and 10,080 scalar comparisons agree with an exact-rational oracle
across 24 cases and both declaration orders, with maximum gap 6.67e-16. Normal and
sanitizer trajectories/reports are byte-identical. Native ABM result publication and bounded IR bindings are now implemented below.

Previous increment: [agent-stock DEVS publication](HYBRID_AGENT_STOCKS_ATOMIC.md)
passes nine focused checks in normal and sanitizer session builds. Its 144
configurations give 3,024 exact-rational snapshots, with maximum gap 1.43e-14
and exact clocks/revisions. The full starting-checkpoint regression has passed 194/194 in both builds.

All five canonical ABM families pass their declared suites: [wealth](ABM_WEALTH.md), [Schelling](ABM_SCHELLING.md), [Boids](ABM_BOIDS.md), [Sugarscape-lite](ABM_SUGARSCAPE.md) and [synchronous SIR](ABM_SIR.md). Each contributes 32 frozen Mesa distribution gates over 2,048 runs per engine, with separate exact-state/conservation evidence. [Continuous-time SIR](ABM_SIR_ASYNC.md) adds another 32 Mesa gates, 864 addressed agent states and 356 event observations (maximum local event-time error zero), plus three high-address golden events. Its sync/async convergence evidence includes six exact joint-state refinement curves, 36 native joint-law gates and six finest-sync/async comparisons over 18,432 native trajectories. Error-halving ratios range from .492 to .508. Pinned Mesa/SciPy regeneration passes; the largest finest-step joint-distribution error is .015660.

The [M5 well-mixed SIR/SD gate](HYBRID_SIR_MEAN_FIELD.md) now passes four frozen population curves at N=40/160/640 with 3,072 trajectories per stochastic engine. Evidence includes 168 independent count-CTMC comparisons, 84 finest-N mean gates, four native RK4 checks, 96 exact complete-graph equivalence cases and 64 addressed count snapshots. Stored plots and numeric gaps separate trajectory spread, ensemble-mean error and Monte Carlo standard error. Pinned SciPy/count-CTMC regeneration passes; [M5 acceptance tracking](M5_ACCEPTANCE.md) records SIR/Bass and DES fluid evidence and the remaining continuous agent stocks and bridge types.

**All 17 distinct focused tests pass in normal and ASan/UBSan builds** for the SIR increment: five new M5 checks, seven existing graph-SIR/convergence checks and five population/statechart/network/RNG regressions. Native mean-field generation takes about 269/822 seconds. Normal/sanitizer trajectories are byte-identical; both existing graph-SIR output files also match their pre-change hashes. This is focused validation of the new 175-test configuration, not a new full-suite run. No IR/schema contracts changed.

The [Bass mean-field suite](HYBRID_BASS_MEAN_FIELD.md) adds six population curves across three cases, two time steps and N=40/160/640, with 4,608 runs per stochastic engine. All 90 independent binomial comparisons, 30 finest-N mean checks, 30 analytic mean/variance gates and three five-step Euler/RK4 refinement checks pass. Independent Philox recurrence matches 72 count snapshots, and high-address hand tests include three golden states. Population error is scored against discrete SD; continuous-SD gaps and numerical bias are recorded separately. Pinned NumPy/SciPy regeneration passes, and plots, numeric gaps and provenance are stored.

**All nine distinct focused Bass tests pass in normal and ASan/UBSan builds**: five new checks plus existing Bass, synchronous/typed population and Philox regressions. Native generation takes about 285/905 seconds. Normal/sanitizer trajectories and reports agree exactly; the standalone example also agrees. High-address golden and clock-overflow hand checks were rebuilt/rerun in both builds. The 180-test configuration has not had a new full-suite run; the previous SIR focused evidence and 170-test full checkpoint remain separate. No shared engine implementation or IR/schema changed in this increment.

The [DES→SD fluid suite](HYBRID_DES_FLUID_LIMIT.md) adds four queue regimes at scales N=8/32/128, with 3,072 coupled native and independent birth/death trajectories per engine. All 180 distribution comparisons, four scaling curves and four reflected-SD checks pass in both builds. All 18,432 native observations preserve exact source/server/stock/queue accounting; 48 snapshots match an independent addressed FIFO recurrence. Hand checks cover all 120 declaration orders, confluence, observation density, step-budget retry and failed-completion rollback. Figures and numeric gaps separate finite-population boundary effects from sampling and numerical error. Independent reference regeneration matches exactly.

**All ten distinct focused fluid tests pass in normal and ASan/UBSan builds**, combining five new checks with five SD/server/process/clock/pulse regressions. Native generation takes about 204/897 seconds; complete trajectory bytes and scored reports agree exactly across builds. The failed-completion rollback check was rebuilt/rerun, and both examples agree. The suite is configured for 185 tests; the last full-suite result remains 170/170 per build. This increment adds a native composition and evidence, with no changes to existing engines or IR/schema contracts.

DES evidence includes **444 native/SimPy/Ciw statistical pair gates**, **222 analytical gates across 672 preplanned native replications**, 96 exact discipline traces (1,382 service and 922 rejection records), and typed queue/delay and seize/delay/release comparisons. [Typed declarative graphs](DES_TYPED_IR.md) additionally pass **120 pinned cases and 76,576 observations** with maximum absolute difference about **2.84e-14**. Hand tests cover runtime field schemas, shared pools across entity types, multiway selection, expression priorities, generated arrivals/service, scheduled capacity, replay, units, graph ownership, and rollback. Current schema/loader counts are recorded at the top of this file.

M1 and the declared M3 DES and M4 ABM CPU cores are locally accepted. M2's source-history work remains partial. M4 has native spatial/network/message foundations, typed sync/async populations, flat statecharts, DEVS/declarative integration and snapshot-based spatial/network queries with validated movement, transactional topic handlers/replies, phase/statechart publication outboxes, scheduled birth/retirement batches, phase/transition lifecycle actions with newborn chart initialization and allocation limits, population-owned networks with scheduled edge edits, declarative ER/WS/BA initialization, and validated native Boltzmann wealth, Schelling, Boids, Sugarscape-lite and synchronous network SIR models; The declared M4 acceptance gates pass in both builds, including finite-graph sync/async SIR convergence. [M4 acceptance](M4_ACCEPTANCE.md) records the supported CPU scope and extensions. M5 is in progress, with its declared SIR/Bass mean-field, DES fluid and native Euler agent-stock gates passed; M6–M8 remain open.

| Area | Current state | Required next gate |
|---|---|---|
| Pinned adevs reference | Builds; upstream schedule test passes; 153 differential cases cover the original five fixed/64 graph cases plus 48 DEVStone, 30 static conformance, two flat dynamic cases, and four root/nested hierarchy cases; new fixtures compare full canonical step traces | Extend the oracle alongside future kernel features |
| Parallel DEVS event kernel | Atomic coupling, confluence, external/root injection, same-step root passthrough, historical boundary results, zero-time guard, and compiled nested add/remove/disconnect/reconnect with staged route validation; 81 static fixtures and one nested dynamic scenario agree across nested native, independently flat native, and adevs; subtree removal cancels pending work, future root inputs reroute at delivery, surviving clocks persist; checked steps restore cloneable atomics and queued root events on failure; 48-case allocation requests/bytes baseline and counter integrity tests recorded | M1 accepted; follow-on rollback for non-cloneable custom atomics; multi-operation structural transactions and model-requested structural edits remain outside the current API |
| SD library | 56 new analytic convergence trajectories, 16 delay boundary trajectories and 45 discrete logistic checks pass; stocks/flows, bounded Euler clipping with explicit priority and conservation, Euler/midpoint/RK4, tables, and bounded Euler orders 1–255 and fixed whole-tick delays pass analytic/conservation tests; six independent nonlinear/signed/time-dependent reference models pass 72 trajectories, expected convergence orders, invariants, exact discrete oscillator checks, and seven stage-failure checks; pinned SciPy regeneration is configured in CI; STEP/PULSE/RAMP, explicit smoothing, and a scalar Euler/RK4 XMILE importer pass boundary/analytic checks; lookup/stateful XMILE mappings, variable-duration cascades, fixed lags, and bounded general-order source mappings, and opt-in signed flows now have 43,598 pinned historical/PySD stock comparisons including reordered variants, plus 158 auxiliary and 26 flow observations; absolute standalone start times, bounded vendor metadata and sign-filtered flow references without source-stock limits reproduce the complete fixed-delay source trajectory; unit-checked expression outputs allow standalone models without stocks; RK4 source mappings pass 1,272 SciPy-scored observations with fourth-order convergence and 132 upstream closed-form checks; explicit variable-duration DELAYN cascade imports add 1,386 independent expanded-stock PySD observations; grid-aligned XMILE STEP/RAMP/PULSE have separate argument/unit contracts and complete hand trajectories; explicit second-order history adds 101 historical Vensim observations and 1,386 PySD comparisons with independent conservation checks; explicit off-grid next-tick inputs add 19,584 exact-rational rate/stock observations, including sub-dt pulse quantities | Other history orders need authoritative evidence after PySD conservation failures; vendor-specific off-grid policies; delay-output-dependent duration semantics; see [M2 acceptance review](M2_ACCEPTANCE.md); [eligible-corpus gate met](XMILE_COVERAGE.md) (8/8 imported = 100%; 7/8 historical-compatible = 87.5%; full inventory 8/67 imported, 49 unassessable) |
| DES library | FIFO single-/multi-server stations, optional finite waiting capacity, LIFO, and deterministic non-preemptive priority in the multi-server process station, rejection records/counts, standalone acyclic routing/merging/backup/discard paths with publication confluence and rollback/retry, 32 graph declaration permutations, 24 bag permutations and 64 independent integer-clock traces, a resizable FIFO/LIFO/priority seize/release pool with explicit preemption rejection, and a source→one-or-more-servers→completion sink path with fixed or addressed-exponential schedules pass exact single-/two-stage schedules, confluence, conservation, cycle-time, queue-area, utilization, rollback, capacity invariants, and seeded M/M/1 and M/M/2 stationary oracles; [M/G/1 and finite-buffer validation](DES_STATISTICAL_VALIDATION.md) passes 60 analytical gates across eight configurations and 336 preplanned replications, cohort waits, occupancy probabilities, exact conservation and time-integral identities; the workload exposed and fixed fractional-clock drift in DES process components; [independent station service](DES_SERVICE.md) adds addressed exponential policies in C++/DES/hybrid, and [Jackson validation](DES_JACKSON_VALIDATION.md) passes 80 marginal/joint analytical gates across 216 preplanned runs of two three-station tandem networks; [pinned SimPy/Ciw evidence](DES_ENGINE_ORACLES.md) adds 444 native/reference gates across all twelve workloads and checks 876,921 service records and 141,345 rejection records between the independent engines ; [probability routing](DES_PROBABILITY.md) adds 82 analytical branching gates across 120 replications | M3 CPU core locally accepted; loops/revisits, general mutable entities, and typed hybrid integration remain explicit extensions |
| ABM library | Synchronous phased population passes Jacobi and stable-ID tests; a seeded homogeneous adoption IR exposes active/adopted metrics; grid and 1–3D continuous spaces pass exhaustive neighborhood, movement, wrapping, and snapshot tests; CSR networks, addressed ER/WS/BA generators and bounded transactional message topics pass native conformance tests; 26 pinned Mesa/NetworkX spatial/network cases match 7,504 exact neighborhood queries; typed columnar phases and async timers pass schema, lifecycle, independent calendar and whole-timestamp rollback checks; flat statecharts pass message/timeout/guard traces and 288 exact addressed rate firings; the population DEVS wrapper adds input/timer confluence, committed result publication and checked rollback; `mode: abm` exposes typed sync phases/async statecharts, guarded assignments, scheduled messages and numeric outputs, with 297 independent CLI rate observations in normal/sanitizer builds; declarative grid/continuous/network/population queries drive phases and guards/actions, with whole-batch movement validation and 4,212 Mesa/NetworkX trajectory observations; population topic handlers add direct/broadcast delivery, guarded unit-checked assignments, reply rounds, bounded cycles, transactional message IDs and checked publication retry, with 840 independent scheduler observations; phase and selected-transition publishers add transactional outboxes, complete-tick delivery and bounded same-time timer/message feedback, checked against 400 independent observations; scheduled birth/retirement batches preserve stable IDs, cancel future timers, initialize newborn charts and update live membership, checked against 480 independent observations; phase/transition lifecycle actions support conditional reproduction and self-retirement with parent-snapshot assignments and bounded allocation, checked against 669 independent observations; value-owned topology follows live membership and supports transactional scheduled edge batches, custom validation and query snapshots, checked against 873 independent observations; declarative graph generators pass 2,808 exact addressed observations and 51 predeclared statistical/analytical gates against pinned NetworkX; the native sequential wealth workload passes 32 gates against pinned Mesa with exact conservation and transactional sweep rollback; sequential Schelling passes 32 additional Mesa gates with exact population/group/occupancy invariants and rollback; synchronous Boids passes 32 more Mesa gates and 2,880 paired values with periodic/reflection, finite-state and speed-cap checks; sequential Sugarscape-lite passes 32 further Mesa gates with exact resource/population conservation, live-ID retirement and complete-sweep rollback; synchronous network SIR passes 32 additional Mesa gates and 864 exact states with fixed contacts, irreversible states and tick rollback; native asynchronous SIR adds 32 Mesa gates, exact event replay and six full-joint-state refinement curves plus 42 native convergence gates | M4 CPU core locally accepted; topic-handler lifecycle and behavior-generated edge edits remain extensions; hierarchical states and changing hazards remain unsupported |
| Hybrid bridges | DES completion→SD pulse, bounded tick-sampled SD stock→DES arrival, and SD-driven piecewise-constant Poisson rate form tested closed loops; C++ and declarative ABM adoption aggregates publish active-agent changes to SD; a C++ population→pool capacity bridge and ownership broker pass dynamic resize, deterministic multi-agent assignment, safe-departure preflight, allocation release, overflow rollback, and static-pool grant-trace equality; a staged agent-pool owner commits population, synchronous ABM phases, pool, broker, and time together, with a single-atomic DEVS wrapper and rollback tests; a declarative agent-pool mode schedules transactions and named synchronous capacity phases, with unit checks, shared population snapshots, staged allocation reads, and hand/C++ trajectory oracles; optional engagement delivery commits completion releases, workforce changes, FIFO grants, its calendar, and statistics together, with hand-schedule and rollback/retry tests; delivery integrates capacity, allocations, free units, queue, service, and active population over event intervals, with capacity-weighted utilization, read-only horizon projections, overflow rejection, and observation-density checks; direct C++ composition, exact cumulative-hazard oracle, on-/off-grid order, stochastic replay, conservation, fractional tick alignment, and a fixed-N Bass ABM/SD check pass; four well-mixed SIR population curves and six Bass curves now have independent stochastic/continuous references, separate numerical refinement and stored plots/numeric gaps; four DES fluid regimes now preserve exact pulse-stock/queue accounting and pass independent birth/death comparisons; [typed agent continuous stocks](HYBRID_AGENT_STOCKS.md) now integrate agent/global transfers and aggregate flows with whole-step rollback, 108 independent analytical snapshots, three refinement curves and 576 pure-SD equivalence snapshots; [typed snapshot aggregates](HYBRID_TYPED_AGGREGATE.md) add five reducers and scalar-driven SD with 1,008 exact-rational snapshots across two declaration orders | Agent-stock lifecycle/pulse wiring, general aggregate graphs and typed rate/lifecycle/entity-agent bridges and broader staffing composition; see [M5 tracking](M5_ACCEPTANCE.md) |
| RNG | Philox4×32-10 passes three Random123 vectors; draw-address layout, uniform/Bernoulli/exponential/categorical transforms, and stochastic DES schedule replay checked | Stream-name assignment, broader distributions, distributional queue oracles |
| Declarative IR and lint | Version 0.1 SD, acyclic multi-stage DES with binary priority/probability routing and rejection/discard paths, bounded two-way linear DES/SD hybrid, homogeneous ABM–SD adoption, and scheduled integer-capacity agent-pool subsets: JSON loader, closed known fields, stock/flow/table/delay and source/server/sink components, fixed and exponential arrivals, independent per-station exponential service with explicit stream ownership, finite station queues and scheduled integer priorities, rejection metrics, validated process path, exact event sampling, dimensional checks for SD and bridges, source/feedback ID and draw-stream disjointness, seeded agent adoption with parameter overrides, transactional capacity/queue/assignment observations and named arithmetic capacity phases with checked units and runtime results, plus fixed-duration engagement delivery, queue/start/completion observations, and cumulative time-weighted staffing/utilization outputs, structured errors, fixtures, CLI run/lint; CI checks Draft 2020-12 schema against loader on valid and invalid examples | Runtime JSON Schema enforcement, general typed hybrid graphs, topic-handler ABM lifecycle/behavior-generated graph edits, broader functions; typed DES process graphs and bounded typed ABM sync/async populations are implemented |
| Experiment runtime | Ordered threaded scenario × replication coordinator, cancellation/progress, parameter overrides, ordered observations and sample statistics; atomic CSV/Parquet/Arrow IPC output and in-memory Arrow tables; grid/LHS/Sobol designs; DES exponential source and station services use seed/scenario/replication draw addresses; default CLI file manifests, embedded Arrow/Parquet provenance, per-observation scenario lineage and strict local numeric replay | General binding composition, API-memory/embedded replay, cross-platform package qualification and general hybrid loading |
| Data foundation | Local CSV/Parquet/IPC snapshots, exact typed schemas and string domains, unique-key row order, immutable owned values, typed lookup and independent file/canonical SHA-256 identities; transactional native population initialization, hold/linear series sampling, keyed parameter maps and source receipts; declarative standalone SD parameter tables and hold/linear series functions with unit checks, owned snapshots and override precedence; typed ABM population-table initialization with canonical IDs and source-key receipts | General binding composition, URI sources, optional fallbacks and nullable schemas, decoded memory budgets and API-memory replay |
| Validation runtime | Analytic SD, DES, RNG, hybrid, and differential DEVS tests | Structural/behavioral `fathom check`, uncertainty decomposition and fit diagnostics |
| Synthetic economics reference | Runs; accounting and behavior tests pass | Reproduce it through the general runtime |

The staffed-delivery C++ component supports DEVS composition through `AgentPoolProcessAtomic`, with committed result messages, same-step completion/workforce confluence, deterministic merging of arrival commands, and deep-copy rollback. Its tests compare full staffing/completion traces against the standalone engine and cover downstream retry, publication confluence, and exact-clock regressions. The kernel provides optional absolute-deadline and timestamp-aware external-transition hooks; the ownership ledger and clocked SD preserve exact timestamps.

The `agent_pool_sd` declarative subset now connects staffed delivery to stock/flow SD through one constant-amount completion bridge. It supports checked DEVS execution, mixed staffing and stock outputs, parameter overrides, global component ID checks, and unit-checked endpoints. A fixture and tests verify every staffing observation against the standalone reference, exact hand-derived stock trajectories, off-grid and horizon publication, repeatability, incomplete work, and failure diagnostics. Schema conformance includes this mode. Feedback to staffing, variable per-engagement amounts, multiple bridges, and general graph composition remain later gates.

The [SD validation report](SD_VALIDATION.md) records the completed nonlinear-reference/convergence increment and the remaining M2 gates. M2 remains in progress.

The CPU reference runtime will be made correct before code generation, tensor execution, or Tenstorrent work begins.

Latest increment: the [typed ABM result adapter](HYBRID_TYPED_AGGREGATE.md) passes
five focused checks in both session builds. Its 336 exact snapshots cover all 24
four-component declaration orders with synchronous phases, asynchronous timers,
commands, births/retirements and checked publication retry. The aggregate interface
now accepts committed results from the existing typed ABM through a native adapter;
bounded declarative continuous-agent expressions are implemented; general graph bindings remain open.

Typed event pulses now pass [144 independent configurations](HYBRID_TYPED_PULSES.md),
3,024 snapshots and 12,096 scalar checks. All 22 focused regression tests pass in
normal/sanitizer builds; existing scalar-only trajectory hashes are unchanged.
Native payload projections and named pulse channels are implemented; declarative
pulse bindings and lifecycle/entity-agent/general graph composition remain open.

[DES-triggered lifecycle](HYBRID_LIFECYCLE_BRIDGE.md) now passes 8,064 exact scheduler
snapshots across 384 configurations. All 17 focused native/declarative regression
checks pass in normal/sanitizer builds. The bridge binds namespace/schema and
replay keys while the population retains authoritative IDs, membership and timers.

[Shared entity-agent ownership](HYBRID_ENTITY_AGENT.md) now passes 18,000 exact
scheduler snapshots across 720 statechart/resource/process configurations. All 19
focused checks pass in both builds. Dispatch snapshots are immutable process inputs;
the typed population remains authoritative for identity, fields and retirement.

[SD-driven typed arrivals and routing](HYBRID_SIGNAL_RATES.md) now pass 9,408 coupled
runs, 41,418 independent event comparisons and fourteen Poisson count gates. Eleven
focused checks pass in both builds; native outputs and reports match exactly.

Latest native increment: [typed workforce pools](HYBRID_TYPED_AGENT_POOL.md) own
population records, resource assignments and broker writeback as one transaction.
All 21,168 independent pool/aggregate/SD snapshots and 1,489,920 scalar checks agree
exactly across 432 configurations. Sixteen corruption controls reject; normal and
sanitizer reports/trajectories match. General graph bindings remain open.

Latest native increment: [dynamic agent stocks](HYBRID_DYNAMIC_AGENT_STOCKS.md) add
transactional lifecycle/pulse changes and discrete inventory receipts. Four clock
pairs across 192 configurations give 7,872 independent snapshots and 818,496 scalar
checks with zero observed gap. The full checkpoint-8 normal regression now passes
231/231 (705.18 seconds); its sanitizer run subsequently passed 231/231.

M6 is now in progress: [ordered CPU ensembles](RUNTIME_EXPERIMENTS.md) pass
native thread-count/isolation/cancellation gates and a 1,000-scenario CLI byte
comparison. Atomic CSV publication preserves existing files on failed runs. The
threaded native test also passes under ThreadSanitizer. Broader runtime work is open.

Earlier runtime increment: [scenario designs](RUNTIME_SCENARIO_DESIGNS.md) pass
510,964 coordinate comparisons, 1,156 stratification/mean gates and 15,570 analytic
CLI observations. M6 remains in progress. [Arrow/Parquet integration](RUNTIME_OUTPUTS.md)
now provides observation output with a CSV-only fallback. [Native data loading](RUNTIME_DATA.md)
now provides validated snapshots and content identity. General binding composition,
API-memory/embedded replay and cross-platform package qualification remain open.
