# Application layer: implementation report and UI-agent handoff

September 30, 2026. Scope: a local, tested API around the existing engine, with
project-owned presentation. No GUI was built or tested, and no simulation kernel,
native IR loader, C/Python ABI, or T&R model equation was changed in this work.
The other agent's existing T&R files and in-progress root documentation were
left intact.

## What is available

| Capability | Delivered behavior |
|---|---|
| Model discovery | List registered models; retrieve versioned parameters, effective defaults, domains, units, assumption labels/sources, metric definitions and capabilities |
| Immutable execution | Capture the model and referenced data, validate overrides, write a native experiment, run the existing CLI, then verify its saved numeric result |
| Background jobs | Queued/running/completed/failed/cancelled/interrupted states, persisted job records, stage polling and SSE updates |
| Cancellation | Isolated native process groups can be terminated even during one long trajectory; cancelled jobs expose no completed result |
| Result reuse | Reuse a verified completed execution only when model/data/descriptor, experiment, seed, threads and engine/application identity match |
| Result access | JSON metric series filtered by scenario, replication, dimension and time; pagination and endpoint-preserving sample selection |
| Shared metrics | Endpoint stocks, period sums, cumulative differences, declared hold/linear integrals and time-weighted means, explicit sums across metrics and ratios of aggregated quantities |
| Decision outputs | Baseline/candidate deltas, defined percentage changes, scalar target checks, incremental NPV and sampled undiscounted payback |
| Explanation | Existing native standalone-SD JSON explanation through the API; verified re-execution, not a stored general event trace |
| Agent integration | OpenAPI 3.1, descriptor JSON Schema, dependency-free Python/browser clients, a runnable demo, and an adapter for the current T&R model |

The API is versioned under `/v1`. Project descriptors have schema version `1.0`.
The implementation is in `application/fathom_service/`; the machine-readable
contract is [application/openapi.json](../application/openapi.json).

## Start the service

The [application README](../application/README.md) contains exact demo and T&R
registration/start commands. Start from the repository root using the existing
runtime Python environment and a built `fathom` executable. Port 8765 is the
default; `--port` changes it. `--workers` defaults to two concurrent jobs; each
request may use 1–32 native threads. The UI's exact origin must be passed with
`--allow-origin`, for example `http://localhost:5173`.

The service binds only to loopback. Hosted deployment, authentication, project
authorization and remote workers are separate work. One service process owns a
state directory using an exclusive lock. Model registration is an operator-side
startup operation, not an endpoint accepting arbitrary browser filesystem paths.

The server does not hot-reload project inputs. Generate a new model/descriptor
and restart to register a new content version. Existing saved jobs remain
inspectable. UI requests must include the version returned by `describe`; stale
versions reject rather than run with changed assumptions.

## Contract for the UI builder

| Operation | Endpoint |
|---|---|
| Discover models | `GET /v1/models` |
| Model/metric catalog | `GET /v1/models/{id}/describe` |
| Start execution | `POST /v1/runs` |
| Status/progress | `GET /v1/runs/{id}` |
| Stage-event stream | `GET /v1/runs/{id}/events` |
| Cancel, retain record | `POST /v1/runs/{id}/cancel` with `{}` |
| Observation slices | `GET /v1/runs/{id}/results` |
| Full-resolution summaries and targets | `POST /v1/runs/{id}/summary` |
| Baseline comparison / optional valuation | `POST /v1/compare` |
| Supported native explanation | `POST /v1/runs/{id}/explain` |

New execution returns HTTP 202. An exact completed-result cache hit returns
HTTP 200, `cache_hit: true`, and the **original run ID**. It is explicitly reused
evidence, not a newly computed trajectory. Failed, cancelled and interrupted jobs
are not cached. Changed saved result bytes fail integrity checks on access.

Requests use `Content-Type: application/json`. Errors use
`{"error":{"code":"...","message":"..."}}`. Unknown fields, parameters,
out-of-domain values, duplicate JSON keys, stale model versions and unsupported
operations reject. Numeric boolean parameters use 0/1, not JSON true/false.

### Minimal browser flow

Import the [browser client](../application/client/fathom.mjs):

```js
import { FathomClient } from './fathom.mjs';
const api = new FathomClient('http://127.0.0.1:8765');
const model = await api.describe('ankura_tr');

const run = await api.run({
  model_id: model.id,
  model_version: model.model_version,
  overrides: { ai: 0.2, policy_hold: 0, policy_responsive: 1 },
  seed: 20260930,
  replications: 1,
  threads: 2
});

const stopListening = api.watch(run.id, status => {
  // Display queued/executing/verifying/completed, or terminal error/cancellation.
  // Fetch results only when status.status === 'completed'.
});

// Once completed: annual values, from full-resolution results.
const annual = await api.summary(run.id, {
  metrics: ['revenue', 'ebitda', 'margin', 'headcount', 'utilization'],
  from: 0, to: 12
});

// Chart/playback observations; raw revenue is cumulative in this model.
const series = await api.results(run.id, {
  metric: ['revenue'], from: 0, to: 60, max_points: 61, limit: 1000
});
```

Do not execute the summary/result calls before completion. `watch()` closes on
terminal status. Its returned function stops listening; `api.cancel(run.id)`
stops computation. SSE reconnection returns the current status, not a historical
event log. Polling is equally supported.

Progress is **execution stages**, not elapsed simulated time or a fabricated
percentage. `completed_trajectories` remains null until successful verification;
the total is known. The existing CLI does not emit intermediate trajectory
callbacks. Playback is entirely a presentation operation over saved observations.

### Parameters and metrics

The descriptor exposes a curated editable subset. Unlisted native parameters
cannot be overridden through this service. Effective defaults are resolved from
captured parameter tables, including CSV/Arrow/Parquet; placeholder IR literals
are not presented as bound defaults. Model registration first uses native lint.

Parameter ranges and cross-parameter constraints are **application-contract
checks**. They do not modify the engine's existing validation or automatically
apply to someone invoking the native CLI outside this service. Model authors
must review descriptors alongside model equations. The JSON Schema checks
structure; service registration also checks native IDs, defaults, units and
dependency cycles.

Time aggregation is explicit:

- `last`: value at the requested endpoint; appropriate for closing headcount.
- `sum`: sum of period amounts timestamped at period end, over **(from,to]**.
- `delta`: difference between two cumulative observations.
- `integral` / `mean`: piecewise hold or linear integration, as declared, on the
  recorded grid. Integral units are source units multiplied by model time units.
- `sum_metrics`: sum explicitly named component summaries; no implicit grouping.
- `ratio`: ratio of the selected numerator and denominator summaries. A zero
  denominator produces null plus a reason, never infinity or an invented zero.

Summary endpoints must match recorded sample times, allowing only floating-point
representation tolerance. Dimensions label metrics (e.g. practice or level);
`dimension=level:analyst` filters series. This is not an arbitrary data-cube API.
Each call selects one scenario and replication; it never silently averages
stochastic replications. Requests with multiple scenarios use
`scenarios: [{id: 0, overrides: {...}}, ...]` instead of `overrides`.

Result pagination uses `offset`, `limit` (maximum 10,000) and `next_offset`.
`max_points` selects existing observations per series, retaining endpoints; it
can omit intermediate peaks. It does not interpolate or preserve every event.
Charts must label raw cumulative series correctly and use `/summary` for period
KPIs. Never compute authoritative totals or percentages from downsampled data.

### Comparisons and valuation

```json
{
  "baseline_run": "BASELINE_RUN_ID",
  "candidate_run": "CANDIDATE_RUN_ID",
  "baseline_scenario": 0,
  "candidate_scenario": 0,
  "replication": 0,
  "metrics": ["revenue", "ebitda", "margin"],
  "from": 0,
  "to": 60,
  "valuation": {
    "metric": "cash_flow",
    "annual_discount_rate": 0.10,
    "time_units_per_year": 12,
    "initial_incremental_cash_flow": 0
  }
}
```

Post this to `/v1/compare`. Both runs must have the same model version and engine
identity. Delta is candidate minus baseline; percentage change is
`100 * delta / abs(baseline)`. Zero baseline yields a null percentage with a
reason. This is one replication comparison, not an uncertainty interval.

Valuation accepts period-amount or cumulative metrics with aligned timestamps.
Discounting uses period-end times measured from the window start. Initial
incremental cash flow is an explicit additional amount at the window start;
do not supply costs already included in the selected metric a second time.
Payback is the first recorded time cumulative incremental cash flow becomes
nonnegative after a deficit. It is undiscounted, has no interpolation, and does
not guarantee that later periods remain nonnegative. `no_deficit` and
`not_recovered` are explicit outcomes. The caller declares the economic meaning
of its cash-flow series; the T&R operating-cash metric is not a complete
enterprise free-cash-flow valuation.

`summary` optionally accepts scalar `thresholds`, e.g.
`[{"metric":"margin","operator":"gte","value":0.2}]`.
These check the selected result; they do not solve for a break-even parameter or
estimate the probability of achieving the threshold.

### Explanations and provenance

`POST /v1/runs/{id}/explain` accepts
`{"metric":"revenue","from":0,"at":12,"scenario":0,"replication":0}`.
It invokes the existing native explainer on the saved manifest. It can require
re-executing the full recorded experiment and has a 120-second request timeout.
Unsupported native cases fail explicitly. Derived application metrics such as
margin do not gain fictional additive explanations; inspect their declared
numerator/denominator and explain supported source outputs.

Responses include originating run ID, model version, native manifest identity
for newly completed jobs, engine/application identity, and verified artifact
hashes. Raw observations can be mapped to their native output via the descriptor.
Comparisons identify both source runs. This is model accounting and lineage,
not evidence of a real-world causal effect.

## Current T&R adapter and verification

The current saved eight-level pilot registers **nine scenario controls and
25 metrics**, including finance, all eight level FTEs, billable-level paid
hours, total headcount, margin and utilization. Hold and responsive hiring are
mutually exclusive; both zero means freeze. Bound model/data files are copied
at service startup. A companion configuration is used to classify levels;
task classifications must match the generated model.

A full 60-month baseline completed through the new execution wrapper with native
declared checks and result verification. **75 annual and level-headcount values
match the existing verified pilot summary exactly**, with maximum gap zero.
This tests transport and aggregation of an existing synthetic model; it is not
new calibration or real-world predictive validation. The evidence is local in
`artifacts/application-tr-parity.json` and `artifacts/application-tr-service/`.

The application contract suite covers native execution and full saved values,
bound defaults, immutable input capture, versions, bad requests, schemas, CORS,
pagination, weighted ratios, interval boundaries, zero denominators, thresholds,
NPV/payback, explanation, SSE, cancellation, failures, restart recovery,
exclusive state ownership, cache identity and corruption rejection.
The suite passes **22/22 against both the normal and ASan/UBSan engine builds**.
The Python service itself is not ASan-instrumented. Long-worker cancellation is
also tested with a controlled external process, independent of engine speed.
The browser client passes JavaScript syntax checking; no browser or GUI test is
claimed. Final local records are in `artifacts/application-contract-final-native/`
and `artifacts/application-contract-final-sanitize/` (22 tests each, 2.72 and
3.74 seconds including test-runner overhead). These do not replace a full engine
regression or certify remote CI.

## Explicit limits and next extensions

- Local macOS/Linux service; no hosted authentication or multi-user authorization.
- Source-checkout execution; this adapter is not yet included in the existing
  native SDK or Python wheel packaging.
- No new engine session API, native mid-trajectory callback contract, resumable
  partial run, or in-run parameter mutation. Cancellation terminates the worker.
- No general ABM/DES animation trace or general hybrid explanation.
- No revenue attribution engine, Sobol sensitivity estimator, prediction model,
  surrogate, or MCP adapter in this increment.
- No automatic validation that a model author's chosen metric definition matches
  business/accounting policy. Descriptor review remains part of model acceptance.
- Full results are retained locally; JSON queries currently load verified CSV
  into memory. Pagination limits response size, not server-side scan memory.

The UI agent can now build controls, scenario execution, baseline comparisons,
time-series charts, numeric playback, assumption/provenance panels and supported
explanation views against this contract. Design and presentation remain project
specific.
