# Declarative population initialization

Typed ABM models (`mode: "abm"`) can initialize their single sync or async
population from a local CSV, Parquet or Arrow IPC table. This extends the native
[transactional initializer](POPULATION_DATA_BINDING.md) through the model loader,
CLI and JSON schema. Data readers require an Arrow-enabled build, including for
CSV sources. Inline-agent models continue to work without Arrow.

The runnable example is
[`models/data/workforce.ir.json`](../models/data/workforce.ir.json):

```sh
./build-arrow/fathom lint models/data/workforce.ir.json
./build-arrow/fathom run models/data/workforce.ir.json
```

The population keeps its `fields` and behavior declarations, omits `agents`, and
uses one top-level `data` entry:

```json
{
  "id": "workforce",
  "source": "synthetic_workforce.csv",
  "schema": {
    "key_column": "key",
    "columns": [
      {"name": "key", "type": "u64", "unit": ""},
      {"name": "work", "type": "i64", "unit": "1"},
      {"name": "ready", "type": "bool", "unit": "1"},
      {"name": "role", "type": "string", "unit": "1"}
    ]
  },
  "use": {
    "kind": "population_init",
    "population": "people",
    "fields": [
      {"field": "work", "column": "work"},
      {"field": "ready", "column": "ready"},
      {"field": "role", "column": "role"}
    ]
  }
}
```

The source path resolves relative to the model file. Absolute local paths are
accepted; URIs and NUL-containing paths reject. Exact schemas, unique keys,
finite floats, no nulls, optional string categories and canonical key order follow
the [native table contract](RUNTIME_DATA.md). The source key is `schema.key_column`;
there is no second `id_column` setting.

Every population field must have exactly one mapping. Source columns may be reused
by different fields, and unmapped source columns remain subject to validation.
Declared source dimensions must equal target field dimensions; equivalent spellings
are accepted without scale conversion. Mapped boolean/string fields use dimensionless
units such as `1`; empty units are suitable only for unmapped labels/keys.

| Population field | Accepted source types |
| --- | --- |
| real | f64 |
| integer | i32, i64, checked u64 |
| boolean | bool |
| string | string |

The IR expression engine represents numeric fields as binary64. Bound integer
fields therefore retain the inline-record limit of ±9,007,199,254,740,991, including
fields not currently read by outputs. Wider values reject instead of rounding.
External keys are separate: a uint64 source key can use its complete range.

Canonical key order assigns engine IDs `0..N-1` in store 0. Physical source row
order and field-mapping order do not affect initialization. Keys are never cast
into entity IDs. The in-memory `Model.population_data` receipt records the binding
ID, population, resolved path, mappings, file/canonical hashes and each typed
source key with its assigned agent reference. It describes the initial population;
subsequent births and retirements retain the existing lifecycle semantics.

IDs are stable for the same key set and canonical ordering. Adding or removing a
key can change assigned IDs; this is not a persistent external-identity registry.
Network edges, messages, publications, outputs and lifecycle references use engine
IDs, as they do with inline agents. Consult the receipt to relate those IDs back
to external keys.

The loader stages a fresh native store and complete receipt, then feeds the
resulting records through the existing ABM validation. Agent limits, spatial
bounds, network endpoints/generator constraints, statechart generation and
lifecycle references are still checked. Async chart initialization retains its
existing reset/start behavior. Empty tables are allowed only when the rest of the
model is valid for an empty population; references to nonexistent agents still
reject. `agent_limit` includes the initial agents and later allocations.

A model supplies inline `agents` or a binding, never both, including an empty
inline list. No implicit append, merge or fallback occurs. Copying/moving the loaded
model retains owned records and receipts. Reloading creates a new snapshot;
running a model never reopens its data source. Source files must remain stable
while loading, and callers must not mutate a shared Model during concurrent runs.
Source aliases are protected from CLI output during preflight, including existing
symlink/hardlink aliases.

Data failures retain `DATA_*` codes with a binding prefix. Mapping errors locate
`/data/0/use/fields`. Conversion-range errors locate canonical sorted source rows;
reader cell errors locate physical source rows, matching the existing native
contracts. Invalid initial chart generation identifies `/data/0/source`. Other
ABM invariant errors identify the relevant component declaration.

This first declarative population path supports one required binding to the one
typed population in `mode: "abm"`. Parameter-table/series bindings remain standalone
SD features; they are not silently accepted in ABM. Ordinary ABM parameters and
scenario overrides still work. General hybrid/multiple-population loading,
optional fallbacks, remote sources, serialized manifests and replay remain open.

Verification compares table-backed and inline versions of twelve existing ABM
fixtures over CSV/Parquet/IPC and both physical row orders, including sync/async
behavior, lifecycle, messaging, spatial queries and networks. These share the
same runtime and validate binding equivalence, not an independent ABM engine.
The native suite additionally checks a hand-computed phase recurrence, full-width
keys, receipts, copy/move ownership, reload isolation and 192 trajectories at
1/8/32 threads. Rejection controls cover schema/mapping/type/unit conflicts,
integer boundaries, duplicate keys, inline/binding ambiguity, initial chart state,
spatial bounds and source aliases. See the session log for recorded results.
