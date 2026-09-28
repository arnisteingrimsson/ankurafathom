# Correctness workload measurements

`devstone-correctness-debug.csv` records one local run of the 48 DEVStone
conformance fixtures. It is an informational baseline, with no timing threshold
and no claim about production throughput or relative engine performance.

Environment: macOS/Darwin arm64, Apple Clang 21.0.0
(`clang-2100.1.1.101`), C++20, CMake Debug, sanitizers off. Reference adevs commit:
`ee9fed91224ed412088a3795ea8e67ffe2ee9b59`. DEVStone topology source and exact
workload are pinned in the [fixture documentation](../tests/conformance/devs/README.md).

Each duration is measured with `std::chrono::steady_clock`, in whole
microseconds. It includes graph construction, model allocation, the entire run
until passive, trace collection, and trace canonicalization. Native nested
includes production hierarchy compilation; native flat independently expands
the routes. Adevs retains boundary pin routes and uses extra passive collectors
for root outputs. The fixture itself is constructed before timing; comparisons
and analytic count assertions are outside the timed intervals. Runs occur in
the fixed order nested, flat, adevs, without warmup or repeated sampling. These
costs and instrumentation differ, so do not interpret ratios as kernel speedups.

`atomics` excludes coupled boundary nodes and reference root collectors;
`steps` counts distinct microsteps; `outputs` counts atomic emitted messages;
`trace_equal` is written only after all three runs agree and count oracles pass.

## Allocation baseline

[devstone-allocations-debug.csv](devstone-allocations-debug.csv) records successful
C++ allocation requests and total requested bytes for the same 48 cases and
three execution paths. [The metadata](devstone-allocations-debug.metadata.json)
records the environment, build flags, source hashes, CSV hash, and repeat check.
All allocation counts and byte totals agreed exactly across two local executions
of this build; timings varied. Instrumented and ordinary runs also agreed on
case IDs, atomic counts, event-step counts, output counts, and all trace oracles.
The repeated measurements do not add new conformance cases.

`devs_allocation_bench` builds the same DEVStone source with a scoped counter.
Only this executable and `allocation_counter_tests` link replacement global
`new`/`delete` operators. The production runtime and ordinary conformance binaries
retain their normal allocators. The counter uses `malloc`/`posix_memalign` and
`free`, supports scalar, array, aligned, sized-delete, and nothrow forms, and
counts each successful allocation once. A zero-byte request counts as one
allocation requesting zero bytes, while reserving at least one physical byte.
Counter overflow is reported as an instrumentation error.

Each current-thread scope starts immediately before its `run_native` or
`run_reference` call and ends after the returned trace is constructed. It includes
graph/model construction, routing compilation, simulation, simulator teardown,
trace recording, and canonicalization. Fixture generation, inter-engine
comparison, analytic assertions, CSV writing, and destruction of the returned
trace are outside the scope. The CSV columns `{nested,flat,adevs}_allocations`
and `{nested,flat,adevs}_requested_bytes` describe these complete workloads,
including harness costs. They do not isolate kernel hot-path allocations.

The metric excludes direct C allocation calls, allocator bookkeeping, allocations
before/after the scope, and allocations on other threads. It measures neither
live nor peak heap usage, retained memory, nor leaks. A future parallel workload
would need per-worker scopes and explicit aggregation. The recorded baseline is
from the nonsanitized Debug build; allocator interception adds overhead, so use
the separate ordinary timing report when examining uninstrumented timings.

For example, native nested `HOmod_w8_d4_ta1` recorded **208,117 requests** totaling
**17,850,289 requested bytes** across construction and execution of 106 atomics,
1,179 steps, and 26,589 atomic outputs. These are observations for the recorded
toolchain and workload, not limits or performance acceptance criteria.

`allocation_counter_tests` checks exact hand-counted totals for allocation forms,
alignment, zero-size requests, deallocation exclusion, nested inclusive scopes,
restoration after exceptions, and independent thread scopes. `devs_allocation_report`
runs all existing DEVStone trace and analytic gates while writing the allocation
CSV. There is no threshold or comparison against the saved allocation baseline.

Reproduce into a new report:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target devs_devstone_tests
./build/devs_devstone_tests build/devstone-timings.csv
cmake --build build --target devs_allocation_bench allocation_counter_tests
./build/allocation_counter_tests
./build/devs_allocation_bench build/devstone-allocations.csv
```

CTest writes both build-directory reports. Update the saved baselines and
metadata deliberately after a validated change; timing and allocation volume
are never correctness gates. The instrumentation currently targets the project's
macOS/Linux toolchains and measures C++ allocation calls that reach the replacement
operators; optimizer-elided allocations do not produce requests to count.
