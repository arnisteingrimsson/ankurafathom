# Local run manifests and strict replay

CLI file runs write a JSON manifest by default and support strict local replay:

```sh
./build-arrow/fathom run models/data/workforce.ir.json \
  --out workforce.csv --threads 8 --seed 42
./build-arrow/fathom replay workforce.csv.manifest.json --threads 1
./build-arrow/fathom replay workforce.csv.manifest.json --out workforce-replayed.parquet
```

`fathom run ... --out PATH` writes `PATH.manifest.json`, appending to the full
output filename (for example, `results.parquet.manifest.json`). Relative paths are
resolved from the working directory, while the receipt records absolute paths.
The sidecar must be new: existing files, directories, hardlinks and symlinks,
including dangling symlinks, reject before replacing the result. Use a new result
path for another default-manifest run, or select a new explicit receipt path.

`--manifest PATH` selects a different sidecar and requires `--out`. The flag
`--no-manifest` disables both sidecar creation and result lineage, preserving the
legacy CSV/Arrow/Parquet schema. It cannot repeat or combine with `--manifest` in
either order. This explicit opt-out does not delete or update older sidecars; if
an older receipt refers to a replaced result, verify it before relying on it.

Standard-output runs remain legacy CSV and create no sidecar; an explicit
`--manifest` still requires a file output. CLI file manifests use version 0.2 and
add the result lineage described below. The default affects file `run` commands;
`lint`, `replay`, `verify-results`, and C/Python defaults retain their contracts.
[C/Python API receipts](API_PROVENANCE.md) use version 0.3 to represent in-memory
inputs/output explicitly; they support saved-artifact verification, but not replay.
`replay` accepts only `--out`, `--format` and `--threads`. Without `--out`, it
re-executes the model and prints a verification JSON with the manifest ID, numeric
identity and selected thread-count setting. With `--out`, it publishes only after numeric
identity matches and creates no new sidecar; retain the original replay receipt. The output format follows the output path unless overridden. Single-run execution
remains serial; the thread setting controls experiment workers.

Manifest-file replay uses the original model, experiment and bound data at the
recorded paths. [Portable bundles](PORTABLE_REPLAY.md) now capture those inputs
for relocation and replay after the originals are removed. The original result file is not needed for replay;
replay verifies newly computed values against the recorded numeric digest, not the
current contents of the old result file.

## Recorded provenance

The versioned [manifest schema](../ir/schema/ankurafathom-manifest.schema.json)
records:

- Model and optional experiment: absolute path, SHA-256 of the file bytes,
  canonical JSON text and its SHA-256. Hashing and parsing use the same captured
  bytes, with a 256 MiB limit per JSON input. Inputs must be complete JSON
  documents; trailing JSON or other non-whitespace rejects.
- Every data binding: its full IR declaration, resolved local path, file SHA-256
  and canonical-table SHA-256, taken from the snapshot actually loaded by the
  native data consumer. Binding declarations retain mappings, units, keys and
  interpolation policy. Initial population ID assignment remains the canonical
  key ordering specified by the population binding contract; the manifest does
  not duplicate every population receipt entry.
- The expanded scenario list, explicit overrides, effective scalar parameters,
  replication count and effective seed, including CLI seed overrides. Generated
  grid/LHS/Sobol designs retain the original experiment identity as well as their
  expanded values.
- Recorded thread count, CPU backend and precision policy, project version,
  source-content fingerprint, Git commit when available, compiler, build type and
  flags, sanitizer/Arrow configuration, standard-library version, OS/release,
  architecture, pointer/double size and byte order.
- Ordered numeric result SHA-256, row/trajectory counts, original result path and
  format, UTC manifest creation time, and model/execution success. Numerical
  accuracy is explicitly `not_assessed`; passing lint and execution is not a
  scientific validation verdict.

The source fingerprint hashes sorted paths and contents under the compiled
project headers/sources, runtime sources, vendored JSON headers and CMake files.
CMake watches those contents so incremental builds refresh the fingerprint. Git
commit is null when this checkout has no Git metadata; the content fingerprint
still exists. Build metadata does not fingerprint every dynamically loaded system
or Arrow library, CPU feature or OS setting. Numeric replay equality remains the
final test, and compatibility across builds/platforms is not claimed.

JSON canonicalization v1 uses the pinned native JSON library's compact `dump()`:
lexically sorted object keys, preserved array order, native finite-number spelling
and UTF-8 strings. It is not RFC 8785/JCS; integer and floating JSON representations
can have different identities. The manifest ID is SHA-256 of that canonical
manifest object with `id` omitted. The timestamp, original output path and thread
count are included, so this is an artifact content ID, not a unique semantic-run
or input-only ID. It is an integrity checksum, not a signature or attestation.

## Numeric identity

The digest encodes the actual ordered observations, independent of file format.
This preserves signed zero and does not depend on decimal formatting or Parquet
writer bytes. All integer lengths and addresses below are unsigned little-endian
64-bit words:

1. UTF-8 bytes `AnkuraFathom.observations.v1` followed by one NUL byte.
2. Trajectory count.
3. For each trajectory, scenario ID, replication ID and observation count.
4. For each observation, the IEEE-754 time bits, UTF-8 output-ID byte length and
   bytes, then the IEEE-754 value bits.

Trajectory addresses must increase strictly and fit in the existing 16-bit
runtime address range. Observation order is preserved; duplicate time/output
pairs, nonfinite values and invalid observations reject. The independent Python
hashlib contract checks 32 identities, 768 adversarial float values and an empty
result, including signed zero, subnormals, extreme finite values and Unicode IDs.

## Result lineage (observation schema 0.2)

Manifest-backed results use seven non-null columns in this order:
`scenario`, `replication`, `time`, `output_id`, `value`, `manifest_id`,
`scenario_parameters`. The first five retain the existing types. The two new
columns are UTF-8 strings: the manifest's SHA-256 content ID and canonical JSON
text of that scenario's **effective scalar parameters**, including defaults and
overrides. Parameters retain the native canonicalization described above, including
negative zero. CSV uses the same seven columns even for a single run, with its
scenario and replication both zero; standard CSV quoting protects JSON text.

Arrow IPC and Parquet schema metadata carry:

| Key | Value |
| --- | --- |
| `ankurafathom.schema_version` | `0.2` |
| `ankurafathom.table` | `observations` |
| `ankurafathom.manifest_id` | Original manifest ID |
| `ankurafathom.manifest` | Complete canonical manifest JSON |

The embedded manifest retains the scenario catalog, including scenarios whose
trajectories have no observations. An empty Arrow/Parquet table retains its schema
and metadata. CSV has no embedded full manifest: retain its sidecar to resolve the
ID and empty trajectories. A CSV with no observations contains only its header.
No extra scenario rows are inserted into the observation table.

The native output API accepts an optional `OutputProvenance` containing manifest
JSON. Before publication it checks version, checksum, ordered numeric identity,
complete scenario × replication addresses and finite scalar parameter maps.
Scenario overrides must agree with effective values, including the sign bit of
zero. Invalid associations reject before replacing an existing result. These
association checks do not independently establish that arbitrary native caller
metadata came from the claimed model; CLI replay additionally validates the source
inputs and re-executes them. Checksums are not authentication.

Lineage is excluded from the numeric digest, so output encoding and metadata do
not create a circular identity. Replay of a version 0.2 manifest retains its
original manifest ID and embedded manifest even when output path, format or worker
count changes. The recorded output and execution fields describe the **original
run**, not a new replay artifact. There is no separate replay-event manifest yet.
Version 0.1 manifests remain readable and replay using the original result schema;
the strict build-policy check still applies, so this is not cross-build replay.

## Replay and publication behavior

Replay checks the manifest checksum and structure, loads original inputs through
the normal validators, and compares captured input identities and build policy.
Both raw and canonical identities must match. Even a whitespace-only model edit
or a physical data-row reorder rejects under this strict policy. A thread-count
override is allowed; it does not rewrite the original recorded count. The original
seed and experiment are retained. Execution then recomputes the numeric digest;
a mismatch prevents result publication. `IR_MANIFEST` identifies malformed or
inconsistent manifests; `IR_REPLAY_MISMATCH` identifies input/policy or numeric
mismatch. Normal loader diagnostics still apply to missing/invalid source files.

Output and manifest paths cannot alias the model, experiment or bound data; replay
also protects its input manifest. Existing hardlink/symlink aliases and equivalent
paths to new destinations are checked. This is filesystem preflight, not protection
against another process changing paths after validation.

Manifest bytes are prepared and flushed in a sibling temporary file before result
publication. Results retain their existing atomic single-file publication. The
manifest is renamed into place last, as the completion marker. There is no atomic
transaction spanning both file renames: a late failure can leave a published
result without a committed manifest. A committed manifest destination must be new;
it is not silently replaced. No directory-fsync crash durability is promised.
Preparation/output-failure tests verify input/sentinel preservation and temporary
cleanup. Manifests can contain scalar parameters and model text, just as the source
files do.

## Verify a saved artifact

```sh
./build-arrow/fathom verify-results workforce.csv.manifest.json
./build-arrow/fathom verify-results workforce.csv.manifest.json --results workforce-replayed.parquet
./build-arrow/fathom verify-results workforce-replayed.parquet --embedded
```

`verify-results` checks a **version 0.2** result against an explicit manifest without
executing the model. With no `--results`, it uses the original path and format in
the manifest. With `--results`, the format follows that path's suffix; `--format
csv|parquet|arrow` can override either choice. It prints a JSON verdict with
`scope: artifact-integrity`, the manifest ID, verified numeric identity and the
checked path. It never publishes or modifies either input file.

With `--embedded`, the positional path is the result file itself. Arrow IPC and
Parquet can be checked without a sidecar; `.arrow`, `.ipc` and `.parquet` select
their formats, and `--format arrow|parquet` supports other suffixes. This mode
rejects `--results` and CSV. The verdict includes `manifest_source: embedded`;
ordinary sidecar verification reports `manifest_source: sidecar`.

The embedded mode extracts the manifest and verifies observations from the same
decoded table, so it does not reopen the file between extraction and verification.
It checks the same schema, lineage and numeric identity as the sidecar path, then
applies the same CLI manifest-envelope validation. Embedded JSON must be canonical,
nonempty and at most 256 MiB. Empty results still require a valid embedded manifest.
The native `verify_embedded_observations` API returns verified provenance after
the runtime association checks; no extraction file or temporary sidecar is written.

The verifier checks the manifest checksum, complete trajectory catalog, exact
observation schema, row addresses/order, canonical scenario-parameter text,
manifest IDs and ordered IEEE-754 numeric digest. Arrow/Parquet also require the
recorded non-null column types and all four provenance metadata entries. Required
metadata cannot have duplicate keys; unrelated metadata entries are allowed.
Embedded manifest text must equal the canonical reference manifest. CSV accepts LF
or CRLF records, quoted commas/quotes/newlines and a final record without a newline;
malformed quoting, wrong headers/widths and malformed numbers reject.

Empty trajectories are reconstructed from the manifest's scenario × replication
catalog before hashing. Empty files still require the observation header/schema;
empty Arrow/Parquet tables still require provenance metadata. Verification is
bounded to the experiment coordinator's one-million-trajectory limit. Decoded
rows and binary tables are currently materialized in memory.

Original model, experiment and data files need not exist, and the verifier's build
need not match the original execution build. This verdict checks **decoded artifact
integrity**, not source-file validity, re-execution, numerical accuracy or producer
authenticity. Independent re-encoding can pass despite different file bytes.
In embedded mode the reference comes from the artifact itself: changing both the
manifest and results consistently can pass. A separately retained sidecar checks
against that independent reference. Neither mode authenticates the producer.
Use `replay` to re-execute and check the original inputs/build policy. Version 0.1
artifact verification and embedded-manifest replay remain later work. Portable
file-backed input bundles are implemented separately. Artifact relocation and missing original inputs/sidecars are supported
by embedded verification.

`IR_RESULT_VERIFY` reports unreadable, malformed or mismatched result files;
`IR_MANIFEST` reports invalid or unsupported manifests; `IR_RESULT_UNAVAILABLE`
reports binary verification requested in a CSV-only build. Failed checks emit no
success verdict. Malformed embedded metadata/JSON reports `IR_RESULT_VERIFY`;
manifest-envelope failures report `IR_MANIFEST`. Invalid mode/format combinations
report `IR_USAGE`. External mutation of files during decoding is not covered.

## Scope and remaining work

The contract covers single runs, explicit experiments, a generated-design case,
stochastic and deterministic models, all three bound-data kinds, CSV/Parquet/IPC
numeric equality and replay at 1/8/32 threads. Independent numeric hashes and
input/manifest drift controls are part of the test suite. See the session log for
actual test counts and build coverage.

This does not complete §6.14 or M6. Legacy artifact verification,
embedded-manifest replay, richer validation timestamps/verdicts,
event traces and explainability remain open. C/Python manifest access is implemented
with the separately documented version-0.3 receipt. Cross-platform/build tolerance
replay is also separate from this strict local path.
