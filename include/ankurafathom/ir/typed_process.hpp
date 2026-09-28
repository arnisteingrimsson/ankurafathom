#pragma once

#include "ankurafathom/des/runtime_entity_store.hpp"
#include "ankurafathom/des/queue_discipline.hpp"
#include "ankurafathom/ir/expression.hpp"
#include <optional>

namespace ankurafathom::ir {

struct ProcessEntityTag {};
using ProcessEntityStore=des::RuntimeEntityStore<ProcessEntityTag>;

struct TypedEntityType {
    std::vector<des::EntityField> fields;
    std::map<std::string,Dimension> units;
    std::uint32_t store=0;
};
struct TypedCondition {
    Expression left;
    std::string op;
    Expression right;
};
struct TypedProcessNode {
    std::string kind,entity_type,pointer;
    struct Entry { double time; ProcessEntityStore::Record values; };
    std::vector<Entry> schedule;
    struct Generator {
        std::size_t count;
        double start;
        ProcessEntityStore::Record values;
        Expression interval_or_rate;
    };
    std::optional<Generator> generator;
    std::optional<Expression> expression;
    std::optional<Expression> priority;
    std::optional<std::size_t> capacity;
    des::QueueDiscipline discipline=des::QueueDiscipline::fifo;
    std::string pool;
    std::size_t max_request_units=0;
    std::uint32_t pool_namespace=0;
    std::uint16_t block=0;
    std::vector<std::pair<double,Expression>> capacity_schedule;
    std::vector<std::string> ports;
    std::vector<TypedCondition> conditions;
    std::vector<double> probabilities;
    std::optional<std::uint32_t> stream;
};
struct TypedProcess {
    std::map<std::string,TypedEntityType> types;
    std::map<std::string,TypedProcessNode> nodes;
    struct Link { std::string from,to,port; std::uint32_t output_port; };
    std::vector<Link> links;
    std::map<std::string,std::uint32_t> output_ports;
};

} // namespace ankurafathom::ir
