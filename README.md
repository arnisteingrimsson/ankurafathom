# AnkuraFathom

AnkuraFathom is a simulation platform for understanding how people, processes and
resources interact—and how decisions change their behavior over time. It runs on
CPUs using a C++20 engine and combines agent-based modeling, discrete-event
simulation and system dynamics in one platform.

Use Fathom to explore questions such as:

- How does AI adoption change workload, staffing, service delivery and revenue?
- Does growth depend on more demand, different skills, faster delivery or additional capacity?
- Where do queues and bottlenecks form, and which investment would relieve them?
- How do individual decisions accumulate into organization-wide outcomes?

A model makes the proposed explanation explicit: who acts, what rules they follow,
what resources they need, and what changes as time passes. Simulation then lets
you test that explanation and compare alternatives, including situations for
which no historical outcome exists.

**Current direction:** a reproducible CPU simulation foundation, an inspectable
workspace for humans and AI assistants to develop models together, and focused
project applications for decision makers. Accuracy and validation come first;
performance optimization and Tenstorrent execution are later stages. Fathom is
standalone and currently operates independently of AnkuraPrism and AnkuraForge.

[Try the visual workspace](#try-the-visual-workspace) ·
[Run a model](#build-and-run-the-command-line-engine) ·
[Explore the projects](#example-projects) ·
[Review validation](#validation-and-trust)

## The modeling methods

Different questions need different representations. Fathom provides three methods
and explicit connections between them.

| Method | What it represents | Example | Implemented capabilities |
|---|---|---|---|
| **Agent-based modeling (ABM)** | Individuals with their own attributes, state and behavior. Their interactions produce population-level outcomes. | Employees differ in skills, availability and AI proficiency; customers differ in requirements and patience. | Typed agents, synchronous and asynchronous behavior, statecharts, messaging, spatial/network interactions, births and retirements. |
| **Discrete-event simulation (DES)** | Work moving through a process, with changes at particular times. Queues emerge when demand exceeds available resources. | An opportunity waits for a solution designer, reserves capacity, then moves through procurement and installation. | Event scheduling, queues, resources, priorities, routing, service processes and scheduled capacity changes. |
| **System dynamics (SD)** | Quantities that accumulate, the rates that change them, and feedback over time. | Backlog grows with incoming work and falls with completions; revenue accumulates as services are delivered. | Stocks, flows, equations, lookup tables, delays, external inputs, and Euler, midpoint and RK4 integration. |
| **Hybrid modeling** | Connections between individual behavior, event-driven processes and continuous state. | Employees supply delivery capacity; projects occupy and release employees; completions update financial stocks. | Native bridges for staffing, population aggregates, event-to-stock updates, continuous agent state and SD-driven event rates, with selected declarative formats. |

ABM agents are **simulated people or entities**. AI assistants that help build a
model are authoring tools; they are separate from those simulated agents.

You do not need to measure a queue before modeling it. Define arrivals, service
times, routing and resource availability, and let the queue develop. Observations
can later test whether those assumptions reproduce the behavior of the real
system. The same principle applies to adoption, learning, hiring and other
mechanisms—provided the model's rules and uncertainty are made explicit.

The native C++ interfaces offer broader composition than the current JSON model
formats. Supported formats validate their contracts and reject unsupported
configurations. Arbitrary combinations of all features are not yet available
through a general visual or declarative editor.

## From a question to a decision

1. **Define the problem.** Agree on the decision, system boundary, time horizon
   and outcomes that matter.
2. **Describe the mechanisms.** Identify entities, processes, resources, stocks
   and feedback. Choose ABM, DES, SD or a combination to represent them.
3. **Make assumptions inspectable.** Define parameters, distributions, rules,
   units and initial conditions. Start with explicit estimates where data is
   unavailable; record which inputs are assumed, measured or calibrated.
4. **Validate the model.** Check small cases with known answers, accounting and
   capacity constraints, numerical behavior and reproducibility. When observations
   are available, calibrate on one period and evaluate against a separate period.
5. **Explore alternatives.** Change policies or investments, vary uncertain
   assumptions, run multiple seeds where appropriate, and inspect what causes
   outcomes to differ.

Historical fitting and causal validation require a project-specific plan. Fathom
provides execution, checks, provenance and selected observation-comparison tools;
it does not automatically establish that a business model is correct.

## Platform, workspace and projects

| Layer | Responsibility |
|---|---|
| **Simulation engine and runtime** | Execute model behavior on CPUs; manage time, random draws, experiments, data interfaces and reproducible results. |
| **Application layer and control center** | Expose execution APIs; switch projects; inspect definitions, assumptions, validation evidence and traces; review configuration changes. |
| **Project models and customer UIs** | Define the business question, domain rules, inputs and decision-making experience for a particular use case. |

The [control center](application/control_center/README.md) is the operator
workspace. Each customer application remains part of its project.

The first **visual model workspace**, available for the Equinix discussion project,
connects agents, delivery processes, resource constraints, financial stocks and
supplemental evidence on one canvas:

- **Define:** select components and connections, inspect their rules and bindings,
  edit parameters, and review changes before accepting a configuration version.
- **Run:** Play or Step requests new native computation. The canvas shows computed
  agent states, queues and economics. No outcomes appear before execution.
- **Validate:** inspect checks mapped to components and compare named observations
  against already-computed results, with explicit units and tolerances.

The visual workspace currently edits configuration for an implemented native
model. Adding structural behavior still requires model implementation and a
reviewed visual mapping. Humans and external AI builders can submit proposals
through the same local API. Selecting and managing the AI builders themselves is
future work.

## Install and get started

The source-build paths below target **macOS and Linux**. You need:

- Git and a C++20 compiler available as `c++`.
- Python **3.10 or later**, with virtual-environment support.
- CMake **3.20 or later** for the command-line engine and optional Python bindings.

The console and Equinix walkthrough use Python's standard library and plain
browser JavaScript; they do not require a Node.js build or an LLM API key.
Arrow/Parquet and the native Python bindings have additional dependencies below.

Clone the repository and create the local environment:

```sh
git clone https://github.com/arnisteingrimsson/ankurafathom.git
cd ankurafathom
python3 -m venv .venv-runtime
```

Run the remaining commands from the repository root. The explicit Python path
means you do not need to activate the environment.

### Try the visual workspace

Build the Equinix project's native model, then start the control center:

```sh
.venv-runtime/bin/python projects/equinix-ai/build.py
.venv-runtime/bin/python -m application.control_center.server
```

Open **<http://127.0.0.1:8086>**.

1. Select **AI Opportunity Lab** and open **Visual workspace** to inspect the model.
2. Open **Runtime** and click **Start customer service**.
3. Return to **Visual workspace → Run** and press **Play** or **Step**.
4. Use **Open customer UI** to explore the separate project application at
   <http://127.0.0.1:8088>.

Browsing or accepting a definition does not start a simulation. Pause stops
subsequent computation requests; an in-flight step can finish. The observation
interval, animation pace and SD integration step are distinct controls where
exposed by the project. DES events retain their scheduled simulation times.

These services are local-only. Stop the console with **Ctrl-C**; it also stops
project services it started. It does not automatically start T&R.

A fresh checkout has no local run history or project acceptance receipt. Generate
verification evidence using the [project's validation instructions](projects/equinix-ai/README.md#validation-and-reproducibility).
T&R needs additional generated inputs and build steps; follow its
[setup guide](projects/tr-ui/README.md) before starting that service.

### Build and run the command-line engine

For a native build with CSV output and no Arrow dependency:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DFATHOM_ENABLE_ARROW=OFF
cmake --build build --target fathom --parallel 2

./build/fathom lint models/decay.ir.json
./build/fathom run models/decay.ir.json \
  --experiment models/decay.experiment.json \
  --out build/decay.csv
```

This example simulates a declining stock under two rate assumptions and writes
its observations to `build/decay.csv`. File runs create a manifest by default,
recording inputs, scenario configuration and result identity.

Verify the saved result or recompute it from the manifest:

```sh
./build/fathom verify-results build/decay.csv.manifest.json --results build/decay.csv
./build/fathom replay build/decay.csv.manifest.json --threads 1
```

Other starting points include [typed DES queues and resources](models/typed_resource_process.ir.json),
[ABM statecharts](models/typed_abm_async.ir.json), and
[staffed delivery connected to financial stocks](models/agent_pool_sd.ir.json).
Pass their paths to the same `fathom lint` and `fathom run` commands.

### Add Arrow/Parquet and Python bindings

Install the pinned runtime dependencies and build the optional interfaces:

```sh
.venv-runtime/bin/python -m pip install -r runtime/requirements.txt

cmake -S . -B build-arrow -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DFATHOM_ARROW_PYTHON="$PWD/.venv-runtime/bin/python" \
  -DFATHOM_ENABLE_PYTHON=ON \
  -DPython_EXECUTABLE="$PWD/.venv-runtime/bin/python"
cmake --build build-arrow --target fathom fathom_python --parallel 2

./build-arrow/fathom run models/decay.ir.json --out build-arrow/decay.parquet
PYTHONPATH=build-arrow/python .venv-runtime/bin/python
```

The last command opens Python with the build-directory package available:

```python
from pathlib import Path
from ankurafathom import Model, run

path = Path("models/decay.ir.json").resolve()
model = Model.from_json(path.read_text(), base_directory=path.parent)
results = run(model, provenance=True)
print(results.to_pydict())
```

This build requires Python development headers. Keep the selected virtual
environment available: the native Arrow-enabled build links to its PyArrow SDK.
For local wheels, editable installation or an installed C SDK/CLI, see
[packaging](docs/PACKAGING.md). See the [Python API](docs/PYTHON_API.md) and
[C API](docs/C_API.md) for interface contracts.

## Example projects

These are executable demonstrations with synthetic business assumptions, not
calibrated forecasts or established estimates of AI's impact.

| Project | Decision question | Start here |
|---|---|---|
| **AI Opportunity Lab — Equinix discussion** | Which combination of demand, design resources, partner choices and site capacity turns opportunities into operating customers? | [Native hybrid model and customer UI](projects/equinix-ai/README.md); [visual control center](application/control_center/README.md). Provider profiles are illustrative, not hardware benchmarks. |
| **T&R Decision Lab — Ankura practice growth** | How do workforce skills, AI adoption, delivery constraints and business development affect a 150-person practice? | [Interactive project](projects/tr-ui/README.md) and [operating-model workshop](projects/tr-ui/workshop/README.md). |
| **T&R monthly economics pilot** | How do level-specific productivity, hiring/attrition, fee structures and cash timing change practice economics? | [Monthly pilot](examples/tr_pilot/README.md), [results and limits](examples/tr_pilot/RESULTS.md). |
| **Synthetic Ankura preview** | What happens under all 16 combinations of Copilot, tool building, process automation and acquisition across two practices? | [General-platform example](examples/ankura_pilot/README.md). |

For smaller models with independently calculable answers, begin with the
[worked validation examples](examples/worked_validation/README.md).

## Execution, data and reproducibility

- **Experiments:** scenario combinations, grid/Latin-hypercube/Sobol designs,
  reproducible random draws, multithreaded ensembles, progress and cancellation.
- **Data:** validated local CSV, Arrow and Parquet inputs for supported parameter,
  time-series and population bindings; typed fields and unit checks.
- **Interfaces:** native C++, CLI, C ABI, optional Python bindings, and a local
  HTTP application layer with [OpenAPI](application/openapi.json) and clients.
  Project live sessions have their own documented adapters.
- **Evidence:** input/result identities, run manifests, saved-result verification
  and portable replay bundles; selected accounting and behavioral checks.
- **Inspection:** source and configuration views, recorded observation traces,
  SD stock/flow diagrams and explanations of supported SD results.

Exact replay is subject to the recorded execution/build contract. A saved trace
is distinct from live execution and may contain observation snapshots and recent
event windows rather than every event. See [execution semantics](docs/SEMANTICS.md),
[run manifests](docs/RUN_MANIFESTS.md) and [portable replay](docs/PORTABLE_REPLAY.md).

## Validation and trust

Fathom separates three questions:

1. **Does the engine implement its algorithms correctly?** Evidence includes
   analytical cases, independent implementations, numerical convergence tests,
   regression suites and memory/undefined-behavior sanitizer checks.
2. **Does this model implement its stated rules?** Project checks test accounting,
   resource ownership, event ordering, reproducibility and known-answer cases.
3. **Do those rules represent the real system well enough for this decision?**
   That requires operational evidence, sensitivity analysis, calibration and
   appropriately separated historical or prospective validation.

Passing the first two does not establish the third. Current business examples
still contain synthetic inputs and intervention effects. Test counts and scope
change as development progresses; [STATUS.md](docs/STATUS.md), the
[benchmark inventory](tests/benchmarks/README.md) and project verification receipts
record what was assessed and what remains open.

The C++ simulation engine is Fathom's own implementation. A
[pinned adevs snapshot](third_party/adevs/UPSTREAM.md) serves as an independent
reference for event-kernel comparisons.

To build and run the engine's test configuration separately from the quick start:

```sh
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Debug -DFATHOM_ENABLE_ARROW=OFF
cmake --build build-tests --parallel 2
ctest --test-dir build-tests --output-on-failure
```

For the control center and visual-definition contracts:

```sh
.venv-runtime/bin/python -m application.control_center.test_console
.venv-runtime/bin/python -m application.control_center.test_workspace
```

Project, Arrow/Python and sanitizer validation have separate build requirements;
follow the corresponding project and interface documentation.

## Documentation and development direction

| Topic | Documentation |
|---|---|
| Current implementation and remaining work | [Status](docs/STATUS.md), [implementation plan](IMPLEMENTATION_PLAN.md) |
| Operator workspace and visual-definition API | [Control center](application/control_center/README.md) |
| Building project applications | [Application-layer handoff](docs/APPLICATION_LAYER.md), [service setup](application/README.md) |
| Model definitions and execution rules | [Semantics](docs/SEMANTICS.md), [typed ABM](docs/ABM_TYPED_IR.md), [typed DES](docs/DES_TYPED_IR.md), [SD validation](docs/SD_VALIDATION.md) |
| Hybrid composition | [Bridge contracts and acceptance scope](docs/M5_ACCEPTANCE.md) |
| Data and experiments | [Data inputs](docs/RUNTIME_DATA.md), [experiment execution](docs/RUNTIME_EXPERIMENTS.md), [scenario designs](docs/RUNTIME_SCENARIO_DESIGNS.md) |
| Model checks and explanations | [Declared validation](docs/DECLARED_VALIDATION.md), [SD explanations](docs/EXPLANATIONS.md), [diagrams](docs/VISUALIZATIONS.md) |
| Developing a pilot with an AI assistant | [Executable pilot workflow](docs/agent/README.md) |

The direction is a shared modeling workspace where people and AI assistants can
make assumptions visible, propose model changes, inspect execution and accumulate
validation evidence as a project develops. Broader visual authoring, richer
hybrid explanations, agent-management controls and real-data business pilots
remain work ahead. Accelerator execution follows the CPU correctness foundation.
