# Ordered CPU experiment execution

The runtime executes explicit scenarios with 1–256 requested worker threads.
Each trajectory owns a callback copy and private observations. Results merge in
scenario/replication order, and thread count never changes Philox addresses.
The runner validates finite observations and unique time/output pairs before
publishing a complete result. It supports at most 1,000,000 trajectories in one
invocation; existing 16-bit scenario/replication address bounds still apply.

`ExecutionOptions` adds a stop token and an optional progress callback. Progress
runs on the caller thread with an ordered validated-prefix count. Returning false
cancels. Cancellation and callback failures stop new task acquisition and join
in-flight work. `ExperimentCancelled` reports the merged prefix and total but
returns no partial result. Model callbacks must be pure and value-owned; they are
not forcibly interrupted while executing. Multiple trajectory failures report
the earliest canonical task, independent of completion order.

```sh
./build/fathom run models/stochastic_process.ir.json \
  --experiment models/stochastic_process.experiment.json \
  --threads 7 --seed 18446744073709551615 --out results.csv
```

The CLI accepts strict unsigned decimal options, rejects duplicate flags, and
writes a provenance sidecar and schema-0.2 lineage for file output by default.
Use `--no-manifest` for the legacy CSV schema; stdout remains unchanged. Without an experiment it runs one trajectory;
`--seed` overrides the default seed zero. With an experiment it overrides that
experiment's seed.

A result file is staged in a private sibling temporary file, flushed, closed and
atomically renamed after all trajectories succeed. Simulation failure preserves
an earlier result; input symlink/hard-link aliases are rejected. Temporary files
are removed after publication failure. This POSIX contract covers the supported
macOS/Linux hosts and does not claim directory-entry durability after power loss.
Standard output is buffered until simulation succeeds, although a later pipe error
cannot retract already written bytes.

Native tests compare 1,000 scenarios and three replications at 1/2/7/32 threads,
including exact decoded f64 values and summary statistics. They also check every
replication address through 65535, cancellation, caller-thread progress, callback
isolation, invalid observations and deterministic multi-failure selection. The
standalone test passes under ThreadSanitizer as well as normal and ASan/UBSan builds.

The CLI independently compares 1,000 stochastic scenarios and three replications
at 1/2/7/16 threads: all **81,000 CSV observations match byte for byte**. Seven
exposed model fixtures add cross-thread isolation checks for SD, adoption, continuous
agent stocks, async/rate ABM and typed DES. Seed overrides, option rejection,
failed ensembles, input aliases, temporary cleanup and prior-file preservation are
covered. CSV SHA-256:
`560a8857f5d85a9014f0327b9cc4e847db9da1469d96c81e2173255d7824c196`.

The fixed floating-point flags (`-ffp-contract=off -fno-fast-math`) now propagate
through the public core target into interpreter consumers. Previously some
interpreter targets did not inherit them. This closes a build-policy gap without
introducing hardware optimization.

```sh
ctest --test-dir build -R '^experiment' --output-on-failure
```

[Grid/LHS/Sobol design expansion](RUNTIME_SCENARIO_DESIGNS.md) is now implemented
with pinned independent references. [Arrow/Parquet outputs](RUNTIME_OUTPUTS.md)
now preserve typed observations with atomic publication. Data bindings, manifests/replay,
the C/Python interfaces and file-backed replay bundles now have bounded
implementations; general hybrid graph loading remains M6 work. M6 is not
complete merely because ordered execution passes.
