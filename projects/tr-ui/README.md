# Current default: T&R operating-model workspace

The root URL now opens the assumption-first hybrid project workspace. See
[its README](workshop/README.md) for the editable people/work/AI definition,
native ABM/DES/SD model, experiment workflow and validation scope. Build it with
`.venv-runtime/bin/python projects/tr-ui/workshop/build.py` before starting the host.
The previous live monthly financial model described below is preserved at
**http://127.0.0.1:8087/monthly**, and its analysis workspace remains at `/analysis`.
These are separate models with different assumptions and outputs.

---

# T&R Live Simulation Studio — project interface

This is **the T&R project's UI**, separate from the reusable AnkuraFathom platform.
All presentation, workflow and domain-specific registration code lives in this
folder. The live controller uses a new generic synchronous observation barrier in
the native SD runtime. The existing integration code and T&R equations are retained;
the shared HTTP application service remains unchanged.

## Start locally

From the AnkuraFathom repository root:

```sh
cmake --build build-arrow --target fathom fathom_ir -j 4
.venv-runtime/bin/python projects/tr-ui/build_live.py
.venv-runtime/bin/python projects/tr-ui/server.py
```

Open **http://127.0.0.1:8087**. Stop the server with Ctrl-C.

The existing runtime environment and `build-arrow/fathom` are required, along
with the generated, calibrated T&R model at
`artifacts/tr-pilot-150-validated-20260930/batch-00/inputs/model.json` and its data
files. That is the verified 150-person test already created in this workspace.
On a fresh checkout, first generate the pilot as described in
[the model README](../../examples/tr_pilot/README.md). Use `--model`, `--engine`,
`--state` and `--port` to select other local paths or a different port. The model
must use the same T&R output/control contract and companion `config.json`.

No JavaScript build step, npm dependencies, remote fonts or hosted assets are
needed. This is a local, loopback-only project workspace. The host serves only
explicit UI assets and the existing API routes; it does not expose the repository
as a static directory. Execution and saved results live under
`artifacts/tr-ui-state/service/` by default.

## Live simulation workflow (default workspace)

The root URL is now a live model workspace. It starts with **no computed state,
plots or saved results**. Opening the page loads only model metadata. Presets
change input conditions; they never populate results.

- **Play** creates a fresh native process and advances it one month at a time.
- **Pause** stops issuing steps. An in-flight integration finishes its current
  month; no next month is calculated. The clock then remains fixed.
- **Step** initializes if necessary and computes exactly one new month.
- **Reset** terminates the native process and clears all displayed output.
- **Pace** changes wall-clock scheduling. It cannot make an expensive computation
  finish faster, and does not change the model's one-month accounting interval.

The diagram exposes current prospects, wins, completed work, backlog, workforce,
recognized fees and cash. Click a component to inspect native quantities and IDs.
Plots append observations only after a native step completes. This is an aggregate
monthly model, not individual consultant/deal animation or an event-level trace.
All 23 native assertions are evaluated on each published state, with actual left
and right values and tolerances. A failing check halts further advancement.

Parameters are frozen during a session; reset before changing them. Sessions are
deterministic (no stochastic draws), run one scenario, and end at month 60. The
controller supports four simultaneous local sessions. Idle sessions are expired
on a later creation request after 15 minutes. Hiding the browser tab pauses the
controller; leaving/reloading the page requests process shutdown. No future
trajectory is calculated or preloaded. There is no saved-result cache in this path.

The runner is in `native/live_runner.cpp`, with project session management in
`live.py`. The only reusable engine addition is `ir::run_observed_sd`, declared in
`include/ankurafathom/ir/model.hpp`. Its callback receives borrowed observations at
the initial state and after each committed integration step. It blocks **before**
the next derivative calculation. Returning false stops with the computed prefix.
Ordinary `ir::run` uses the same integration implementation without an observer.

The C++ process speaks a line-based protocol. It publishes one state, then blocks
on stdin until it receives a step command. The browser sends sequential requests;
server-side locking and expected revisions prevent duplicate requests from
advancing twice. This is interactive paced execution, not a hard real-time
scheduler or a playback of a completed run. A lost response can be retried with
the same revision. A failed process requires a reset.

Every session captures model/data inputs and records initial parameters, runner
and engine identities, native frames and check results under
`artifacts/tr-ui-state/live-sessions/`. SHA-256 chains cover the observed prefix;
a record hash covers its configuration and status. Export includes only computed
months. These receipts are a project live protocol, not the platform's full batch
run-manifest format. Verify an export with:

```sh
.venv-runtime/bin/python projects/tr-ui/verify_live.py /path/to/tr-live-session.json
```

Build and test the native observer:

```sh
projects/tr-ui/native/build/observer-tests .
.venv-runtime/bin/python projects/tr-ui/test_live.py
# Optional focused native ASan/UBSan verification:
cmake --build build-arrow-sanitize --target fathom_ir -j 4
.venv-runtime/bin/python projects/tr-ui/build_live.py --build build-arrow-sanitize --sanitize
projects/tr-ui/native/build/observer-tests-sanitize .
```

The native tests compare ordinary versus observed execution bit-for-bit for
Euler/RK4, lookup, delays and signed flows. They include a future-overflow trap:
pausing before the failing transition succeeds, proving the future was not
precomputed. Live-session tests compare **29,768 observations** from two complete
60-month sessions against previously verified batch outputs, test idle barriers,
idempotent step retries, reset, bad configurations, record corruption, and a
planted native assertion failure that stops at its first failing month.

Local routes:

| Method / route | Behavior |
|---|---|
| `POST /project/live` | Fresh session: `{model_version, overrides}`; initial state only |
| `POST /project/live/{id}/step` | `{expected_revision}`; compute exactly one month |
| `POST /project/live/{id}/stop` | `{}`; terminate and preserve computed prefix |
| `GET /project/live/{id}/state` | Read current state; does not advance |
| `GET /project/live/{id}/record` | Export computed prefix and provenance |

## Earlier analysis and validation workspace

The **Model & validation records** link opens `/analysis` at its Validation tab.
The earlier batch comparison and historical-evaluation features are retained
there as a separate analysis surface. They are not the live execution controller.
The remaining sections describe those earlier analysis features.

### Batch comparison workflow

1. Start with a verified saved example: AI adoption, pipeline growth, business
   development, hiring freeze, or no AI. The source is visibly labeled.
2. Set AI savings, pipeline growth and hiring policy. Expand the commercial
   controls for price pass-through, eligible fixed-fee share, senior BD, synthetic
   market stress and the expected success-fee multiplier.
3. Run the comparison. The API executes a baseline and candidate in one native
   experiment, with declared checks, saved-result verification and immutable inputs.
4. Inspect annual revenue, EBITDA, margin, expected workforce, utilization,
   operating cash and a five-year operating-cash NPV/payback comparison.
5. Switch years or play saved annual observations; inspect workforce by level,
   the revenue breakdown, assumptions and originating run evidence.
6. Export the displayed comparison as JSON, including its assumptions and source
   identities. An export never substitutes unrun draft settings for its results.

The matched baseline shares the candidate's hiring policy, market exposure and
success-fee assumption. It has no AI, no extra BD, no pipeline uplift, and the
original fixed-fee mix. Differences therefore include every other changed
scenario assumption; they are not automatically attributed to AI alone.

Editing controls marks the form as an unrun draft. Existing charts retain their
previous verified values until a run completes. Failed or cancelled jobs do not
replace them. Native stages are shown as stages, without invented progress
percentages. An in-progress or completed live run is restored in the same browser
session after reload. Exact completed-result reuse is visibly labeled.

## Project-specific boundaries

`server.py` generates a project descriptor using the existing T&R adapter. It
resolves effective parameters from the bound native tables and narrows the
`fixed_shift` domain to the actual remaining eligibility. For this test that is
0–0.10, making the final new-work fixed-fee share 15%–25%. The API rejects an
out-of-domain request before starting the engine. Hold/freeze/responsive are one
UI choice, mapped to the service's two numeric policy controls.

The project adds five cumulative fee metrics to the adapter's 25 metrics:
`earned_tm`, `earned_fixed`, `earned_retainer`, `earned_success`, and `disallowed`.
They use existing native outputs and the shared API's `delta` aggregation. The
revenue dialog presents this accounting breakdown. No project-side simulator,
financial aggregation library or valuation formula is introduced.

The existing native flow-explanation endpoint exceeded its 120-second timeout
when exercised against this large T&R model. It remains available in the shared
API, but is not used by the project's normal revenue-breakdown workflow. The
breakdown is an accounting reconciliation, not a native flow graph or causal
attribution report.

Authoritative live comparisons come from `/v1/compare` using full-resolution
monthly observations. Annual charts use these server-calculated period values,
not browser sums of chart points. NPV uses the monthly incremental `cash_flow`
series, a 10% annual discount rate, period-end discounting and zero additional
initial cash flow. Modeled AI costs are already included. Payback is undiscounted
first sampled recovery; later reversal is possible. Operating cash excludes
financing, taxes and capex, so this is not enterprise valuation.

The preview pack is generated from verified native pilot artifacts with the
**same shared application metrics code**. It includes native manifest/result
identities and model/data hashes. Its input hashes are checked against the live
descriptor; changed source inputs mark the old example as an unrun comparison.
To rebuild the pack after an independently validated pilot run:

```sh
.venv-runtime/bin/python projects/tr-ui/build_preview.py
```

All business assumptions remain synthetic. FTI is an external comparison only.
Expected fractional FTE is not a prediction of individual employee actions.
Acquisition modeling, real-data calibration, general animation and hosted
multi-user deployment are outside this UI increment.

## Verification

```sh
.venv-runtime/bin/python projects/tr-ui/test_project.py
node projects/tr-ui/test_ui.mjs
node --check projects/tr-ui/app.js
```

The Python contract tests cover effective bound defaults, pre-execution fee and
workforce-policy rejection, and **750 saved-example values** compared with the
verified pilot's annual report. JavaScript checks cover matched-baseline mapping,
unrun-draft detection, missing/zero values and HTML escaping.

Browser acceptance covers a real paired native execution, cancellation, verified
cache reuse, reload restoration, preset switching, draft labeling, financial and
workforce views, revenue breakdown, year selection/playback, export and narrow
mobile layout. Recorded acceptance evidence is retained locally in
`artifacts/tr-ui-acceptance/`. These are project UI checks, not a fresh full engine
regression or a claim of calibrated business forecasts.

## Validation workspace

The main **Validation** tab is project-owned. Its run selector determines which
immutable execution is inspected; changing scenario sliders does not alter that
run. Model & runs lists completed and cancelled jobs, their model versions,
scenario IDs, seed and threads. The equation browser describes the currently
registered mathematical model; the evidence export also includes the selected
run's exact captured native model and request, even for an older version.

Checks & evidence distinguishes:

- **Saved artifact integrity:** hashes are checked again on access. This detects
  changed artifacts; it is not an independent endorsement of assumptions.
- **Accounting:** three independently calculated saved-output reconciliations
  (cumulative EBITDA, revenue and cash), across every scenario, replication and
  recorded time. Expected/observed values, worst gap, location, tolerance, samples
  and failures are shown. These do not repeat the full economics oracle.
- **Native execution checks:** all 23 declared rules and their tolerances are
  inspectable. A completed require-check execution is evidence that they passed,
  but the original live worker did not retain individual residual receipts.
- **Earlier pilot:** the recorded 34-scenario oracle and replay receipt has an
  explicit scope. Its receipt hash and current model/data compatibility are
  checked against the checked-in preview. A new scenario does not inherit an
  independent-oracle comparison merely by using the same equations.
- **Historical accuracy:** unestablished until actual observations are evaluated.
  Passing technical checks is never shown as validated business forecasting.
- **AI provenance:** prior author/reviewer model IDs and work logs were not
  captured. Their absence is shown. Engine build identity is not AI identity.

The project check receipt can be regenerated with:

```sh
.venv-runtime/bin/python projects/tr-ui/record_validation.py
```

It records actual commands, exit codes, stdout/stderr and source hashes in
`verification.json`. The UI flags the receipt as stale if those sources changed.
It covers 6 project contract tests, 12 evidence/evaluation tests, 6 live-session
tests, normal/sanitizer native observer programs and the JavaScript checks, including bad units, missing/nonfinite/duplicate/gapped data, zero
percentage denominators, corrupted artifacts and a deliberately wrong formula.
This recorded test execution is separate from human or AI author/reviewer
approval, for which no identity is invented.

### Playback and timestep

Playback exposes monthly, quarterly and annual **observation intervals**, a
scrubber and four playback speeds. Finances use full-resolution period
aggregation, staffing uses period-end FTE and utilization uses the shared
weighted ratio. A cursor follows the recorded revenue trajectory. Playback uses
replication 0 and does not rerun the model or manufacture intermediate events.

The T&R model is a **monthly transition model implemented in Euler SD**, with
`dt=1 month`. Hiring and payment queues advance once per step. Substituting
`dt=0.5` changes those transitions and is not a legitimate accuracy refinement.
The project now rejects startup with a changed timestep, solver or horizon.
Finer-step execution/convergence needs a time-scaled model revision and its own
acceptance tests; it is explicitly unavailable in this UI. The engine's ability
to integrate other continuous models at other timesteps remains separate.

### Historical evaluation contract

Download the JSON template in Historical fit or import a local JSON file (up to
1 MB). Select an immutable completed run and scenario. Supply dataset provenance,
`kind` (`observed` or `synthetic`), the calendar month corresponding to model
month 1 (`start_month`, YYYY-MM), `calibration_through`, a boolean
`heldout_attestation`, acceptance thresholds, and consecutive monthly rows.

Supported metric IDs and required units:

| ID | Unit | Monthly definition |
|---|---|---|
| `revenue` | `USD` | Recognized revenue during month |
| `ebitda` | `USD` | Practice EBITDA during month |
| `cash_flow` | `USD` | Operating cash during month |
| `headcount` | `person` | Ending expected FTE |
| `utilization` | `1` | Delivery / billable-role paid hours, fraction |

Every selected metric must appear in every row and have a threshold such as
`"revenue": {"unit": "USD", "mae_max": 100000}`. Missing values, duplicates,
month gaps, invalid units, nonfinite numbers, out-of-grid observations and
empty calibration/holdout partitions are rejected. Zero-actual WAPE is undefined,
never silently zero. Negative EBITDA and operating cash are supported.

The evaluation reports MAE, RMSE, signed bias and WAPE separately before/after
the cutoff, plus actual/simulated series and every observation. The shaded chart
region identifies the holdout. Limits apply to MAE in each metric's declared
unit. Thresholds are supplied at evaluation time, not preregistered; independence
of the holdout is user-attested, not proven. No fitting, calendar reconstruction,
training-data audit, uncertainty intervals or automatic calibration is performed.
Users must supply a scenario and starting state aligned to the historical period.

The synthetic demonstration perturbs this run's revenue by 5%. It is intentionally
circular fixture data with an obvious mismatch, never empirical validation.
Actual Ankura data has not been supplied or evaluated in this increment.

Evaluations are saved under `artifacts/tr-ui-state/historical-evaluations/` with
the dataset, thresholds, full errors, data hash, run/model identities and receipt
hash. **Load latest saved evaluation** rechecks the receipt and run artifacts.
Exports retain that evidence; changing the input clears the previous verdict.

Project routes (local host guards and JSON body limits apply):

| Method / route | Purpose |
|---|---|
| `GET /project/validation` | Current model, run catalog and earlier evidence |
| `GET /project/evidence?run_id=…` | Verify and inspect selected-run evidence |
| `GET /project/playback?run_id=…&step=1` | Period observations; step is 1, 3 or 12 |
| `POST /project/history` | Evaluate `{run_id, scenario, dataset}` and save receipt |
| `GET /project/history?run_id=…` | Recheck and retrieve saved evaluations |

The earlier validation workspace did not change shared platform code. The later
live workspace adds the synchronous native SD observation barrier described above;
T&R equations and shared HTTP service implementation remain unchanged.
