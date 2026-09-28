# XMILE import 0.11

`tools/xmile2ir.py` converts a bounded scalar XMILE subset into standalone SD
IR. It requires Python 3.10+ and no third-party Python packages at runtime.

```sh
python3 tools/xmile2ir.py tests/oracles/xmile/fixtures/stateful.xmile \
  --units metadata --out build/stateful.ir.json
./build/fathom lint build/stateful.ir.json
./build/fathom run build/stateful.ir.json --out build/stateful.csv
```

The converter writes the IR plus `build/stateful.ir.json.metadata.json` and
refuses to overwrite either. The sidecar records source SHA-256, importer
version, original variable names/units, flow policy, and the owner/function of
each generated delay component. By default, outputs contain stocks when present; models without stocks expose scalar auxiliaries as pure expression outputs. `--outputs all` exposes stocks, auxiliaries and flows when their applied values are representable; source-stock-limited flow outputs are rejected.

## Supported subset

- UTF-8 XMILE 1.0 in the OASIS or historical systemdynamics.org namespace;
  one scalar model with a stock or scalar auxiliary output.
- Fixed Euler or classical RK4 integration, finite start time, finite positive dt
  (including reciprocal notation), and 1–1,000,000 whole ticks. Omitted method
  selects Euler. RK4 currently excludes stateful delay/smoothing calls, source input functions and clipping directives.
- Arithmetic `+ - * /`, unary signs, parentheses, decimal/scientific literals,
  quoted names, and case-insensitive space/underscore name aliases. `TIME`
  resolves to the simulation clock. Built-in names cannot be redefined.
- Acyclic scalar auxiliary/flow equations; stock initial values are evaluated
  from acyclic initial dependencies. Every runtime flow reads shared pre-tick
  stock and delay state under Euler; RK4 flow equations read each stage snapshot.
- One source and/or destination stock per flow. An auxiliary explicitly named
  in a stock inflow/outflow is translated as a flow, supporting Vensim-exported
  stock-linked auxiliary rates. Its original kind and promoted ID remain in the
  metadata; the same rate has at most one source and destination. XMILE flows without clipping
  directives become signed IR flows (`non_negative: false`); negative rates
  reverse their contribution to the declared endpoints. Stock bounds and
  finite-value checks remain independent of flow sign.

## Bounded nonnegative clipping

Empty `<non_negative/>` on an Euler stock maps to `clip_outflows: true`,
`non_negative: true`, and an explicit `outflow_order` copied from its ordered
source `<outflow>` tags. On a flow (or stock-linked auxiliary rate), it maps
to `clip_negative: true` and `non_negative: true`.

This implements the [Stella-style Euler allocation contract](SEMANTICS.md#bounded-euler-clipping):
negative requested uniflows become zero, and outflows consume the stock plus
current incoming amounts in declared priority order. Shared flows use one
applied amount at both ends. Current upstream allocations can fund downstream
outflows during the same tick, while every requested rate reads pre-tick state.

Every flow incident to a clipped source stock must itself have the explicit
nonnegative directive. Cycles among clipped stocks, equations reading flows limited by a source
stock, tagged auxiliaries without stock endpoints, negative initial
clipped stocks, and RK4 clipping are rejected. These restrictions avoid silently
substituting an unconstrained rate where an equation expects the applied flow.
Native `non_negative` alone retains its existing fail-on-negative behavior.
The metadata records `clipping_policy: euler_current_inflows_priority_acyclic`.
This is a bounded execution contract, not general XMILE filter conformance.

## Lookup tables

Standalone named `gf` declarations and graphical functions embedded in flows
or auxiliaries lower to native table components. An embedded table transforms
its containing equation's result. An embedded table may also declare a distinct
name for explicit calls elsewhere. Names must be globally unambiguous.

Supported encodings are explicit `xpts/ypts`, custom single-character separators,
and evenly spaced x coordinates from `xscale` plus `ypts`. Points must be finite,
with at least two matching values and strictly increasing x coordinates with
finite spans. `xscale/yscale` display metadata does not clip table values.

`type="continuous"` (the default) maps to linear interpolation with endpoint
clamping; `type="extrapolate"` uses linear interpolation and linear extension
of the endpoint segments. The historical namespace's `discrete="false"` is
also recognized. Discrete interpolation, table references without definitions,
and unknown attributes are rejected. Lookup evaluation in initial values uses
the same declared policy.

## Stateful functions

Under Euler, case-insensitive `SMTH1`, `SMTH3`, `DELAY1`, and `DELAY3` calls accept
`(input, duration[, initial])`. Each syntactic call creates one explicit native
information or material delay component. Repeated references to its enclosing
variable share that state; distinct calls retain separate state, even if their
expressions are identical. Nested calls form explicit delay chains.

Every stage starts from the explicit initial output, or the input evaluated at
the source start time when initial is omitted. Defaults can read initial stock values,
lookups, and other independently initialized delays. Initialization cycles
fail. Runtime feedback through initialized state is allowed; algebraic cycles
without state still fail. Calls directly inside stock initializer equations
must first be declared in a separate auxiliary, so the lifetime of that state
is explicit.

Duration may depend on time, stocks, constants/auxiliaries, and lookups. It is
lowered to a native duration expression, checked at initialization, and evaluated
from the shared committed stock/time snapshot on every tick, including the final
observation. References to delay outputs, including through auxiliary aliases or
nested calls, are rejected; the importer does not solve implicit duration/output
algebraic loops. A duration must stay finite, positive, and cover at least one dt
per stage. A later violation fails the run rather than silently changing order.
Material delays require nonnegative initial outputs and inputs and finite
initial pipeline contents. Smoothing permits signed inputs and outputs.
Changing orders and Vensim spellings such as `SMOOTH` are rejected. No hidden mutable state is added to the native
expression language. All delay updates use one pre-tick snapshot; nested
stages do not pass their newly updated values downstream during the same tick.

`SMTHN(input, duration, order[, initial])` accepts constant integer orders 1–255
and variable durations. `DELAYN` accepts the same order range. Its default
`--delayn-policy constant` requires provably constant duration. Explicit
`--delayn-policy cascade` permits duration driven by TIME, stocks and lookups,
using current-duration material stages. Both policies are recorded in metadata;
neither accepts changing order or delay-output-dependent duration.

Under the cascade policy, stage contents initialize to `initial * duration / N`.
At each tick, every outflow is `q[i] / (current_duration / N)`; simultaneous
Euler updates use old contents. Changing duration immediately changes output,
without rescaling stored material. The pipeline conserves input minus output.
This is an explicit interpretation of the material cascade described in
[Stella's DELAYN documentation](https://iseesystems.org/resources/help/v3/Content/08-Reference/07-Builtins/Delay_builtins.htm),
not a claim of equivalence to every source dialect. [Vensim DELAY N](https://www.vensim.com/documentation/fn_delay_n.html)
documents earlier-duration behavior, and PySD 3.14.3's XMILE implementation carries
duration history. The verified second-order history option is described below;
general history orders remain unsupported.

```sh
python3 tools/xmile2ir.py tests/oracles/xmile/fixtures/delayn_variable.xmile \
  --units metadata --delayn-policy cascade --outputs all --out build/cascade.ir.json
```

Explicit `--delayn-policy history2` maps only constant-order-2 DELAYN calls to
native `history2` components. First-stage transfer uses current duration;
boundary output uses saved previous duration. Updates conserve signed quantity
and commit history with both stages. Signed initial outputs and inputs are
supported. Other history orders fail. The default `constant` and explicit
`cascade` policies are unchanged. There are 101 matching historical Vensim
observations and 1,386 additional PySD observations for this bounded contract.
Failed conservation probes for other PySD orders remain explicit diagnostics;
see [history acceptance](DELAY_HISTORY.md).

`DELAY(input, duration[, initial])` freezes duration at its simulation-start value.
The lag must be 1–1,000,000 whole simulation ticks. Fractional lags are rejected;
no source-specific rounding is assumed. Outputs before that lag use the explicit
initial value, or simulation-start input by default. Signed signals are supported.
Nested fixed delays preserve the sum of their lags.

These mappings follow the [XMILE 1.0 specification](https://docs.oasis-open.org/xmile/xmile/v1.0/os/xmile-v1.0-os.html),
particularly graphical functions and delay functions. This bounded importer
does not claim XMILE base-level conformance.

## Source input functions

Under the default `--input-policy grid`, Euler source `STEP(height, first)`, `RAMP(slope, first)` and
`PULSE(quantity, first[, interval])` lower to distinct pure native functions
`XMILE_STEP`, `XMILE_RAMP`, and `XMILE_PULSE`. A step includes its starting tick;
the ramp is zero through its starting tick and subsequently `slope * elapsed`.
A pulse emits `quantity / dt` for one tick. Omitted or zero interval means once;
a positive interval repeats. These are the parameter meanings in the
[XMILE input definitions](https://docs.oasis-open.org/xmile/xmile/v1.0/os/xmile-v1.0-os.html).
Existing native `PULSE(start, width)` remains a separate, unit-height function.

First times and intervals must be constant expressions, possibly through
auxiliaries or lookups, aligned to the simulation grid. First times may precede
the simulation start. Relative starts are bounded to ±1,000,000 ticks and
intervals to 1,000,000 ticks. Only roundoff is tolerated: eight machine epsilons
scaled by relative ticks and absolute clock/dt, with the allowance itself capped
at one millionth of a tick. Unrepresentable or coarsely resolved clocks fail.
Off-grid schedules, time/state-dependent schedules and RK4 source input calls
are explicitly rejected. Heights, slopes and quantities may vary with state.

Inputs are evaluated at the absolute source start during stock and delay
initialization. Runtime evaluation uses shared old state. A pulse visible at
the final observation does not integrate beyond the horizon. Metadata records
`input_policy: euler_grid_aligned_quantity_pulse`. Native dimensional checks
preserve STEP units, multiply RAMP slope units by time and divide PULSE quantity
units by time; source units still follow the metadata-only contract below.

Explicit `--input-policy next_tick` accepts constant off-grid first times and
fractional intervals. STEP and RAMP sample at tick time; PULSE assigns each event
to the first tick on or after its nominal time, counts all events assigned to
that tick, and emits `count * quantity / dt`. Intervals shorter than dt are
supported within the documented count/precision bounds. Lowering uses separate
`XMILE_NEXT_*` functions with the source start as schedule origin. Initialization
uses the same rule, and final-tick rates never integrate beyond the horizon.
Metadata records `euler_next_tick_quantity_pulse`. This explicit Fathom policy
has 19,584 exact-rational oracle comparisons and does not assert universal source
vendor timing. See [timing rules, limits and evidence](OFFGRID_INPUTS.md).

## Units and other limits

`--units metadata` is mandatory. Original units are retained in the sidecar;
imported IR units are dimensionless. **Import does not validate, convert, or
reconcile source dimensions.** Native hand-authored IR retains strict dimensional
checks. Model-unit definitions remain source metadata identified by the source hash.

Clipping is limited to the explicit Euler contract above. Integrators other than Euler/RK4, arrays, modules, other
functions outside those listed above, and unknown semantic elements/attributes
remain unsupported. Header metadata, views, documentation, model-unit metadata,
and the historical editor equation-order preference are ignored. DTD/entity
declarations and excessive input size/nesting fail. A successful conversion
does not guarantee a model stays in its valid numerical domain over its horizon.

## Evidence

Five unmodified pinned corpus variants (three distinct models) pass full stock
trajectory comparisons against historical Stella/Vensim exports. A sixth
fixed-delay variant matches its historical export of unspecified tool provenance. The latest
lookup case includes negative flow rates and a return to zero stock. Separately,
four purpose-built XMILE fixtures provide matching pinned PySD 3.14.3 stock trajectories, with the nested-fixed-delay exception below,
including mixed/nested stateful functions, explicit/default initialization,
feedback, shared references, named extrapolation, and variable durations. The variable-duration fixture covers abrupt shortening/lengthening, nested calls, and durations driven by integrated stocks, including delivery feedback. Reversing variable order
leaves trajectories unchanged.

PySD 3.14.3 translates **inline** extrapolating tables through `numpy.interp`,
which clamps endpoints. We observed this in its generated Python and do not use
that behavior as the oracle for inline extrapolation. Named extrapolating tables
agree across tools; inline forms are checked against hand-calculated trajectories
and the specified policy.

The explicit variable-duration DELAYN policy adds **1,386 comparisons** against
a separate pinned PySD model expanded into literal stocks and flows: 660 stock
and 726 auxiliary observations across both source orders. It covers orders
1/2/3/5, default/explicit initialization, abrupt and gradual changes, stock-driven
duration, feedback, nesting and aliases. A two-stage hand calculation separately
checks contents, current-duration output timing and conservation. The original
unexpanded PySD DELAYN trajectory is retained as a discrepancy diagnostic and
excluded from pass counts. Source input functions have complete hand-derived
trajectories across four dt values and three start times, quantity checks,
initialization, nesting, override, unit and rejection tests.

PySD's sequential fixed-state updates shorten the nested `DELAY(DELAY(...))`
fixture by one tick. Its values remain in the pinned reference for reproducibility
but are excluded from matching-comparison counts. Fathom verifies every value
of that trajectory against an exact sum-of-lags calculation, also after reversing
source variable order. Total evidence is **43,598 matching cross-tool stock
comparisons**, plus **98 hand-oracle nested-fixed stock comparisons**.

[Oracle provenance, tolerances, and coverage](../tests/oracles/xmile/README.md)
distinguish the 8/67 upstream import count from the separate custom fixtures. The [independent eligibility audit](XMILE_COVERAGE.md) measures 8/8 assessable supported-equation files imported (100%); seven have compatible complete historical references (87.5%). The new RK4 source conflicts with its Euler export, as documented below. Another 49 malformed files remain unassessable.
The 80% gate is met for this assessable subset. M2 remains open for the remaining source-function contracts; this is not coverage of malformed XML or general XMILE conformance.

## Stock-free auxiliary models

When a model has no stocks, each scalar auxiliary is emitted as an IR
`{id, expr, unit}` output. Auxiliary references are expanded with the existing
cycle checks; stateful calls still own explicit delay components. No artificial
stock is introduced. Outputs observe the committed tick and cannot mutate it.
The pinned evaluation-order model now reproduces both Stella values exactly;
hand tests additionally cover time-dependent auxiliaries, references, cycles,
and fixed-delay state in a stock-free model.

## RK4 mapping and evidence

`sim_specs method="RK4"` maps to native `integrator: "rk4"`; case is ignored
for the source spelling. Omitted/Euler maps to `euler`. Other method names and
fallback lists are rejected explicitly. The mapping preserves the declared
source method. [XMILE's integration method definitions](https://docs.oasis-open.org/xmile/xmile/v1.0/xmile-v1.0.html)
distinguish Euler and RK4.

The RK4 source fixtures for SIR and time-dependent growth are tested against
independent pinned SciPy DOP853/Radau trajectories, with fourth-order convergence
and reversed variable ordering. They are not passed through the Euler PySD
generator. The source equations and stage behavior also have exact hand checks.

The upstream `zeroled_decimals` XML declares RK4, while its Vensim `output.tab`
uses Euler timing for `stockmixed`: the XML equations give derivative
`-.6777 - t`, so the exact RK4/continuous stock is `-.6777*t - t*t/2`. The
export instead has `-.6777*t - t*(t-1)/2`. At t=10 those are -56.777 and
-51.777. Both original artifacts remain unchanged. Tests verify all six RK4
stock trajectories against closed forms and confirm that an explicitly labeled
Euler diagnostic rerun reproduces the full export. That rerun is excluded from
source-faithful historical coverage.

## Nonzero starts and complete fixed-delay source coverage

Source start/stop map to native `time.start = start` and
`time.horizon = stop - start`. Horizon remains elapsed duration, not an absolute
stop. TIME, stock/default delay initialization, and duration initialization use
the declared start; output timestamps remain absolute. Negative starts are
allowed. The grid must contain 1–1,000,000 whole ticks with distinguishable
finite timestamps (and distinguishable RK4 half stages).

A nonnegative flow without a clipped source stock has no availability limit.
References to its value lower to the pure native `NONNEGATIVE(expression)`
filter, including in delay inputs, default initial values, and auxiliary chains.
This filters negative finite rates to zero and preserves units. A flow whose
source stock limits its amount cannot be substituted this way and is rejected
when referenced, including through an alias or with `--outputs all`.

The importer now accepts a narrow allowlist of Stella metadata in the
`http://iseesystems.com/XMILE` namespace: the named UI preferences in the pinned
source, disabled multiplayer settings, Builtin/adaptive time formatting, and an
empty default format. Unknown attributes/content remain errors. For Euler only,
`simulation_delay` must be finite and nonnegative, while `restore_on_start` and
`instantaneous_flows`, if present, must be `false`. These settings and ignored
UI metadata are recorded in the sidecar. This is a fresh batch simulation;
interactive restore, multiplayer, arbitrary vendor settings, and alternate
flow-reporting policies are not implemented.

`tests/delay_xmile/test_delay_xmile.xmile` remains byte-for-byte unchanged. With
`--outputs all`, all eight declared variables match its pinned `output.tab`
at all 13 ticks, in both source orders: **208 comparisons** (26 stock, 156
auxiliary and 26 flow values). The upstream README leaves generating software
unspecified; this is a pinned historical reference, not a claimed new Stella
run. The whole suite now has **43,598 matching stock**, **158 auxiliary**, and
**26 flow** comparisons; RK4 and hand-oracle checks remain separate.

Hand tests additionally make the filtered inflow cross zero in both directions,
check default and explicit delay initialization and exact release ticks, and
check native parameter overrides, negative starts, source TIME initialization,
RK4 absolute stage times, unit errors, and collapsed/overflowing clock rejection.
The fixed-lag and default-initial behavior follows the documented
[Stella DELAY contract](https://iseesystems.org/resources/help/v3/Content/08-Reference/07-Builtins/Delay_builtins.htm).
