# Project application layer

For the operator workspace, start the [AnkuraFathom control center](control_center/README.md):

```sh
.venv-runtime/bin/python -m application.control_center.server
```

Open `http://127.0.0.1:8086` to inspect project configurations, models, run traces
and validation evidence, then open the selected project's customer UI.

This package wraps the existing native engine for AI-built, project-specific UIs.
Start with the [implementation report and UI-agent handoff](../docs/APPLICATION_LAYER.md).
The mathematical engine and existing IR are unchanged.

From the repository root:

```sh
.venv-runtime/bin/python -m application.fathom_service \
  --registry application/examples/demo/registry.json \
  --state artifacts/demo-api-state \
  --engine build-arrow/fathom \
  --allow-origin http://localhost:5173
```

API: `http://127.0.0.1:8765`. Machine-readable contract: `/openapi.json` or the
checked-in [OpenAPI file](openapi.json). Model-package schema:
[model-descriptor.schema.json](model-descriptor.schema.json).

Use [client/fathom.mjs](client/fathom.mjs) from browser code, or
`application.fathom_service.client.Client` from Python. No frontend framework
or UI layout is prescribed. The server is loopback-only, for a trusted local
operator; it is not an authenticated, multi-user hosted service.

Create a T&R registry from the other agent's generated model:

```sh
.venv-runtime/bin/python application/examples/register_tr.py \
  --model artifacts/tr-pilot-150-validated-20260930/batch-06/inputs/model.json \
  --out artifacts/my-tr-api-registry

.venv-runtime/bin/python -m application.fathom_service \
  --registry artifacts/my-tr-api-registry/registry.json \
  --state artifacts/my-tr-api-state \
  --engine build-arrow/fathom \
  --allow-origin http://localhost:5173
```

The model path is an example from this workstation. On another machine, use a
generated T&R `inputs/model.json` and its sibling data files. A companion
`config.json` one directory above `inputs/` enables the reviewed level/FTE and
billable-utilization metrics. Without it, the adapter exposes financial metrics
and omits those workforce aggregations rather than guessing level classifications.
Registration destinations must be new; the service state directory can be reused.

Run the API tests (HTTP, not GUI):

```sh
.venv-runtime/bin/python application/tests/run.py \
  --engine build-arrow/fathom --out artifacts/my-application-test
```

Tests require `jsonschema` in addition to the existing runtime environment.
Arrow/Parquet parameter tables require `pyarrow`; the basic demo and HTTP server
use the Python standard library. macOS/Linux are supported by this local adapter.
The new `application-contract` GitHub workflow builds the engine and runs this
suite. No remote-CI result is claimed by the local report.
