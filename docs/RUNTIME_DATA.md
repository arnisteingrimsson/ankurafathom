# Validated data snapshots and content identity

The M6 data foundation loads local CSV, Parquet and Arrow IPC **files** into a
value-owned, read-only table. Every column is declared, the key is explicit, and
loading either returns a fully validated snapshot or throws a structured error.
The API is `include/ankurafathom/runtime/data.hpp`; link the `fathom_data` target.
It uses the same optional Arrow SDK as [result outputs](RUNTIME_OUTPUTS.md).

This is the native loading contract. [Population initialization](POPULATION_DATA_BINDING.md)
now consumes these snapshots transactionally. General model IR `data` declarations
remain open. [Native series/parameter adapters](DATA_INPUT_BINDINGS.md) now consume
these snapshots as well.
The test probe's JSON requests are test infrastructure, not a published model IR.

## Native use

```cpp
namespace data = ankurafathom::runtime::data;
data::Schema schema{{
    {"practice", data::Type::string, "", {}},
    {"capacity", data::Type::i32, "people", {}},
    {"rate", data::Type::f64, "USD/hour", {}}
}, "practice"};
const auto table = data::load_file("parameters.parquet", schema);
const auto row = table.find_row(std::string("disputes"));
if (row) {
    const double rate = std::get<double>(table.rows()[*row][table.column_index("rate")]);
    // Use rate in a model's parameter initialization.
}
```

The included values are synthetic examples, not calibrated Ankura data:

```sh
cmake --build build-arrow --target fathom_data_table --parallel 4
./build-arrow/fathom_data_table models/data/synthetic_parameters.csv
```

The example prints both hashes and the rows sorted by practice. A native lookup
uses the exact declared key type; missing keys return `std::nullopt`, wrong types
and non-finite numeric keys fail. Unknown column names fail. The loaded snapshot
owns its strings and scalar values and remains valid after the source disappears.

## Schema and row contract

The binding schema contains 1–1024 uniquely named ASCII identifier columns and
one declared key column. All columns are required and all values non-null.

| Native type | Required decoded Arrow type | Canonical code |
| --- | --- | --- |
| `boolean` | `bool` | 1 |
| `i32` | `int32` | 2 |
| `i64` | `int64` | 3 |
| `u64` | `uint64` | 4 |
| `f64` | `float64` | 5 |
| `string` | `utf8` | 6 |

No implicit integer widening, integer-to-float conversion, missing-column fill,
extra-column dropping or null substitution occurs. Dictionary encoding is accepted
when its decoded value type matches. Float values must be finite. Strings are valid
UTF-8; their bytes, including embedded NUL, are preserved. An optional string
`categories` set restricts the domain; duplicate members or categories on a
non-string column are errors. Category order has no semantic significance.

Column order is normalized by name. Rows sort ascending on the key, numerically
for numeric keys and lexicographically by unsigned UTF-8 bytes for string keys.
Duplicate keys fail; floating key `-0.0` and `+0.0` count as the same key. Key
strings may be empty if unique. No Unicode normalization is performed. Input row
order never assigns an implicit identity. Nullable Arrow field metadata is allowed
when the actual column contains no nulls; file metadata is not binding metadata.

Units are declared strings carried into the canonical identity. The reader does
not convert units or prove dimensional compatibility with a model; that belongs
to the binding adapter and the existing IR unit checker.

## Formats, limits and errors

Suffix inference recognizes `.csv`, `.parquet`, `.arrow` and `.ipc` only; the last
two mean Arrow IPC file format. An explicit `ReadOptions::format` can override a
suffix. Network URIs, directories and non-regular files are unsupported. Arrow
decodes a single in-memory byte snapshot; file hashing and decoding use the same
bytes, without a second read of the path. The producer must keep the source stable
while it is being read; concurrent writes are not claimed to be an atomic snapshot.

CSV uses the declared types rather than inference for expected columns. It accepts
quoted multiline fields and decimal point `.`. Booleans are exactly `true` and
`false`. Empty numeric cells are nulls and fail; empty strings and the text `null`,
`NA` or `nan` remain strings. Blank lines are not silently discarded. Numeric
overflow or malformed records fail. CSV headers must match the schema exactly.

Default limits are 256 MiB of input bytes and 1,000,000 decoded rows. Limits are
positive and caller-configurable. The file cap is enforced while reading; the row
cap is checked after Arrow decoding. This is not a bound on decompression or total
memory usage: files, decoded tables and owned values can coexist during loading.
Streaming and memory-budget enforcement remain future work.

`data::Error` carries `code`, `pointer` and `what()`:

| Code | Meaning |
| --- | --- |
| `DATA_SCHEMA` | Invalid declaration, missing/extra/duplicate column or wrong type |
| `DATA_KEY` | Undeclared key, duplicate key or invalid lookup type/value |
| `DATA_NULL` | Null cell |
| `DATA_VALUE` | Non-finite number or value outside the category domain |
| `DATA_FORMAT` | Unknown suffix or unsupported explicit format |
| `DATA_IO` | Missing/non-regular/local-only source or read failure |
| `DATA_READ` | Arrow decoding or table validation failure |
| `DATA_LIMIT` | Invalid limits or file/row limit exceeded |
| `DATA_UNAVAILABLE` | Reader requested in a build without Arrow |

Cell pointers use `/rows/<physical-source-row>/<column>` before row sorting;
schema pointers use `/schema/<column>`. No partial table is returned on failure.
CSV-only simulation builds still work, but these data readers all require Arrow,
including the CSV reader. They report `DATA_UNAVAILABLE` without attempting input I/O.

## Hash encoding, version 1

`file_hash()` is SHA-256 of the exact read bytes. `canonical_hash()` is SHA-256
over the following unambiguous byte sequence, constructed independently of Arrow
serialization. All integer framing is **little endian**; strings are UTF-8 bytes
prefixed by a uint64 byte length.

1. ASCII `AnkuraFathom.table.v1` followed by one zero byte.
2. uint64 column count, uint64 row count, length-prefixed key-column name.
3. For each column in ascending name order: length-prefixed name, one-byte type
   code from the table above, length-prefixed unit string, uint64 category count,
   then length-prefixed categories sorted by unsigned UTF-8 bytes.
4. Rows in canonical key order, with cells in canonical column order. Boolean is
   one byte (0/1); i32 is four bytes in two's complement; i64/u64 are eight bytes;
   f64 is its eight IEEE-754 bytes; strings use the same uint64 length prefix.

Finite float bits are preserved, so changing a non-key cell from negative zero to
positive zero changes the canonical identity. File compression, metadata,
dictionary encoding, chunk sizes, row order, column order and category declaration
order do not affect it. Changed values, units, domains, names, key selection or
types do. This digest identifies a validated data snapshot plus its declared
contract; it is not yet a complete run manifest.

## Evidence and remaining scope

The portable SHA-256 implementation follows [FIPS 180-4](https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf).
Four known-answer vectors include the empty message, `abc`, a multiblock message
and one million `a` bytes. Another 141 binary patterns cover padding boundaries
and inputs up to 4 MiB, each with seven update layouts; Python `hashlib` independently
checks every digest. Snapshot/copy tests verify that requesting a digest does not
change subsequent updates.

The data contract tests generate **55 valid tables**, compare **62,037 typed cells**
and independently construct canonical hashes with Python `struct` and `hashlib`.
They exercise all six key types, extreme signed/unsigned integers, subnormals,
signed zero, UTF-8 and NULs, empty tables, dictionary arrays, multiple IPC batches
and Parquet row groups, source removal, unit/domain sensitivity and 10,003-row
shuffled tables. **37 invalid cases** cover schema, keys, nulls, limits, encoding,
formats and I/O. Both normal and sanitizer builds are tested; the prebuilt Arrow
SDK itself is not sanitizer-instrumented.

```sh
cmake --build build-arrow --target runtime_data_probe sha256_tests --parallel 4
ctest --test-dir build-arrow -R '^runtime_(data|sha256)' --output-on-failure
```

Native population initialization, exogenous series sampling and keyed parameter
mapping are implemented. Their declarative IR contracts remain open, along with
URI sources, nullable schemas, optional binding fallbacks, replayed
entities, calibration targets, run manifests/replay and C/Python interfaces remain
open. The native loader does not mark the full M6 data layer complete.
