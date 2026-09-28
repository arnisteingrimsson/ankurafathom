# Typed entity storage

The C++ `EntityStore<Tag, Fields...>` is the first entity-reference foundation for M3. It stores real, signed integer, boolean, and text fields in separate typed columns. `EntityRef<Tag>` carries an explicit store namespace and a monotonic 48-bit identity. Retired rows remain in the columns for audit and cannot be accessed as live entities or reused.

Spawn/update batches commit atomically. They reject nonfinite real fields, foreign/unknown/retired references, duplicate updates, and identity exhaustion before exposing changes. Read-only column views include tombstones and may be invalidated by mutation; callers keep references by ID. Copies own independent column data. See the full [contract](SEMANTICS.md#typed-entity-column-store).

`des_entity_store` verifies typed column alignment, boolean access, retirement, namespace checks, independent copies, failed-batch rollback, maximal IDs and exhaustion, empty batches, integer extremes, and growth across 128 records. A checked DEVS test owns the store inside a cloneable atomic: spawning during confluence is rolled back after a downstream failure, then retry publishes exactly the same two entity references without skipping an ID.

```cpp
struct Engagement {};
using Store = ankurafathom::des::EntityStore<
    Engagement, double, std::int64_t, bool, std::string>;
Store engagements(7); // Explicit namespace within this model.
auto ref = engagements.spawn({4.5, 2, false, "drafting"});
engagements.update(ref, {3.0, 2, true, "review"});
engagements.retire(ref);
```

This is a native library with compile-time field types. Declarative field names, units, source bindings, and process-reference ports are subsequent work. Store IDs must be unique within the owning model. Sharing mutable storage outside copied atomic state does not inherit checked-step rollback; ownership must be established explicitly when composing process blocks.
