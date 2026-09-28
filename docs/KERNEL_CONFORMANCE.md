# M1 kernel conformance report

**M1 is complete for the declared CPU kernel contract.** The §7.1 correctness
suite passes against the pinned adevs reference, and both timing and allocation
measurements are recorded. This establishes the tested event-kernel subset; it
does not complete Phase 1 or imply support for every DEVS feature. Allocation
volume and timing remain nongating diagnostics.

## Acceptance evidence

| Requirement | Evidence | Result |
|---|---|---|
| Reference builds and executes | `adevs_reference_schedule` | Pass |
| Original differential coverage | `devs_differential`: five fixed cases and 64 deterministic graphs | Pass |
| GPT | `devs_conformance`: four exact completions at 2.5, 4, 5.5, 7; eight transducer messages | Pass |
| Both confluent policies | Same-time input 3 and pre-state 10 produce subsequent state 23 or 26, as explicitly selected | Pass |
| Ping-pong and zero-time chains | Six microsteps at time zero; 64-atomic chain with 65 steps at time 0.5 | Pass |
| Nested/flat equivalence and fan-out | Three-way comparison for 48 DEVStone and 30 other static cases, including 24 larger graphs and duplicate payloads on distinct paths | Pass |
| Dynamic add/remove and rewiring | Two native/adevs boundary-edit cases; insertion clock, due-output boundaries, replacement, cancellation, disconnect/reconnect | Pass |
| Root passthrough | Three nested/flat/adevs cases: empty root, empty nested routes with duplicate payloads, and same-time atomic confluence | Pass |
| Dynamic nested structure | One nested/independently flat/adevs scenario: busy subtree replacement, nested insertion, root rewiring, future-input redirection, historical boundary results, and recursive retirement | Pass |
| Edit rejection and root retry | Native invariants: unreachable boundary cycle, invalid child/parent, missing edge, invalid insertion schedule, stable IDs, surviving deadlines, callback guards, root-event rollback, and dropped routes | Pass |
| Native deterministic order | Distinct payloads verify injection order, source IDs, output positions, destination ports; forbidden mutation leaves next-step routing intact | Pass |
| Zero-time cycle diagnosis and invalid schedules | Existing `devs_kernel` tests | Pass |
| LI, HI, HO, HOmod | `devs_devstone`: 48 configurations, canonical trace equality plus analytic message/transition counts | Pass |
| Timing recorded, ungated | [DEVStone CSV](../bench/devstone-correctness-debug.csv) | Recorded |
| Allocation count measured, ungated | [48-case allocation CSV](../bench/devstone-allocations-debug.csv), environment/source-hash metadata, and exact counter self-tests; repeat counts/bytes agree | Recorded |

There are **153 differential cases**: 69 original, 48 DEVStone, 30 additional
static cases, two flat dynamic cases, and four root/nested hierarchy cases.
This count excludes native-only invariants
and is distinct from the CTest test count. The newer 84 cases compare complete
canonical step traces; the original 69 retain their existing applicable trace
comparisons.

The conformance-directory entries, allocation counter self-test, and instrumented
DEVStone report run automatically through the existing CMake/CI workflow.
The instrumented report repeats 48 existing cases; it does not raise the count
of distinct differential cases. At M1 closure, local verification on macOS arm64 with Apple Clang 21.0.0 was:

- Debug build: **57/57 CTest tests pass**.
- Debug with AddressSanitizer and UndefinedBehaviorSanitizer: **57/57 pass**.

These are local results. The workflow is configured for macOS and Linux; this
report does not claim a new remote CI run or Linux result.

## Comparison boundary

The new harness compares exact timestamps, per-component outputs, input bags,
transition kinds, elapsed times, post-transition state, next time advances, and
root outputs. It preserves each microstep and duplicate message. Simultaneous
component records and unordered bags are canonicalized because the engines have
different callback iteration orders. Native order is tested separately. Shared
atomic behavior makes this an oracle for scheduling and routing, with hand
schedules and analytic counts checking selected model behavior independently.

The reference is the unchanged local adevs snapshot at
`ee9fed91224ed412088a3795ea8e67ffe2ee9b59`; it is not the runtime engine or a hosted
fork. Detailed workload definitions, source provenance, and normalization rules
are in [the fixture documentation](../tests/conformance/devs/README.md).

## Kernel follow-on work beyond the accepted M1 subset

- A broader rollback contract for custom atomics without `clone`; checked
  execution currently rejects them before stepping.

Allocation measurements cover whole sequential conformance runs, including model
construction and trace instrumentation. They count successful current-thread
C++ requests and requested bytes, not live/peak heap usage. The
[measurement contract](../bench/README.md#allocation-baseline) records exact scope
and limitations. Kernel-only phase attribution and parallel-worker aggregation
can be added when later performance work needs them.

Root passthrough and compiled nested edits are implemented with the
[documented contract](SEMANTICS.md#compiled-hierarchy-edits). Edits are separate
between-step operations. Multi-operation structural transactions, reparenting,
and model-requested structural transitions remain outside this API.

SD, DES, ABM, IR, experiment/replay, and broader hybrid acceptance work remains
tracked in [STATUS.md](STATUS.md). These conformance results do not close those
milestones.
