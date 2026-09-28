#pragma once

#include "ankurafathom/des/runtime_entity_store.hpp"
#include <optional>
#include <variant>

namespace ankurafathom::des {

struct ResourceLease {
    std::uint32_t pool;
    std::uint64_t request_id;
    std::size_t units;
    bool operator==(const ResourceLease&)const = default;
};

template<class Tag>
struct EntitySnapshot {
    double time;
    std::vector<EntityField> schema;
    typename RuntimeEntityStore<Tag>::Record record;
    bool operator==(const EntitySnapshot&)const = default;
};

template<class Tag>
struct EntityToken {
    EntityRef<Tag> entity;
    std::int32_t priority=0;
    std::vector<ResourceLease> leases={};
    std::optional<EntitySnapshot<Tag>> snapshot={};
    bool operator==(const EntityToken&)const = default;
};

struct QueuePull {
    std::size_t count;
    bool operator==(const QueuePull&)const = default;
};

template<class Tag>
using ProcessMessage=std::variant<EntityToken<Tag>,QueuePull>;

} // namespace ankurafathom::des
