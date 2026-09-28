# Native series and parameter-table inputs

`runtime/data_inputs.hpp` adds two read-only consumers of [validated snapshots](RUNTIME_DATA.md):
`ExogenousSeries` and `ParameterTable`. They own their required data, check declared
units/types and preserve source identity. They can be shared across private
simulation trajectories. General JSON model binding and expression registration
remain open; the test probe's request format is not a public IR contract.

## Exogenous series

`SeriesSpec` names time/value columns, expected units and either `hold` or `linear`
interpolation. Both source columns must be f64, and time must be the table's unique
key. The source is therefore sorted and duplicate times have already failed loading.
At least one row is required. Negative historical times are allowed. Unit labels
must match exactly; no conversion or model-level dimensional inference occurs.

- Hold is right-continuous: a knot takes its own value; between knots, the latest
  preceding value applies.
- Linear interpolation uses the containing interval's normalized fraction and
  fixed-policy `std::lerp`. Exact knots return the stored value without interpolation.
- Outside the source range, both policies hold the nearest endpoint. A singleton
  source is constant everywhere. There is no silent linear extrapolation.
- Queries must be finite. Exact knots, held samples and endpoints preserve stored
  finite float bits, including negative zero and subnormals.

The interpolation fraction uses `(t-left)/(right-left)`. If the endpoint difference
overflows despite finite endpoints, all three times are scaled by one half before
forming the fraction. This avoids a spurious infinite denominator. Fraction bounds
and the resulting value are checked. Interpolation uses finite-precision f64
arithmetic; it does not promise correctly rounded real arithmetic or universal
bitwise agreement with other interpolation libraries.

`sample({start, step, count})` produces owned `times` and `values` vectors plus file
and canonical source hashes. It evaluates `start + double(i) * step` with the
project's fixed floating-point policy, rather than repeatedly adding the step.
Start is finite and nonnegative, step is finite and positive, and count is
1–1,000,000. Every timestamp must be finite and strictly greater than its predecessor.
Overflow and an unrepresentable clock increment reject the whole request. The
adapter never silently skips or merges samples. Count includes the initial sample.

The simulation can index this array by its known tick; no interpolation is needed
inside the loop. The caller must align its simulation clock with this grid. Euler
examples use the left sample for each step. RK4/midpoint stage sampling, adaptive
clocks and generic `series_id(t)` expression wiring are not implemented by this
native grid adapter; callers must explicitly provide the required sampling policy.

## Parameter tables

`ParameterTable` owns a validated table and a nonempty list of
`ParameterField{parameter, column, expected_unit}` mappings. Target names must be
nonempty and unique. Each mapped source column must be f64 with an exactly matching
unit label. Integer columns are not silently converted into model doubles. Several
parameter names may explicitly read the same column; unused source columns are allowed.

`values(key)` performs an exact typed lookup and returns an owned map in lexical
parameter-name order, the actual source key, and both source hashes. Full-width
integer keys never pass through double. A missing key is an error, not an empty map
or default value. Mutating a returned map cannot alter subsequent reads. Parameter
names are native mapping names; matching them to a particular model and deciding
how scenario overrides interact with file values remain declarative-layer work.

Unit/type/mapping failures use `DATA_UNIT` or `DATA_MAPPING`; absent or invalid keys
use `DATA_KEY`. Series shape/policy/query failures use `DATA_SERIES`; invalid grids
use `DATA_GRID`. Errors return no partial sampled series or parameter receipt.
The receipts identify the source table; a future run manifest must additionally
record the selected mappings, interpolation policy and grid.

## Running the example

```sh
cmake --build build-arrow --target fathom_series_from_data --parallel 4
./build-arrow/fathom_series_from_data \
  models/data/synthetic_parameters.csv models/data/synthetic_seasonality.csv
```

The example selects each practice's synthetic hourly rate, samples a seasonality
factor at hourly ticks, and feeds those inputs into the actual SD kernel. Linear
samples are `1, 1.5, 2, 1.5, 1`. Four Euler steps end at 2,461.5 for disputes and
1,953 for forensics. This illustrates binding mechanics, not the consulting-firm
economic model or a calibrated revenue forecast.

## Correctness evidence

An independent Python `Fraction` oracle checks **72 cross-format series configurations**
and **7,968 query/grid values**. Maximum observed absolute interpolation difference
is **1.7763568394002505e-15**. Hold/knot/endpoint comparisons require exact float bits;
interior linear comparisons use an explicit 32-epsilon endpoint-scale bound.
**12 parameter configurations** check **696 values**, including uint64 keys above
2^53, source provenance, mapping order and signed zero. Eighteen invalid request
cases reject. CSV, Parquet and IPC have the same decoded behavior.

Native checks add singleton/empty series, adjacent-knot queries, overflowing source
intervals, invalid policies/units/types, stalled/overflowing grids, key failures and
owned-return isolation. The adapters feed the actual SD kernel over 192 private
trajectories at 1/8/32 threads. Results agree bitwise across thread counts and with
an exact left-grid integral (38.5 for the native fixture). Data-loader and population
binding regressions remain in the affected test selection.

```sh
cmake --build build-arrow --target data_inputs_tests runtime_data_probe --parallel 4
ctest --test-dir build-arrow -R '^runtime_(data_inputs|data_contract|population_init)' --output-on-failure
```

The same checks run under ASan/UBSan for project code; the prebuilt Arrow SDK remains
uninstrumented. CSV-only builds verify compilation and unavailable-reader handling.
Declarative binding schemas, optional fallbacks, model-level unit validation,
manifests/replay and C/Python exposure remain open. M6 is not complete.
