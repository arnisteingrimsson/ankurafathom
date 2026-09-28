# C and Python run provenance

C results now retain a manifest captured once per completed run. Python can opt
into a table carrying that manifest and per-row scenario lineage:

```python
import json
from pathlib import Path
import pyarrow.parquet as pq
from ankurafathom import Model, run

path = Path("models/decay.ir.json").resolve()
model = Model.from_json(path.read_bytes(), base_directory=path.parent)
table = run(model, seed=42, provenance=True)
manifest_bytes = table.schema.metadata[b"ankurafathom.manifest"]
manifest = json.loads(manifest_bytes)
print(manifest["result"]["sha256"])
pq.write_table(table, "results.parquet")
Path("run.json").write_bytes(manifest_bytes)
```

```sh
fathom verify-results results.parquet --embedded
fathom verify-results run.json --results results.parquet
```

Both commands check artifact integrity without reopening model, experiment or data
inputs. Arrow IPC works the same way. Saving a transformed/subset table retains
metadata in many Arrow operations, but its rows no longer match the original run;
verification then rejects it. This is an integrity check, not proof of authenticity
or scientific accuracy.

## C interface

`fathom_results_manifest(results, &json, &size)` returns borrowed, NUL-terminated
JSON with a byte length excluding the terminator. It remains valid until
`fathom_results_free`; getters and repeated exports preserve it. Model and caller
input buffers may already have been freed. Invalid calls leave outputs unchanged.
The getter works in CSV-only builds too.

`fathom_results_arrow_with_manifest(results, &stream)` uses the same ownership and
release contract as `fathom_results_arrow`. It exports observation schema **0.2**:
existing five observation columns plus non-null UTF-8 `manifest_id` and
`scenario_parameters`. Its schema metadata contains the exact receipt returned by
the getter. Returned arrays, schemas and streams own their resources independently
of the result handle. CSV-only builds return `FATHOM_UNAVAILABLE` for either Arrow
export. These are additive ABI-1 entry points, bringing the public C symbol count
to thirteen.

The default C Arrow export and Python `run()` still return schema **0.1**. Python's
`provenance` argument must be a bool; `True` selects schema 0.2. Run returns a
`pyarrow.Table` in either case. Native execution captures receipt data even if a
caller chooses the unmanifested export; no receipt or result file is written by
the API itself.

## Manifest version 0.3

Existing CLI runs continue emitting file-backed manifest **0.2**, and versions
0.1/0.2 keep their existing validation/replay rules. API results use **0.3** with
the same envelope, input/build receipts and numeric encoding, but explicit memory
semantics:

- `output` is exactly `{"path":"","format":"memory","schema_version":"0.2"}`.
  This describes the available lineage representation, not a saved artifact.
- Model/experiment `path` is empty for captured JSON byte spans. C file loading
  retains its absolute model path. Nonempty paths must still be absolute.
- `file_sha256` retains its existing name and holds the hash of the original bytes,
  including for memory inputs. `canonical_json` and `canonical_sha256` identify
  their parsed representation. The validator checks canonical JSON consistency;
  raw bytes are not embedded, so their hash cannot be reconstructed from it.
- Bound data receipts retain captured binding declarations, absolute source paths,
  file hashes and canonical table hashes. Source files are not reread at run/export.
- Expanded scenarios, effective parameters, replications, effective seed, requested
  worker count and the existing build/precision policy match the CLI. Single runs
  remain serial even when a larger worker count was requested.
- The ordered numeric result digest is unchanged. The manifest ID also covers
  creation time and execution metadata, so equivalent numeric runs need not have
  identical manifest IDs. Repeated access to one result returns the same receipt.

Schema: [ankurafathom-manifest.schema.json](../ir/schema/ankurafathom-manifest.schema.json).
Version 0.3 requires memory output; weakening version 0.2 to allow empty file paths
is deliberately avoided. Preserve emitted manifest bytes when saving or hashing:
the checksum uses nlohmann's canonical JSON spelling, and another serializer can
spell a binary64 value differently while representing the same number.

**Version 0.3 replay is not implemented.** `fathom replay` rejects it with
`IR_REPLAY_UNSUPPORTED` before loading inputs or writing outputs. Explicit
`verify-results` needs `--results`, since the receipt contains no saved-artifact
path. Embedded verification uses the artifact supplied by the caller. This does
not introduce portable bundles or claim file-backed replay for memory inputs.

## Acceptance

`runtime_api_provenance_contract` compares C file/memory and Python receipts to
CLI input/execution/result identities, checks SHA-256 independently, validates the
JSON schema when jsonschema is available, and verifies saved Parquet/IPC outputs
through both sidecar and embedded paths. It covers default/zero/maximal seeds,
explicit/grid/LHS/Sobol designs, worker counts, three data-binding types, snapshots
after input removal, resealed invalid envelopes and tampered numeric output.
Existing native C tests cover borrowed JSON lifetime, unchanged failure outputs,
stream lifetime and unavailable Arrow export. See the session log for exact counts.
