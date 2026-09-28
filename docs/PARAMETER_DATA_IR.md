# Declarative parameter tables

Standalone SD model files can now select a row from a local CSV, Parquet or Arrow
IPC table and map its f64 columns to declared parameters. `fathom lint` resolves
and validates the binding; `fathom run` uses the same loader. An Arrow-enabled
build is required, including for CSV input. Models without `data` work as before.

The complete runnable example is
[`models/data/parameter_decay.ir.json`](../models/data/parameter_decay.ir.json):

```sh
./build-arrow/fathom lint models/data/parameter_decay.ir.json
./build-arrow/fathom run models/data/parameter_decay.ir.json
```

Its `data` field is:

```json
[
  {
    "id": "practice_parameters",
    "source": "synthetic_decay_rates.csv",
    "schema": {
      "key_column": "practice",
      "columns": [
        {"name": "practice", "type": "string", "unit": ""},
        {"name": "rate", "type": "f64", "unit": "1/day"}
      ]
    },
    "use": {
      "kind": "parameter_table",
      "key": "disputes",
      "parameters": [{"parameter": "decay_rate", "column": "rate"}]
    }
  }
]
```

Sources resolve relative to the model file's directory, independent of the CLI
working directory. Absolute local paths are also accepted; URI and NUL-containing
paths reject. Format follows the source suffix. The loader uses the native
[table contract](RUNTIME_DATA.md): exact columns/types, no nulls, unique typed keys,
finite floats, optional string categories and file/canonical hashes. Each column
requires a unit string; empty units are allowed for unmapped labels/keys. Numeric
parameter sources must have a valid unit dimension matching the model declaration.
Equivalent dimensional spellings are accepted; no unit scale conversion occurs.

A table key can be bool, i32, i64, u64, f64 or string. Integer key selection checks
the complete declared range without conversion through double. Integer types
require integer JSON tokens; bool is separate from numeric keys. Float keys accept
finite numeric tokens. Empty string keys are valid. Parameter values must be f64,
even when a source integer could be widened exactly.

Precedence is literal model value, then selected table value, then explicit
scenario override. Unbound parameters retain their literals. Table values are
installed before component and delay validation, so lint checks the effective
model. Literal declarations still require finite numbers and valid units. Binding
IDs are unique within `data`; a parameter may have only one data mapping across
all bindings. Different parameters may use the same source column or file.
Unknown targets and ambiguous mappings reject rather than depending on order.

The loaded `ir::Model` owns effective parameter values and a `parameter_data`
receipt for each binding: ID, resolved source path, typed key, target-to-column
mapping, selected values, file SHA-256 and canonical-table SHA-256. Run workers do
not access source files. Reloading captures new input; changing a source after
load cannot change existing trajectories or receipts. Input files must remain
stable while loading. The receipt is an in-memory record, not yet a serialized
run manifest or replay artifact. Ordinary C++ callers can mutate their own Model;
concurrent runs require callers to leave that shared model unchanged.

CLI outputs cannot overwrite a bound source, including existing symlink or
hardlink aliases. This is a preflight check, not protection against concurrent
filesystem changes after validation. Data failures retain native `DATA_*` codes
with binding-prefixed locations; source-cell locations use
`/data/<binding>/rows/<physical-row>/<column>`. These refer to source rows, not
fields in the model JSON. Model-reference, unit and mapping failures use IR codes.

This increment supports standalone SD only (omitted `mode` or `"sd"`). `data`
inside hybrid submodels or in other modes rejects. A present `data` array has
1–1024 bindings, each with 1–1024 columns and mappings. `required` may be omitted
or true; optional fallback, scenario-selected table keys, remote sources, manifests and replay remain open.
[Population inputs](POPULATION_DATA_IR.md) now work separately in typed ABM mode. [Series inputs](SERIES_DATA_IR.md) now coexist
with parameter tables in the same model.

Verification includes an independent Euler/RK4 decay calculation across
CSV/Parquet/IPC, scenario override precedence at 1/8/32 threads, full-width typed
keys, dimensional equivalence, multiple bindings, source-relative paths, protected
output aliases, missing/invalid inputs and effective delay validation. A native
suite verifies receipt values/hashes, reload isolation and 192 trajectories at
1/8/32 threads after the source becomes invalid. The schema contract checks the
public shape and rejection of unsupported binding constructs. See the session log
for actual test results and build coverage.
