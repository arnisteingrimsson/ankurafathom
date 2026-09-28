# Declarative exogenous series

Standalone SD model files can declare a local CSV, Parquet or Arrow IPC series
and call it in expressions as `series_id(t)`. The series is resolved during lint
and load; the model owns its values throughout simulation. This requires an
Arrow-enabled build, including for CSV input.

The runnable example is
[`models/data/seasonal_stock.ir.json`](../models/data/seasonal_stock.ir.json):

```sh
./build-arrow/fathom lint models/data/seasonal_stock.ir.json
./build-arrow/fathom run models/data/seasonal_stock.ir.json
```

Its binding reads the same synthetic seasonality table as the native adapter:

```json
{
  "id": "seasonality",
  "source": "synthetic_seasonality.csv",
  "schema": {
    "key_column": "time",
    "columns": [
      {"name": "time", "type": "f64", "unit": "day"},
      {"name": "factor", "type": "f64", "unit": "1"}
    ]
  },
  "use": {
    "kind": "exogenous_series",
    "time_column": "time",
    "value_column": "factor",
    "interpolate": "linear",
    "extrapolate": "hold"
  }
}
```

Paths resolve relative to the model file. The native [table rules](RUNTIME_DATA.md)
apply, including exact schemas, no nulls, finite values, unique keys, format
inference and canonical row ordering. Both time and value columns must be f64.
The time column must be the unique table key; negative historical times are valid.
The table must have at least one row. Singleton series are constant. A schema may
include other columns, which still participate in validation and table identity.

The time-column dimension must match the model clock. The value-column unit is
the return dimension of the function. Function arguments and surrounding flow,
output and delay expressions are unit checked. Dimensional equivalence is allowed;
no scale conversion is inferred. Series function IDs share the parameter/component
namespace and cannot be built-in names, `t` or `dt`. Binding IDs remain unique
across both series and parameter-table entries. Output IDs retain their existing
separate namespace.

`interpolate` must be `hold` or `linear`. Hold is right-continuous: an exact knot
returns that knot's value; intermediate times return the preceding knot's value.
Linear interpolates the surrounding samples and preserves exact-knot values.
`extrapolate` is required and must be `hold`: before the first sample and after
the last, the corresponding endpoint value is returned. Both policies preserve
signed-zero values at exact knots and held endpoints.

Evaluation uses the actual expression argument. Thus Euler flow evaluation uses
its left-boundary time, RK4 uses each internal stage time, and `seasonality(t-lag)`
queries the explicitly shifted time. Outputs use their observation time. Delay
inputs and durations retain the existing Euler tick semantics; initial durations
are validated with the bound function before simulation. No data-file access or
mutable interpolation cursor occurs during evaluation.

The CPU correctness implementation evaluates the immutable native series on
demand. It does not first sample only the model's observation grid, which would
lose RK4 midpoint information. Grid caching can be a later optimization if it
preserves these semantics for all allowed expression arguments.

RK4 still uses its ordinary four-stage quadrature when a step crosses a knot.
It does not automatically split steps at source changes. In particular, a hold
jump exactly at a right endpoint contributes the new value to the final RK4
stage. This is the defined discretization, not an exact integral of discontinuous
forcing, and fourth-order convergence is not claimed across discontinuities or
piecewise-linear corners. Choose/refine the step against the intended model.

Each `ir::SeriesData` stores the resolved path, binding ID/index, column mapping,
interpolation policy, dimensions and an owned `ExogenousSeries`. Its file and
canonical SHA-256 are available through the series accessors. Hashes identify the
source bytes/table; the mapping and policy are separately retained and are not
part of the canonical table hash. Copying or moving a Model keeps its series
ownership. Reloading is a new snapshot; changing a source after loading does not
alter existing runs. Sources must remain stable while loading, and callers must
not mutate a shared Model during concurrent runs.

Series and parameter-table bindings can coexist, including scenario overrides
of ordinary parameters used in a series-driven expression. A series itself is a
function and cannot be overridden as a scalar parameter. Output paths cannot
alias series sources during CLI preflight, including symlink/hardlink aliases.
Source errors retain binding-prefixed `DATA_*` diagnostics. Source row locations
refer to the external table; ordinary IR diagnostics identify declaration or
expression errors. Receipts are in-memory; manifests and replay remain open.

This increment supports required local bindings in standalone SD. Optional
fallbacks, remote sources and general hybrid binding remain open.
[Population initialization](POPULATION_DATA_IR.md) is now available separately in
typed ABM models. Unsupported modes and policies reject explicitly.

The public test contract independently computes each stage with rational
arithmetic for a stock driven by a series plus state-dependent loss. It covers
24 format/policy/integrator/grid configurations, 1/8/32 threads, 4,320 numeric
comparisons, shifted queries, mixed parameter/series bindings, delay inputs and
initial-duration checks, singleton signed zero, aliases and invalid declarations.
The native suite checks source hashes, copy/move ownership and 192 trajectories
at 1/8/32 threads after the source becomes unreadable as a table. See the session
log for test results and scope.
