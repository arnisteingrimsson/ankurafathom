# Python scripting interface

The optional `ankurafathom` package provides `Model.from_json`, `Experiment`,
`run() -> pyarrow.Table`, and `lint()`. A nanobind extension calls only the public
[C ABI](C_API.md); it does not link directly to the C++ model interpreter or Arrow
C++ API. Execution, input validation and scenario expansion use the existing runtime.

Expressions are limited to 65,536 source bytes and 256 parser/AST levels.
Exceeding a limit returns the structured `IR_EXPR` diagnostic; split larger
expressions into auxiliaries. Arithmetic order within accepted expressions is unchanged.

## Build and use

The build requires a Python interpreter with development headers, nanobind 3.1.0
and, for table results, PyArrow 25.0.1. These are pinned in `runtime/requirements.txt`.
The binding is off by default so the native platform still builds without Python
development headers or nanobind.

From the project directory, using the existing runtime environment:

```sh
cmake -S . -B build-arrow -DCMAKE_BUILD_TYPE=Debug \
  -DFATHOM_ARROW_PYTHON="$PWD/.venv-runtime/bin/python" \
  -DFATHOM_ENABLE_PYTHON=ON \
  -DPython_EXECUTABLE="$PWD/.venv-runtime/bin/python"
cmake --build build-arrow --target fathom_python c_api_probe -j 3
ctest --test-dir build-arrow -R '^runtime_python_contract$' --output-on-failure
PYTHONPATH=build-arrow/python .venv-runtime/bin/python
```

```python
from pathlib import Path
from ankurafathom import Model, Experiment, run, lint

path = Path("models/decay.ir.json").resolve()
source = path.read_text()
assert lint(source) == {"verdict": "pass", "diagnostics": []}
model = Model.from_json(source, base_directory=path.parent)
experiment = Experiment({
    "seed": 42,
    "replications": 2,
    "scenarios": [
        {"id": 0, "parameters": {}},
        {"id": 1, "parameters": {"decay_rate": 0.1}},
    ],
})
table = run(model, experiment, threads=8)
print(table.to_pydict())
```

These commands produce a **build-directory package**, linked to the C shared
library and selected SDK. Local wheels and source distributions are also supported;
see [PACKAGING.md](PACKAGING.md) for isolated installation and native C SDK setup.
The wheel includes the Fathom library and uses a pinned, separately installed
PyArrow dependency through relative library paths. Local acceptance used CPython 3.14.2 on macOS; the CI recipe
also configures Python 3.12 but remote CI has not run.

## Input and validation contracts

`Model.from_json(source, *, base_directory=None)` accepts a string, bytes, or dict.
Strings use UTF-8; dicts are serialized with non-finite values rejected. Byte/string
inputs retain their original JSON for the C validator, including
trailing-content and embedded-NUL checks. Other object types reject. Inputs are
copied and models own their bound-data snapshots. Later changes to a source dict
cannot change an already loaded model.

`base_directory` accepts an absolute string or path-like object; it is required
when the model declares data bindings. Relative directories and embedded NUL reject.
The package does not infer a base directory from JSON text. Supply the parent of
the source model file when loading a file's contents.

`Experiment(source)` copies JSON into an immutable specification. It supports the
existing explicit, grid, Latin-hypercube and Sobol shapes. Validation is deferred
until `run`, when the model's parameters are available. It neither changes the
experiment schema nor accepts Python callbacks as simulation behavior.

`lint(source, *, base_directory=None)` runs the same loader as `Model.from_json`.
Success returns `{"verdict": "pass", "diagnostics": []}`. Model/argument failures
return `verdict: fail` with code, pointer, message and C diagnostic truncation bits.
Python type/encoding errors and resource/runtime failures raise exceptions. Lint
includes existing structural, dimensional and data-binding validation; it does not
run simulation trajectories or the future M7 behavioral checks.

## Execution, ownership and errors

`run(model, experiment=None, *, threads=1, seed=None, provenance=False, progress=None)` returns a PyArrow table.
`None` means a single trajectory. Explicit threads must be integers in 1–256;
seeds must be integers in 0–2**64−1. Booleans, floats and strings are not integer
options. `seed=None` preserves the model/experiment default, while `seed=0` is an
explicit override. Thread count controls ensembles; single runs remain serial.

`progress(completed, total)` runs on the calling thread with the GIL held and must
return a bool. False cancels with `FathomError(status=8)` and no partial table; callback
exceptions retain their original object and traceback after worker cleanup.
Cancellation happens between trajectories. See [execution callbacks](API_EXECUTION_CALLBACKS.md).

The wrapper releases the GIL during native loading and execution/export. Independent
Python calls may share a validated native model. The runtime still owns thread-count
determinism and scenario ordering; the Python wrapper adds no alternate execution
path. Free-threaded Python and subinterpreter support are not claimed.

Results transfer through a standard `arrow_array_stream` capsule and PyArrow's
C Stream protocol. The native result handle is freed before Python imports the
stream. Capsule cleanup covers unconsumed streams and import failures. Imported
tables and slices own their buffers and remain valid after deleting the model or
reader. Results are materialized in memory. The default is observation schema
**0.1**. `provenance=True` selects schema **0.2** lineage columns and embeds the
run manifest under `table.schema.metadata[b"ankurafathom.manifest"]`. It requires
a bool. See [API_PROVENANCE.md](API_PROVENANCE.md) for receipt lifetime, in-memory
manifest version 0.3 and saved-artifact verification.

`FathomError` extends `RuntimeError`, with `status`, `code`, `pointer` and `truncated`
attributes copied from the C diagnostic. Later API calls and other threads cannot
change an existing exception. Truncated UTF-8 diagnostics decode with replacement;
the original truncation flags remain available. Runtime worker failures publish no
partial Python table. Allocation-failure injection has not been performed.

CSV-only builds support Python loading and linting; a valid run raises
`FathomError(status=7, code="FATHOM_UNAVAILABLE")` when Arrow export is requested.
The `run` function requires PyArrow to be installed; loading/linting do not.

## Verification and remaining scope

`tests/python_contract.py` checks each JSON input form across SD, DES, ABM, agent
pools, agent stocks, hybrid and the three declarative data bindings. It compares
all numeric bits with a C-compiled consumer at 1/8/32 workers, including all scenario
designs and seed extremes. Additional checks cover multi-batch results, signed zero,
subnormals, maximum finite doubles, table/slice lifetime, concurrent runs/errors,
truncated diagnostics, input snapshots and abandoned/failed Arrow imports.

For ASan/UBSan, CMake provides `fathom_python_test_host`; build that target along
with the extension before running CTest. This small instrumented executable embeds
the selected Python interpreter, loading the sanitizer before the extension. The
binding and nanobind runtime are instrumented; the prebuilt Python/Arrow SDKs are
not. This checks the boundary's ownership behavior, not new scientific accuracy.

M6 remains in progress. Local Python/C installation and packaging are implemented.
Cross-platform binary release qualification and general data-binding composition
remain open. CLI file-backed replay bundles are implemented; API memory receipts
remain outside their scope. CLI file runs now default to manifests;
Python continues to select result lineage explicitly with `provenance=True`.
