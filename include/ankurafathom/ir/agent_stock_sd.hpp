#pragma once
#include "ankurafathom/abm/population_store.hpp"
#include "ankurafathom/ir/expression.hpp"
#include <optional>

namespace ankurafathom::ir {
struct ContinuousAgentTag {};
struct AgentStockSd {
    using Store=abm::PopulationStore<ContinuousAgentTag>;
    std::string population_id;
    double population_dt=0;
    std::vector<des::EntityField> fields;
    std::map<std::string,Dimension> field_units;
    std::vector<Store::Record> agents;
    struct FieldStock {
        std::size_t field;
        bool non_negative;
        std::optional<Expression> inflow,outflow;
        std::string pointer;
    };
    struct Aggregate {
        std::string id,op,pointer;
        std::optional<Expression> expression,filter;
        std::optional<double> empty;
        Dimension unit;
    };
    struct Output { std::string id,pointer; Expression expression; };
    std::vector<FieldStock> agent_stocks;
    std::vector<Aggregate> aggregates;
    std::vector<Output> outputs;
};
} // namespace ankurafathom::ir
