# C ABI and Arrow streams (ABI 1)

The platform now builds a shared library, `libankurafathom`, with a C11-compatible
[public header](../runtime/include/ankurafathom/c_api.h). It loads validated models,
runs single trajectories or experiments, and exposes owned observations and their
numeric SHA-256. Arrow-enabled builds also export independently owned CPU Arrow
C Streams. The [Python package](PYTHON_API.md) now uses this boundary through
nanobind. [Local SDK/wheel installation](PACKAGING.md) is implemented. [Run provenance](API_PROVENANCE.md) is also exposed; broader
platform release qualification remains open in M6. [Execution callbacks](API_EXECUTION_CALLBACKS.md) expose ordered progress and cooperative cancellation.

```sh
cmake --build build-arrow --target fathom_c_api c_api_tests c_api_probe c_api_arrow_tests c_api_arrow_ipc_probe
ctest --test-dir build-arrow -R '^runtime_c_api' --output-on-failure
```

The library has ABI/SONAME major 1, independently of the project's release version.
Call `fathom_abi_version()` before using ABI 1 structures. The macOS export list and
Linux version script expose only the fourteen declared C symbols; C++/Arrow internals
are hidden. Link through CMake target `fathom_c_api`, or use the public include path
`runtime/include` and `-lankurafathom`. Build-tree artifacts and a CMake-installed native SDK/CLI are available; see
[PACKAGING.md](PACKAGING.md). Python wheels carry the C library privately. Windows
acceptance and cross-platform binary release qualification remain open.

## Entry points

| Function | Contract |
| --- | --- |
| `fathom_load_json` | Load a length-delimited JSON buffer, with an optional absolute data base directory |
| `fathom_load_file` | Load a model path using the CLI loader |
| `fathom_run` | Run an immutable model with optional experiment JSON and run options |
| `fathom_run_with_callbacks` | Run with calling-thread progress and cooperative cancellation |
| `fathom_results_size` | Return trajectory and observation counts |
| `fathom_results_observation` | Read an ordered observation, retaining exact binary64 time/value bits |
| `fathom_results_sha256` | Borrow the ordered numeric identity string |
| `fathom_results_arrow` | Export an independently owned CPU Arrow C Stream |
| `fathom_results_manifest` | Borrow the stable run-manifest JSON and byte length |
| `fathom_results_arrow_with_manifest` | Export schema 0.2 with row lineage and embedded manifest |
| `fathom_model_free`, `fathom_results_free` | Release independently owned handles |
| `fathom_last_error`, `fathom_abi_version` | Inspect thread-local diagnostics and ABI version |

Loading and execution reuse the CLI's model, experiment and scenario-design
validators and interpreter. Explicit scenarios, grid, LHS and Sobol designs use
the same JSON definitions. The API adds no alternate simulation engine.

## Ownership and execution

Creation destinations must point to `NULL`. They stay `NULL` on failure; passing
an already populated destination rejects without replacing it. Model and
experiment JSON are byte spans, limited to 256 MiB, with no terminating NUL
required. Raw NUL bytes and trailing JSON reject. Input buffers remain caller-owned
and can be discarded after the call returns.

File loading resolves bindings relative to the model path. JSON loading requires
an explicit absolute base directory when `data` is present. Bound tables use the
existing immutable snapshot loader. In-memory model identity retains captured
raw/canonical hashes and an empty source path; it does not pretend that the JSON
came from a replayable file. [Version 0.3 manifests](API_PROVENANCE.md) expose
these captured identities without claiming replay support.

Pass `NULL, 0` for the experiment to request a single run. A non-NULL empty buffer
is invalid JSON. `NULL` options select one worker and the model/experiment default
seed. Initialize explicit options with `FATHOM_RUN_OPTIONS_INIT`; `threads` must
be 1–256, `reserved` must be zero, and `override_seed` must be 0 or 1. An explicit
seed, including zero or `UINT64_MAX`, requires `override_seed=1`; otherwise the seed
field must be zero. Worker count controls experiments; single runs remain serial.

Each successful result owns its trajectories independently. Free the model after
`fathom_run` returns if it is no longer needed; the result remains readable. Rows
retain scenario/replication and runtime observation order. The digest uses the
existing [ordered IEEE-754 encoding](RUN_MANIFESTS.md#numeric-identity).
Output-ID and digest pointers are borrowed until the result is freed; other getters
do not invalidate them. Scalar/observation output arguments are unchanged on failure.

Free each live handle exactly once. Free functions accept NULL. Non-NULL handles
must be live, correctly typed handles returned by this library; stale, forged or
wrongly cast handles are outside the contract. Use normal platform C structure
layout, without custom packing. Immutable models support concurrent runs, and
results support concurrent reads. Keep handles alive until every call using them
has returned, and use distinct caller-owned output storage across threads.

## Arrow C Stream ownership

```c
struct ArrowArrayStream stream = {0};
fathom_status status = fathom_results_arrow(results, &stream);
if (status == FATHOM_OK) {
    fathom_results_free(results);  /* stream owns its data */
    results = NULL;
    /* Pass stream to an Arrow importer, or use its callbacks directly. */
    stream.release(&stream);      /* only if not transferred to an importer */
}
```

The public header includes the vendored standard CPU C Data/Stream declarations
from Arrow 25.0.1 in `ankurafathom/arrow_c.h`; C callers need no Arrow SDK headers.
The declarations coexist with the SDK header in either include order. Upstream
license/notice are retained under `third_party/arrow_c`.

The destination must be initialized with `release == NULL`. A zero-initialized or
previously released struct is valid; a live stream is rejected without modification.
Failure leaves the whole destination unchanged. CSV-only builds export the same
function and return `FATHOM_UNAVAILABLE` with no stream. Invalid arguments still
return `FATHOM_INVALID_ARGUMENT` in those builds.

Each successful export materializes its own Arrow table from the result and owns
that table through the stream reader. Multiple exports have independent cursors and
lifetimes. Model and result handles can be freed after export returns. This is a
batch interface over completed, materialized results; it does not stream a running
simulation or bound total memory usage. Batches currently contain at most 65,536
rows; consumers must handle arbitrary positive batch sizes and array offsets.

The schema is the existing unmanifested observation schema **0.1**: non-null
`scenario:uint32`, `replication:uint32`, `time:float64`, `output_id:utf8`,
`value:float64`, with `ankurafathom.schema_version=0.1` and
`ankurafathom.table=observations` metadata. Observation order and binary64 bits are
preserved. Use `fathom_results_arrow_with_manifest` for schema 0.2 lineage and
embedded provenance; it has the same ownership rules.

Direct consumers call `get_schema` and `get_next` into fresh/released output
structures. A successful `get_next` with `array.release == NULL` signals end of
stream. Release each returned schema and array independently via its own callback;
they remain valid after releasing the stream. Release the parent array/schema,
not its children separately. Call each live object's release callback once; it
sets that object's `release` to NULL. Never duplicate ownership by copying a live
struct; Arrow importers move ownership and clear the source's release callback.
Keep the shared library loaded until all exported objects have been released.

Serialize callbacks on a given stream. Independent streams may be used independently.
The callbacks follow Arrow's errno-compatible return codes and per-stream
`get_last_error` (call only after a failed callback), rather than the Fathom status
or thread-local error channel. Callback errors do not update `fathom_last_error`.

## Error boundary

All status-returning entry points catch C++ exceptions. Statuses distinguish
invalid arguments, IR diagnostics, runtime failures, allocation failure, out-of-range
indices, ABI mismatch, cancellation and an unavailable optional feature. Failed runs publish no partial result handle, including
when an experiment worker fails after other trajectories have completed.

`fathom_last_error()` points to a thread-local, fixed-storage record with status,
code, JSON pointer and message. IR failures preserve their existing diagnostic code
and pointer. Diagnostic copying does not allocate memory. Each string is terminated;
long diagnostics retain byte prefixes, with truncation bits 1/2/4 for code/pointer/
message. Code/pointer prefixes also flag truncation at embedded NUL. Consumers
should account for possible UTF-8 truncation when a bit is set.
The allocation-failure status is implemented; allocation fault injection is not
part of this checkpoint's acceptance evidence.

A successful status-returning call clears the calling thread's diagnostic.
Version queries, error queries and free calls preserve it. Copy the record if it
must survive a later API call. Other threads have independent records. This is
an API error channel, not model validation or numerical-accuracy certification.

## Evidence and remaining scope

The C11 executable `c_api_tests` links the shared library without C++ or Arrow
headers. It checks the analytic Euler decay recurrence, handle/buffer lifetime,
failure output preservation, worker failure, version/options validation, truncated
diagnostics and concurrent calls with isolated errors. A second C-compiled
consumer emits exact time/value bits and numeric hashes; an independent Python
contract compares them with CLI results and hashlib for SD, DES, ABM, agent pools,
hybrid and data-bound models at 1/8/32 workers, all scenario designs and signed zero.
This Python test is a verifier, not the planned Python binding.

A separate C11 stream consumer checks release ordering, destination preservation,
repeated exports, schema, end-of-stream, multiple batches and exact floating-point
bits after freeing the handles. A native SDK consumer imports the stream after
freeing those handles and writes IPC for an independent PyArrow schema/bit check.
That subprocess design also exercises sanitizer builds without loading an
instrumented library into an uninstrumented Python process.

See the session log for measured coverage. C/Python local packaging is implemented;
run provenance and [execution callbacks](API_EXECUTION_CALLBACKS.md) are exposed.
The nanobind package is implemented; see [PYTHON_API.md](PYTHON_API.md). Tenstorrent and performance work remain later phases.
