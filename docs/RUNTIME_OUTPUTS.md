# Arrow, Parquet and CSV results

The M6 output adapter converts committed trajectories to an Arrow table in memory
and writes Parquet or Arrow IPC files. It is isolated in `runtime/outputs/`; the
simulation core does not include or link Arrow. CSV-only builds remain supported.
The native API is declared in `include/ankurafathom/runtime/outputs.hpp`.

## Build and run

The default configuration looks for Arrow/Parquet CMake shared-library packages
version 25 or later and falls back to CSV when unavailable. The tested SDK is
25.0.1. To select the pinned PyArrow wheel's C++ libraries explicitly:

```sh
python3 -m venv .venv-runtime
.venv-runtime/bin/python -m pip install -r runtime/requirements.txt
cmake -S . -B build-arrow -DCMAKE_BUILD_TYPE=Debug \
  -DFATHOM_ARROW_PYTHON="$PWD/.venv-runtime/bin/python"
cmake --build build-arrow --target fathom runtime_outputs_tests --parallel 4
./build-arrow/fathom run models/decay.ir.json \
  --experiment models/decay.sobol.experiment.json --threads 8 --out results.parquet
./build-arrow/fathom run models/agent_stock_sd.ir.json --out results.arrow
ctest --test-dir build-arrow -R '^runtime_outputs' --output-on-failure
```

No Python subprocess participates in execution or serialization. CMake uses the
selected Python only to locate the C++ headers/libraries, and the cross-reader
test uses PyArrow to decode results. The wheel environment must remain available
at runtime because the executable dynamically links its Arrow/Parquet libraries.
Set `-DFATHOM_ENABLE_ARROW=OFF` for a build without those dependencies. An explicitly
requested but invalid SDK causes configuration to fail.

`--out` selects Parquet for `.parquet` and Arrow IPC **file** format for `.arrow`
or `.ipc`. Other suffixes retain CSV behavior. `--format csv|parquet|arrow` overrides
the suffix; duplicate/unknown options fail. Binary formats require a file path.
Standard output remains CSV. Requesting a binary format from a CSV-only build
returns structured diagnostic `IR_OUTPUT_UNAVAILABLE` before simulation.

## Observation schema 0.1

Native outputs without provenance, CLI `--no-manifest` runs and default Python runs use the same five non-null Arrow/Parquet columns, including for a
single run or an empty native result:

| Column | Arrow type | Contract |
| --- | --- | --- |
| `scenario` | `uint32` | Runtime address, 0–65535 |
| `replication` | `uint32` | Runtime address, 0–65535 |
| `time` | `float64` | Finite nonnegative model time |
| `output_id` | `utf8` | Nonempty output name |
| `value` | `float64` | Finite observed value |

A single CLI run has scenario and replication zero. Trajectory addresses must
increase strictly; observations preserve the runtime's time/output declaration
order. Duplicate time/output pairs within a trajectory are rejected. The adapter
neither sorts away errors nor drops rows. Arrow validates UTF-8 identifiers; the
IR already restricts identifiers to its narrower alphanumeric/underscore syntax.

Schema metadata carries `ankurafathom.schema_version=0.1` and
`ankurafathom.table=observations`. Parquet stores the Arrow schema so non-nullability,
unsigned types and metadata round-trip. Parquet is uncompressed, uses no dictionary
encoding and has at most 65,536 rows per group; IPC batches have the same row limit.
Both preserve the bit patterns of finite float64 values, including negative zero
and subnormals. Cross-build/platform simulation tolerances remain the separate
execution contract; identical Parquet file bytes are not required.

Manifest-backed runs use [observation schema 0.2](RUN_MANIFESTS.md#result-lineage-observation-schema-02),
adding `manifest_id` and effective `scenario_parameters` to each row and a complete
manifest to Arrow/Parquet metadata. CLI file runs use this path by default, including scenario and
replication in single-run CSV, and write `<output>.manifest.json`. Select
`--no-manifest` for legacy output. Native/Python provenance remains opt-in;
standard-output CLI runs remain legacy CSV.

CSV keeps the existing three-column single-run and five-column experiment layouts,
with 17 significant digits and the classic locale. Native names containing commas,
quotes or line breaks receive CSV escaping. Empty native results emit a header.

## Publication and failure behavior

The adapter validates results and builds the Arrow table before opening a private
sibling temporary file. Arrow writes through a duplicate of that file descriptor;
all serialization, footer, flush and close operations must succeed before the
original descriptor is synced and the file atomically replaces its destination.
No path reopening or direct truncation of the destination is used. Setup, encoding,
I/O and rename failures clean up the temporary and preserve an earlier result.
The CLI rejects output paths that alias the model or experiment through a symlink
or hard link. This is a POSIX macOS/Linux contract, not a guarantee of directory
metadata durability after a power failure.

## Correctness evidence

`runtime_outputs_tests` covers exact float bits (zero, negative zero, positive and
negative subnormals, minimum normal, maximum finite magnitudes and neighboring
values), maximum addresses, UTF-8/CSV escaping, locale independence, invalid
observations, empty schemas, injected writer failure and rename cleanup. Native
round-trips use the Arrow C++ readers. A 65,539-row fixture crosses both Parquet
row-group and IPC record-batch boundaries.

`runtime_outputs_contract.py` independently decodes files with PyArrow and checks
**622,546 ordered rows**. This includes a 1,000-scenario × three-replication
stochastic experiment at **1/8/64 threads** in both formats, six additional execution
families, and a closed-form decay check. Nine corruption controls detect altered
schema, metadata, addresses, order, row counts, nulls, negative zero and subnormals.
It forces an actual mid-write `EFBIG` in an isolated child process using a file-size
limit, and checks that the previous destination survives. CSV-only tests verify
format rejection and explicit CSV override without importing PyArrow.

Normal and ASan/UBSan runs instrument project code. The wheel's prebuilt Arrow and
Parquet libraries are not sanitizer-instrumented. CI configuration covers Linux
and macOS, normal and sanitizer builds; local results do not claim remote CI passed.

## Remaining M6 scope

This is the observation-output contract. [Native data readers](RUNTIME_DATA.md)
and bounded model binding adapters are now implemented separately, along with
[default CLI file lineage and strict local replay](RUN_MANIFESTS.md). Event-log tables,
general binding composition remains open. [File-backed replay bundles](PORTABLE_REPLAY.md)
are implemented. C ABI and Python
model bindings are implemented.
Full trajectories and output tables are materialized in memory. Streaming
ensembles and memory budgeting remain future runtime work; this increment makes
no performance or Tenstorrent claim.

Implementation references: [Arrow C++ Parquet](https://arrow.apache.org/docs/cpp/parquet.html),
[Arrow IPC](https://arrow.apache.org/docs/cpp/ipc.html), and
[PyArrow C++ SDK access](https://arrow.apache.org/docs/python/integration/extending.html).
