# Transactional population initialization from data

`runtime::data::initialize_population` connects a [validated data snapshot](RUNTIME_DATA.md)
to a native typed `abm::PopulationStore<Tag>`. It initializes records, allocates
simulation IDs, reconciles network membership and returns an identity receipt as
one transaction. This is the first native data-consumer adapter; declarative model
IR bindings remain open.

The target must be a never-populated store: zero allocated records and its original
next ID. A store whose agents have all retired is not fresh and is rejected. Empty
input tables validate their mappings and return an empty receipt without consuming
IDs; they do not mark a store as initialized, so repeating an empty load is allowed.

## Field and identity rules

Every target field must have exactly one explicit `PopulationField` mapping:
`{target_field, source_column, expected_unit}`. Mapping order and target schema order
do not change values assigned by name. Unknown/duplicate targets, missing mappings,
unknown source columns and incompatible types fail even for an empty table.
Unused source columns are allowed; a source column can explicitly feed multiple
compatible target fields. The adapter does not infer defaults or field names.

| Source | Target | Conversion |
| --- | --- | --- |
| `f64` | real | Exact finite float bits |
| `i32` | integer | Exact widening to signed 64-bit |
| `i64` | integer | Exact signed 64-bit |
| `u64` | integer | Checked value ≤ `INT64_MAX` |
| boolean | boolean | Exact |
| UTF-8 string | string | Owned byte-preserving copy |

Other conversions fail. In particular, integers are not silently converted to
float and floating values are not truncated to integer. The expected unit must
exactly equal the source column's declared unit, including empty declarations.
Native population schemas lack unit metadata, so the caller supplies the expected
target unit. This is not unit conversion or a substitute for the IR unit checker.

Rows already arrive in canonical source-key order. The population allocates new
48-bit IDs beginning at its configured `first_id`. Source keys are **not** used as
simulation IDs; full-width uint64 keys and strings are retained unchanged in the
receipt. The receipt owns `{source_key, agent_reference}` entries plus the source's
file and canonical hashes. Existing empty network configuration is preserved and
new agents become vertices with no inferred edges.

All mappings, conversions, records, source-key copies and receipt allocations are
prepared before commit. Births and topology changes occur on a candidate store.
The final store move and receipt return are statically required to be nonthrowing.
A late conversion error, allocation failure or exhausted ID range leaves the
original store and identity counter unchanged. No receipt is returned on failure.

## Native example

```sh
cmake --build build-arrow --target fathom_population_from_data --parallel 4
./build-arrow/fathom_population_from_data models/data/synthetic_parameters.csv
```

The synthetic example loads practice rows, maps capacity and rate into typed agents,
then runs one synchronous ABM phase that adds one to each rate. Expected output:

```csv
agent_id,practice,people,rate_after_one_synthetic_step
10,disputes,36,411.25
11,forensics,24,326.5
```

Errors use the data layer's structured exception: `DATA_POPULATION` for a nonfresh
target or exhausted IDs, `DATA_MAPPING` for incomplete/incompatible fields,
`DATA_UNIT` for unit mismatch and `DATA_RANGE` for an unsigned value that cannot fit
the population's signed integer. Mapping pointers use `/fields/<index>/...`.
Range-error pointers use `/rows/<canonical-row>/<source-column>`; these rows are
already sorted, unlike the loader's physical-source-row diagnostics.

## Correctness evidence and limits

Native checks cover all 120 mapping permutations with alternating target field
orders, source-key preservation through `UINT64_MAX`, final assignable 48-bit IDs,
network membership, receipt hashes, signed zero/subnormal preservation, empty
input, repeat initialization rejection, retired-ID preservation, mapping/unit
failures and a range error after two valid predecessors. Failed initialization can
be retried with valid data without consuming IDs.

A Python source-row oracle checks 36 CSV/Parquet/IPC configurations, **9,252 agents**
and **55,512 field values**, with shuffled rows/columns/mappings/target schemas,
dictionary encoding, multiple batches, full-width source keys and high simulation
IDs. Nine invalid bindings reject. The native runtime also initializes 192
trajectories from a shared immutable snapshot and runs an ABM phase at **1/8/32
threads**, checking against hand-derived results and identical numeric bits.

```sh
cmake --build build-arrow --target population_init_tests runtime_data_probe abm_typed_population_tests --parallel 4
ctest --test-dir build-arrow -R '^(runtime_(population_init|data_contract)|abm_typed_population)' --output-on-failure
```

The adapter requires no Arrow types in its header; file loading still requires an
Arrow-enabled data library. The CSV-only build checks compilation and reader
availability, not successful population loading. ASan/UBSan instrument project
code; the prebuilt Arrow SDK remains uninstrumented. Remote CI is not claimed run.

Dynamic append/import, initialization of an already-running population, network
edge binding and declarative JSON `population_init` remain open. Native
[series and parameter-table adapters](DATA_INPUT_BINDINGS.md) are now implemented.
This receipt establishes identity lineage but is not
a complete run manifest. M6 remains in progress.
