# AnkuraFathom control center

The operator console is the main project workspace. Customer applications remain
separate project-owned UIs, opened through **Open customer UI**. This follows the
console / Delivery separation in AnkuraPrism without adding a Prism dependency.

From the repository root:

```sh
.venv-runtime/bin/python -m application.control_center.server
```

Open <http://127.0.0.1:8086>. Equinix and T&R are registered in `registry.py` with
explicit local source, artifact and customer-service locations. Existing customer
URLs remain <http://127.0.0.1:8088> and <http://127.0.0.1:8087> respectively.
`--port` and `--state` customize the console host/state; project service addresses
are configured in the registry. No simulation or project service starts on browsing.

## Views

- **Overview:** question, components, parameters, saved-run count, readiness and
  operator notes. Notes are versioned and do not modify simulation inputs.
- **Visual workspace:** Equinix's connected agents, delivery process, resources,
  financial stocks and supplemental evidence targets. Define, Run and Validate
  use the same canvas; select a component or connection to inspect its bindings.
- **Model register:** actual ABM, DES, SD and coupling descriptions; complete JSON
  contract; current native implementation and configuration adapter with hashes.
- **Configuration:** searchable parameters, bounds, units and evidence status;
  structured definition tree; raw JSON and export. These are current defaults,
  distinct from each run's captured configuration.
- **Data & assumptions:** explicitly assumed inputs and operational validation gaps.
- **Runs & traces:** paginated customer and acceptance records; original-record
  download; captured configuration, methods, seed, source/binary identities and
  hash verification; inspect individual recorded frames and their events, agent
  state, metrics and checks.
- **Validation:** the actual project verification receipt, source freshness,
  recorded checks and the limits of that evidence.
- **Runtime:** native build identity and prerequisite checks, customer service
  health, explicit service startup and shutdown of console-owned processes only.
- **Customer UI:** launches the selected project's independent application in a
  new tab. This does not press Play or compute outcomes.

## Trace semantics and limits

The current projects use native C++ behavior with JSON configuration contracts.
The console does not claim the JSON is a complete declarative model. It shows
both layers. Current source inspection is not a historical source archive; saved
records retain historical source identities and build metadata.

Run inspection reads saved observation frames. It does not replay or execute
simulation steps. The native project frames contain recent-event windows (50
events for Equinix, 60 for T&R); events between observations may be absent.
This is **not a lossless event trace**. Full frame JSON exposes exactly what was
retained, including agents/resources and chained hashes. AI-agent construction
reasoning and tool-call traces are not currently registered and are labeled absent.

Integrity checks recompute the whole-record, configuration and frame-chain hashes.
They establish consistency, not third-party authenticity or empirical correctness.
Saved statuses are artifact statuses, not assertions that a process is still alive.
Only runtime health queries report current customer-service availability.

Project contracts and source files are read directly from their registered
locations. Missing, stale or malformed evidence is surfaced rather than replaced
with sample results. Metadata is cached by file timestamp/size; unchanged source
hashes are cached to keep browsing responsive. Run lists paginate 20 records at a
time. Records are not copied to a second database or mutated by inspection.

## Local service lifecycle

The console binds only to loopback and checks Host/Origin through the shared
application HTTP adapter. This is a trusted single-machine operator tool, not an
authenticated hosted control plane. Service launch commands are fixed by the local
registry; browser requests cannot supply executables, arbitrary paths or shell text.
Only services started by this console can be stopped through it. Shutting down the
console also stops its owned services; pre-existing project services remain running.
T&R is not automatically started.

The visual workspace now supports versioned configuration editing and native
execution for Equinix. Customer applications retain their own configuration
controls and defaults. Accepting a workspace version does not change those defaults.
Generic structural model authoring, job scheduling and AI-agent orchestration are
not introduced here. A new project requires an explicit registry adapter and visual
mapping. T&R remains inspectable through the existing console views; its visual
mapping and live workspace adapter have not been registered.

## Visual definitions and collaboration

Open <http://127.0.0.1:8086/?project=equinix-ai#workspace>.

`projects/equinix-ai/visual-model.json` maps 17 components and 17 connections to
the actual native model. It declares parameter, table, state, metric and check
bindings, source identity, connection units and update timing. All 27 configuration
parameters are exposed through components or **Run settings**. This is an inspectable
projection of implemented behavior, not an executable free-form graph. Moving or
adding model structure requires native implementation and a reviewed visual mapping.
Native source changes make the mapping stale and block proposals/execution.

1. In **Define**, select a component and edit its inputs. Changes remain a working
   draft until **Review changes** shows the exact diff and **Accept version** records
   a new immutable configuration version. Contract validation does not establish
   realistic business effects. Stale proposals cannot overwrite a newer version.
2. In **Runtime**, start the Equinix customer service if it is offline. In **Run**,
   Play or Step creates a native session with the accepted configuration hash and
   advances it on demand. Opening the workspace or accepting a definition does not
   compute results. Pause stops subsequent requests; a current step can finish.
   Switching views/projects or hiding the browser pauses advancement. End/reset
   closes the native session; leaving/reloading the page attempts to close it.
3. **Advance** selects the observation request interval (six hours, one day or one
   week). It is distinct from `sd_dt`, the SD integration setting in Run settings.
   DES events still occur at their scheduled times. No prerecorded trajectory is
   used. Counts, queue/busy values, customer dots and financial values come from
   returned native frames; up to seven customer dots per stage are shown.
4. **Validate** maps the current frame's native checks onto components. Green means
   those checks passed, not that the business model is calibrated. Supplemental
   evidence nodes download a JSON observation template and compare uploaded point
   observations to already-computed timestamps. Source, observed/synthetic kind,
   matching units and tolerances are required. Comparisons are not fitting,
   causal validation or proof of independent holdout; no real dataset is attached.

Versions and proposals persist in
`artifacts/control-center/<project>-visual-workspace.json`. A
`visual-run-<session>.json` sidecar links each workspace execution to its accepted
revision, configuration hash and visual mapping hash. Native run records remain in
the project's normal artifact directory and appear in **Runs & traces**. Active
session ownership is local to the console process; restart creates new live sessions.

Humans and external AI builders can use the same review contract:

| Method | Route under `/api/projects/{project}/workspace` | Body / result |
|---|---|---|
| GET | root | Mapping, current accepted configuration, version metadata, proposals |
| POST | `/propose` | `base_revision`, full `config`, `author`, `note`; validates and returns a diff |
| POST | `/accept` | `proposal_id`; records the next version without running |
| POST | `/start` | `revision`; creates a native session at day zero |
| POST | `/runs/{id}/step` | `expected_revision`, `days` (0.25–7) |
| POST | `/runs/{id}/stop` | `{}` |
| POST | `/runs/{id}/evidence` | Existing project observation contract: `source`, `kind`, `rows` |

The UI proposal inbox exposes the supplied author and diff. Author names are local
labels, not authenticated identities. Builder-model switching and agent lifecycle
management remain future work.

## Tests

```sh
.venv-runtime/bin/python -m application.control_center.test_console
.venv-runtime/bin/python -m application.control_center.test_workspace
node --check application/control_center/web/console.js
node --check application/control_center/web/workspace.js
```

Tests cover build/receipt freshness, exact recorded-state inspection, tamper
detection, path and frame boundaries, pagination/cache invalidation, versioned
notes, fixed-command process ownership, HTTP trace/download routes and Origin
rejection. These tests do not rerun the simulation engine regression.

The five workspace tests additionally cover native mapping freshness, complete
parameter bindings, immutable accepted versions, stale/invalid proposals, project
capacity validation and execution of accepted configurations only. With both local
services running, `.venv-runtime/bin/python -m application.control_center.check_live_workspace`
checks the actual native day-zero/day-one boundary, configuration identity, positive
and deliberately wrong synthetic comparisons, and invalid-unit rejection. It writes
`artifacts/visual-workspace-acceptance/live-api.json` and closes its session.
