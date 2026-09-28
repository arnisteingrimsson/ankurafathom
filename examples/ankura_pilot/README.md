# Synthetic Ankura scenario preview

This runs the existing Phase A economics through the general declarative SD
runtime: two synthetic practices, sixty months, and all sixteen combinations of
Copilot, tool building, process automation and acquisition. It produces native
Parquet observations, run manifests, annual practice/firm comparisons, NPV and
payback. The generator and reporting script do not call the Phase A simulator;
that separate C++ implementation is used only as a test oracle.

From the repository root, with the Arrow-enabled CLI, pinned runtime Python,
and Graphviz `dot` on PATH for native diagrams:

```sh
.venv-runtime/bin/python examples/ankura_pilot/generate.py build-arrow/ankura-inputs
.venv-runtime/bin/python examples/ankura_pilot/run.py \
  build-arrow/ankura-inputs build-arrow/ankura-results \
  --executable build-arrow/fathom --threads 8
```

Both destination directories must be new. Open `ankura-results/report.md` and
`yearly.csv`. Each practice has the same model structure, populated by a keyed
practice table and hold-interpolated calendar series in Parquet. The generated
`scenarios.json` contains four binary intervention switches. Native computation
is in `fathom check` and `fathom run`; Python generates inputs and summarizes outputs.
CLI previews require passing declared validation for each practice and identical
validation/run input and result identities. The thirteen declarations cover
accounting, fee-specific work balances, capacity, nonnegative backlog/headcount
and cumulative monotonicity. Per-practice validation JSON retains codes, counts,
tolerances and coverage; the preview report includes the verdict. The CLI runner
also generates eight native JSON/HTML explanations: first-year revenue and profit
for baseline and Copilot in each practice. The summary links each page. See
[explanation semantics](../../docs/EXPLANATIONS.md) for identity checks and limits.

The same model runs through the Python binding:

```sh
.venv-runtime/bin/python examples/ankura_pilot/run.py \
  build-arrow/ankura-inputs build-arrow/ankura-python-results \
  --package build-arrow/python --threads 32
```

With an installed wheel, omit `--package`. CLI runs emit file-backed receipts
that support `fathom replay` and `fathom bundle`. Python emits API-memory receipts;
their native observations/provenance can be verified, but memory-receipt replay
remains unsupported. Every summary retains the originating manifest IDs. Python-only previews
explicitly report native validation as not run; use the CLI check separately.

`generate.py --practices PATH` accepts the Phase A CSV schema and validates its
basic numeric/fraction bounds. Regenerate after changing source inputs: initial
stocks are literal IR values derived from those rows, and annual demand growth
is expanded into calendar series. The source CSV and its SHA-256 are retained.
All effects, adoption lags, acquisition additions, costs and the 10% discount
rate are synthetic assumptions, not estimates from Ankura data. The full source
and generated model remain available for inspection.

The reference model distinguishes baseline work from actual delivery hours:
T&M bills actual hours, fixed-fee bills contracted baseline work. Saved T&M hours
earn revenue only if demand fills the released capacity. Backlog, payroll,
capacity and intervention costs remain explicit. Acquisition adds the reference
headcount and pipeline at month 13 and includes its purchase/integration costs.

This is a working **preview**, not M8 acceptance. M7's broader validation, hybrid explanation,
hybrid diagrams, fit and native reporting workflows remain open. Standalone SD
[structure diagrams](../../docs/VISUALIZATIONS.md) are linked per practice. The bounded native
`check` is available; see [its semantics](../../docs/DECLARED_VALIDATION.md). The preview does not model
hiring/attrition dynamics, stochastic opportunities, per-consultant behavior or
causal identification. Initial assumptions and pricing mechanisms need sponsor
review before interpreting the outputs as a business forecast. No real client or
employee data is used.

Validation covers all 16 combinations against the separate Phase A implementation,
monthly accounting/capacity identities, zero-demand/capacity extremes, pricing
effects and exact CLI/Python/thread equality. `--scenarios 1000` repeats the sixteen
deterministic configurations at distinct scenario addresses for runtime testing;
it does not create 1,000 independent uncertainty draws or confidence intervals.
