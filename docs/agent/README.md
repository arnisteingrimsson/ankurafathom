# Pilot workflow: current executable subset

This playbook covers generation, declared validation, execution and the synthetic
pilot summary, plus standalone SD structure diagrams. General native report, hybrid
diagrams, bridge explanation, fit, automated extremes and the full M7 independent-
agent acceptance exercise are still open.

Use an Arrow-enabled `build-arrow/fathom`, the pinned `.venv-runtime` Python,
and Graphviz `dot` on PATH.
Run from the repository root with new destination directories:

```sh
.venv-runtime/bin/python examples/ankura_pilot/generate.py build-arrow/review-inputs
build-arrow/fathom check build-arrow/review-inputs/practice-0.ir.json \
  --experiment build-arrow/review-inputs/scenarios.json --threads 8 \
  --out build-arrow/disputes-validation.json
.venv-runtime/bin/python examples/ankura_pilot/run.py \
  build-arrow/review-inputs build-arrow/review-results \
  --executable build-arrow/fathom --threads 8
```

Read `review-results/report.md`, `yearly.csv`, and both
`practice-N.validation.json` files. The runner validates both practices and
requires a pass before running each one. It then compares the exact captured
inputs and numeric result identities with those in the corresponding run
manifest. If these differ, the runner raises an error rather than publishing a
completed pilot summary. The model declarations cover accounting, fee-specific
work balances, capacity, nonnegative backlog/headcount and cumulative totals.

For a simpler model with all four declaration types:

```sh
build-arrow/fathom check models/validation_transfer.ir.json
build-arrow/fathom run models/validation_transfer.ir.json --require-check
```

To diagnose a failure, read `code`, `pointer` and `first_failure` (or `first_error`)
in the JSON report. Open the pointed declaration; check its units and intended
relationship before changing a tolerance. `fail` means the model, declaration or
inputs need attention. `warn` for no declarations does not qualify a pilot.
Do not turn off a failed assertion simply to obtain a pass.

A pass covers declared rules at the output grid for the supplied experiment.
Consult `coverage.not_assessed` before making broader accuracy claims. Synthetic
assumptions are not calibrated Ankura effects. A thousand repeated deterministic
scenario addresses are not independent uncertainty draws.

Native file manifests can be verified and bundled using `verify-results` and
`bundle`; see [replay documentation](../PORTABLE_REPLAY.md). Changing IR or data
requires a new validation run. See [declaration semantics](../DECLARED_VALIDATION.md)
for expression grammar, tolerances, sampling and API limitations.


## Explain a pilot result

The CLI runner generates eight first-year revenue/profit explanations and links
their HTML views from its summary. To inspect a different month:

```sh
build-arrow/fathom explain build-arrow/review-results/practice-0.parquet \
  --output cumulative_revenue --at 24 --scenario 1 \
  --format html --out build-arrow/month-24-revenue.html
```

This verifies the recorded input/build/result identities before reconstructing
the selected interval. Default `--from` is the preceding sample; set `--from 0`
for a cumulative window. Read the flow contributions, residual and dependency
receipts, then check the explicit limitations. A successful explanation verifies
model arithmetic; it does not estimate intervention causality. See
[explanation documentation](../EXPLANATIONS.md). Original files or a portable
bundle and the matching execution build are required.


## Inspect model structure

Install Graphviz so `dot` is on PATH. The CLI pilot runner now links a structure
page per practice from its report. For a separate review:

```sh
build-arrow/fathom viz build-arrow/review-inputs/practice-0.ir.json \
  --out build-arrow/structure-review --output cumulative_profit \
  --manifest build-arrow/review-results/practice-0.parquet.manifest.json
```

Use the actual model filename in the generated `pilot.json` plan. Inspect
`index.html`, zoom the SVGs, and read expressions/source receipts in `graph.json`.
Stock-flow arrows follow declared endpoints; signed rates can reverse transport.
Dependency cycles are retained, without inferred causal signs. Manifest linkage
verifies inputs only; the diagram never claims result verification. Read
[diagram scope and diagnostics](../VISUALIZATIONS.md) before broadening a review.
