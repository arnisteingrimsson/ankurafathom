#pragma once
#include "ankurafathom/ir/model.hpp"
#include <nlohmann/json.hpp>
#include <set>
namespace ankurafathom::ir {
void parse_typed_abm(const nlohmann::json&,Model&,std::set<std::string>&,
                     const std::filesystem::path* base=nullptr,
                     std::optional<std::size_t> population_binding=std::nullopt);
std::vector<Row> run_typed_abm(const Model&,const std::map<std::string,double>&,std::uint64_t,std::uint32_t,std::uint32_t);
}
