#pragma once
#include "ankurafathom/ir/model.hpp"
#include <nlohmann/json.hpp>

namespace ankurafathom::ir {
std::optional<std::size_t> resolve_model_data(const nlohmann::json& bindings,
                            const std::filesystem::path& base, Model& model);
PopulationData resolve_population_data(const nlohmann::json& binding, std::size_t index,
    const std::filesystem::path& base, const std::string& population, TypedAbm& spec);
}
