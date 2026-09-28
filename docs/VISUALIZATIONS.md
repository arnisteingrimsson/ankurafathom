# Native standalone SD structure diagrams

`fathom viz` renders static model declarations for review. It does not run a
simulation, estimate causal effects or validate economic assumptions.

```sh
build-arrow/fathom viz models/bass_sd.ir.json --out build-arrow/bass-review
build-arrow/fathom viz model.json --out review --output cumulative_profit \
  --manifest results.parquet.manifest.json
```

The model must be standalone SD, including valid data bindings if declared. The
output directory must be new and its parent must exist. Graphviz `dot` must be
on PATH. Only visualization needs this external renderer; `run`, `check` and
`explain` do not. Local QA used Graphviz 13.0.1; layout geometry can differ between
versions. Graph JSON is authoritative for topology, not SVG byte identity.

Each successful call writes eight files and prints the graph receipt as JSON:

- `stock-flow.dot`, `.mmd`, `.svg`: all stocks, flows and external boundaries.
- `dependencies.dot`, `.mmd`, `.svg`: parameters, auxiliaries, stocks, flows,
  delays, lookup tables, external series, data bindings, clock references and outputs.
- `graph.json`: declarations, typed edges, model hash, optional manifest linkage
  and explicit verification scope.
- `index.html`: local review page linking the SVGs and structured receipt. The SVG
  can be opened separately and zoomed; large dependency graphs need zooming.

DOT, SVG and Mermaid each carry the canonical model hash. Without `--manifest`,
they explicitly say no run is selected. With a file-backed manifest, the CLI
checks exact model/data/experiment/build input identity and includes the manifest
ID. It does **not** read or verify result values. Use `verify-results` or
`explain` for those checks. Base parameter declarations are not scenario values.
The JSON contract is [visualization_report.schema.json](../ir/schema/visualization_report.schema.json).

## Interpretation

Stock-flow arrows follow declared endpoints: source stock → flow → destination
stock. Null endpoints have individual external boundary nodes. Signed flow rates
can reverse actual transport. A self-loop remains visible with both endpoints.
Stock and flow units appear in the labels; full expressions and clipping flags
remain in `graph.json`.

Dependency arrows point from a reference to its consumer. Stock-inflow and
stock-outflow edges point from a flow to the stock it updates; these are update
relationships, not signed causal effects. Outputs have a distinct namespace even
when they share an identifier with a stock. Delay input and duration references
are separate roles. Lookup/series calls and parameter-table bindings retain
source receipts. STEP/PULSE/RAMP clock references are included by the expression
parser. Both branches of conditional expressions are included.

`--output ID` retains the selected output's complete recursive dependency closure,
including feedback cycles; it does not filter the stock-flow diagram. Without it,
the dependency view includes all declared components and outputs, even disconnected
ones. Neither view expands internal delay state, integrator stages, clipping
allocation/priority rules or evaluation ordering. No feedback-loop polarity is
inferred. DES/ABM/hybrid diagrams and general native `report` remain open.

## Failure and qualification

Unknown outputs, unsupported modes, input drift, existing destinations and missing
renderers return structured diagnostics. Layout is bounded to 2,000 nodes and
10,000 edges per view, with a 30-second deadline per renderer invocation. Rendering
finishes in an owned temporary directory before destination reservation. Existing
files, directories and symlinks are rejected. Publication reserves the directory
exclusively then moves files; it is not crash-atomic as a whole directory.

The contract independently checks Bass topology, cycle-safe filtering, signed
boundaries/self-loops, namespace collisions, declaration ordering, dynamic delays,
clock references, escaped labels, manifest drift, renderer failure and cleanup.
Generated SVGs are parsed as XML and checked for model identity and active/link
markup. The pilot contract verifies diagram/run identities and data receipts
alongside the independent economics comparisons. Static Graphviz layouts are
inspected offline; no browser-rendered HTML review is claimed.
