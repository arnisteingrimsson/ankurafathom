# Verified SD explanations

`fathom explain` reconstructs a selected result and its flow accounting from a
captured run. It supports standalone SD with Euler or RK4, including auxiliary
expressions, lookup tables, bound parameters/series and delay output values.
Clipped flows and hybrid models are rejected explicitly. This is bounded M7
work; it does not complete the bridge attribution or stored-event-trace gates.

```sh
build-arrow/fathom run models/bass_sd.ir.json --out build-arrow/bass.csv
build-arrow/fathom explain build-arrow/bass.csv --output adopters --at 1 \
  --out build-arrow/bass-explanation.json
build-arrow/fathom explain build-arrow/bass.csv --output adopters --from 0 --at 1 \
  --format html --out build-arrow/bass-explanation.html
```

All output paths must be new. Default format is JSON. HTML is self-contained with
inline SVG, tables, model name, IR/run identities and an expandable full JSON
receipt. It needs no network resources or JavaScript. With `--out`, stdout remains
the JSON receipt; without `--out`, HTML format prints the complete HTML document.

## Inputs and selection

Accepts a schema-0.2 CSV result (with its conventional `.manifest.json` sidecar),
an Arrow/Parquet result (embedded manifest), a file-backed run manifest, or a
portable input bundle. Explicit result files are verified before explanation.
Manifests/bundles do not require a saved result file; they establish its numerical
identity by complete re-execution. The report distinguishes these two cases.
API-memory manifests cannot yet be re-executed for explanation.

Model/data/experiment identities and execution build policy must match the run,
as with strict replay. Original inputs must be available unless a bundle is used.
An old run is not automatically reinterpreted under new code. If code or inputs
change, produce a new run. No source, result or bundle member is modified.

`--output ID` and `--at TIME` are required. Scenario and replication default to
zero and must exist in the captured experiment; use `--scenario N` and
`--replication N` otherwise. `--threads N` changes verification worker count,
without changing the selected run identity. All recorded scenarios/replications
are re-executed before a result is attributed.

The default window is the preceding output interval. At the initial sample it
has zero duration. `--from TIME` selects a longer window. Both endpoints must be
on the output grid; a small floating-point representation tolerance accommodates
values such as 0.3 on a 0.1 grid. The report emits the actual sampled times. No
interpolation is performed.

## Accounting method and limits

The explainer first verifies the complete ordinary run against its recorded
ordered numeric SHA-256. It then executes the selected trajectory with private
passive stocks integrating each declared flow expression. These extra stocks
are never referenced by original expressions. They use the original integrator,
including RK4 stage states; this is not endpoint rate multiplied by duration.
Every original output/time/value must remain bit-identical with instrumentation.
A mismatch aborts explanation.

For a stock output, the report lists each incident flow's integrated amount,
endpoint sign (-1 source, +1 destination, zero self-loop), and signed contribution.
Signed flow rates retain their signs. The sum is compared with the observed stock
change, retaining a `rounding_residual`. Stock updates and subtraction of cumulative
flow integrals have different floating-point arithmetic; the residual is reported,
not silently forced to zero. Nonrepresentable arithmetic fails explicitly. Passive
integrals can overflow even when an original model's offsetting flows leave its
stocks finite; explanation then fails rather than inventing a decomposition.

Clipping requires instrumentation of **applied** flow amounts, so it is excluded
from this method. Expression and delay outputs expose dependency values but have
`accounting: null`: arbitrary nonlinear expressions are not assigned fictitious
additive contributions. Delay history internals and hybrid events remain outside
this increment. This is verified **re-execution**, not a stored event trace.

The dependency graph is finite even with stock feedback: nodes are visited once,
with explicit incoming edges. Output IDs have a distinct `output:` namespace.
Nodes retain flow/auxiliary/output expressions, parameter overrides, unit metadata,
source-key/column receipts and raw/canonical data hashes. Parameters overridden by
scenarios retain the underlying source receipt and an explicit override flag.
Flow `unit` is a rate unit; `amount_dimension` describes its integrated amount.

Values are snapshots at window boundaries, not individual RK4 stage values or
time-averaged contributions. An exogenous-series node's `value_at_window_start`
and `value_at_window_end` specifically evaluate the series at those times; they
do not claim to record every dynamic lookup argument. Lookup definitions and
expressions remain available in the JSON. Syntactic dependencies include inactive
conditional branches and are not causal effect estimates or signed causal loops.

## Pilot review and qualification

The CLI pilot runner now saves JSON/HTML explanations of first-year revenue and
profit for baseline and Copilot scenarios in both practices. Its scenario summary
links the eight pages and checks originating manifest IDs. Python-only preview
runs do not generate native explanations. These are explanations of synthetic
model mechanics, not real Ankura forecasts.

The [explanation schema](../ir/schema/explanation_report.schema.json) and
[contract](../tests/validation/explanation_contract.py) cover analytic Euler/RK4
amounts, Bass feedback, multi-flow/signed accounting, parameter overrides,
thread parity, dependencies, data drift, result corruption, relocated bundles,
HTML escaping and unsupported modes. Pilot explanations are compared with the
independently computed annual summaries. Current evidence is in
[the session log](SESSION_2026_09_28.md).

HTML structure, no-script/no-external-resource behavior and escaping have automated
coverage. Browser visual review of the local artifact was blocked by the browser
URL policy; it is not claimed as passed. Native `fathom viz`, the broader
`fathom report`, clipped-flow/bridge attribution and M7/M8 acceptance remain open.
