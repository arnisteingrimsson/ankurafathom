# AI Opportunity Lab — Equinix discussion project

**Question:** Which combination of commercial demand, solution-design resources,
partner options and site readiness converts AI opportunities into active customers?

This is a standalone project UI and native model using AnkuraFathom. It is not a
platform UI, an Equinix forecast, or a hardware benchmark. Three fictional sites
start empty. All numerical business inputs are illustrative assumptions. The
meeting's broader Prism, Forge and managed-endpoint context does not add runtime
dependencies or integrations to this project.

## Start locally

From the repository root, using the existing Python runtime and a C++20 compiler:

```sh
.venv-runtime/bin/python projects/equinix-ai/build.py
.venv-runtime/bin/python projects/equinix-ai/server.py
```

Open <http://127.0.0.1:8088>. The host binds only to loopback; do not expose it as a
production service. It does not start the T&R project. Stop with Ctrl-C. Native
binaries are ignored under `native/build/`; run records are under the repository's
ignored `artifacts/equinix-ai-state/` directory. `--port` and `--state` are optional.

## Meeting walkthrough

1. Open **Assumptions & rules** and agree on the business question. Inputs describe
   hypothetical opportunities, effective delivery teams, and infrastructure.
2. Return to **Live studio**, press **Play**, and pause as a queue develops. Select
   a customer to inspect its requirements, deadline, state and blocking reason.
3. Inspect a site: reserved power, activated power and actual draw differ. Revenue
   begins at activation, not at pipeline entry or capacity reservation.
4. Reset to try another definition, or use **Compare strategies** for six strategies
   across shared seeds. More pipeline can increase abandonment if downstream
   resources cannot absorb it. An infrastructure investment can remain unused if
   the limiting resource is solution design. These are hypotheses to test, not
   findings about Equinix.
5. Show **Validation & context**: the model register, technical verification receipt,
   explicit boundaries and observation comparison. Export a definition and a run.

Nothing is simulated automatically on page load. Play repeatedly requests new
native computation. Pause stops further requests; an already requested step may
finish. Step advances the model once. Advance (6 hours, 1 day, 1 week) is the
observation interval; Pace is the wall-clock delay between requests. Neither is
the SD integration step, which is a separate assumption. Native event timestamps
remain exact to floating-point precision and integration stops at each event.
There is no prerecorded animation. Hiding the browser tab pauses live stepping.
Explicit comparison experiments run in the background until completed/cancelled.
Definitions freeze after initialization; Reset permits edits and clears results.

## Models and coupling

| Method | Implementation | Role |
|---|---|---|
| ABM | `SyncPopulation<Customer>` | Typed customer agents with size, budget, preferred site/provider, flexibility and deadline. Their behavior executes through the event model, not a separate synchronous tick. |
| DES | `devs::Simulator` with a project `Market` atomic model | Poisson arrivals; qualification, design and installation queues; equipment lead times; reservations; abandonment; capacity-change events. |
| SD | `sd::Model`, Euler | Revenue, operating cost and facility energy accumulated between events; separate investment stock. Rates are constant within each event interval in this version. |
| Hybrid | Project-owned C++ coupling | Individual requirements govern allocation; activation changes financial/energy rates; deadlines release work and capacity. |

All run on the CPU. This project uses the native C++ interfaces, not the generic
declarative IR. `spec.py` is the validated project configuration contract.

The event order for simultaneous timestamps is upgrade, qualification completion,
design completion, equipment arrival, deadline, opportunity arrival, installation
completion. Thus an installation must finish **strictly before** its deadline.
Deadlines immediately cancel unfinished work and free its lane. Stale completions
are ignored. Queues use arrival order; incompatible capacity requests may be
bypassed by later feasible requests. A reservation holds racks, cooling positions
and IT power through equipment procurement and installation. Active customers do
not leave within this model's horizon.

### Explicit numerical assumptions

- Enterprise and distributed-inference deployments use 1–2 rack blocks;
  high-density deployments use 3–6 and require liquid-capable positions.
- Customer budgets are uniform over `blocks × 40 × [200,400]` USD/month. The
  reference 40 kW is fixed in the customer demand rule, independent of later edits
  to provider power. Budgets and these size distributions require model-code changes;
  they are disclosed rules, not UI controls.
- Patience is uniform over 0.75–1.25 times its configurable mean. Preferred site
  and provider are uniform draws. Traits and decisions use separate Philox streams
  addressed by customer ID, preserving paired randomness between strategies.
- Design takes `design_days × (0.75 + 0.25 × blocks) × (1 − design_saving)`.
  If assistance is enabled, rework adds `0.5 × design_days` with the configured
  probability. Installation takes `installation_days × blocks`.
- First feasible site/provider wins, in preference order. Enabled profile, budget,
  maximum rack density, racks, liquid positions and reserved IT power are checked.
  Alternatives require both the global policy and individual customer flexibility.
- Upgrade is a fixed package: +800 IT kW, density at least 80 kW/rack, +12 liquid
  positions (capped at total racks), at the specified day and investment cost.
- Revenue/day = active reserved IT kW × monthly price / 30.
  Facility kWh/day = active IT kW × draw ratio × PUE × 24.
  Operating cost/day = overhead + all service lanes × lane cost + energy × tariff.
  Contribution = revenue − operating cost. Cash proxy also subtracts the upgrade.
  Hardware purchase, depreciation, financing, tax, and a full contract cash-flow
  model are absent. Starting from no installed base matters when reading losses.

NVIDIA, AMD, Qualcomm, Tenstorrent and Other partner start with identical **assumed**
40 kW/block and 14-day procurement inputs. These are editable profiles, not measured
device specifications, comparative performance, or a claim of software portability.
Network latency, thermal physics, endpoint inference queues, outages and post-sale
churn are outside this version. Sites are fictional; no private company data is used.

## Validation and reproducibility

Each computed frame shows 23 accounting/resource checks, including customer
conservation, reservation and rack ledgers, resource ownership/capacity, independent
per-customer revenue accounting, and energy/cost integration. These checks verify
specific model mechanics, not the realism of assumptions. The observation log keeps
the most recent 50 events per frame; it is not a lossless all-event trace.

```sh
.venv-runtime/bin/python projects/equinix-ai/build.py --sanitize
.venv-runtime/bin/python projects/equinix-ai/acceptance.py
```

Acceptance runs the nine project tests on both normal and AddressSanitizer/
UndefinedBehaviorSanitizer binaries, then six strategies × three seeds over 120
days. It verifies all exported run hashes and recomputes one entire daily trajectory
per strategy, comparing every frame exactly. A corrupted-record negative control
must fail. The tests also cover independently hand-calculated schedules and
economics, cancellation, zero demand/capacity, deterministic repetition, mismatched
observation units, and invariance to different observation/integration intervals.

The receipt, test logs, comparison and full runs go to
`artifacts/equinix-ai-acceptance/`. The UI reports whether the receipt's source
hashes still match. It is project-level evidence, not a rerun of all engine tests.

Exported records include the exact definition, seed, build identity and source
hashes, frame hash chain, recorded checks and whole-record hash. Build receipts
conservatively capture all repository C++ headers and the JSON header, plus compiler
flags/version and platform. Preserve the JSON bytes as exported. To verify or replay:

```sh
.venv-runtime/bin/python projects/equinix-ai/verify_record.py /path/to/equinix-ai-run.json
.venv-runtime/bin/python projects/equinix-ai/verify_record.py /path/to/equinix-ai-run.json --replay
```

Exact replay requires the recorded binary and current matching source receipt.
Cross-compiler bitwise reproducibility is not claimed. Hashes establish internal
consistency, not third-party authenticity. Records can be inspected without rerunning.

### Later operational validation

The UI accepts JSON observations with a named source/vintage, `kind` of `observed`
or `synthetic`, and rows containing `time`, `metric`, `unit`, `actual`, and absolute
`tolerance`. Download its template. Time is calendar days from the agreed start,
and must match a computed frame. Metric units are in `/api/catalog`.

Compare opportunity arrival counts, time in stages, wins/losses, activations,
reserved capacity, revenue and energy against a held-out period once available.
This version supports point comparisons with user-specified tolerances, not
automatic fitting, initial-state reconstruction, a completed historical backtest,
or causal validation. Calibration and holdout selection still need an explicit plan.

## Project HTTP contract

`GET /api/catalog`, `POST /api/validate`, `GET /api/verification`.
`POST /api/sessions` takes the complete definition and returns an empty day-zero frame.
`POST /api/sessions/{id}/step` takes `{ "expected_revision": 0, "days": 1 }`.
`GET /api/sessions/{id}/state|record`; `POST /api/sessions/{id}/stop` takes `{}`;
`POST /api/sessions/{id}/evidence` accepts the observation payload above.
Step retries with the immediately preceding revision return the committed result.

`POST /api/experiments` takes `{ "config": <definition>, "variable": "strategy",
"values": [0,1,2,3,4,5], "replications": 3 }`.
Poll `GET /api/experiments/{id}`; cancel with
`POST /api/experiments/{id}/cancel` and `{}`. At most one experiment runs at once.
Results show means and observed ranges, not confidence intervals. Strategies apply
their changes to the supplied definition; strategy 0 leaves it unchanged. A combined
strategy changes multiple controls and cannot isolate any one causal effect.

## Public context

- [Equinix's AI partner ecosystem](https://blog.equinix.com/blog/2026/09/24/your-ai-strategy-depends-on-partners-heres-how-to-find-them/)
- [Equinix and NVIDIA](https://www.equinix.com/partners/nvidia)

These motivate the discussion only; none of the numerical assumptions is calibrated
from these pages. Other provider names follow the user-supplied meeting context.
