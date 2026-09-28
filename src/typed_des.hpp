#pragma once
#include "ankurafathom/ir/model.hpp"
#include <nlohmann/json.hpp>
#include <set>

namespace ankurafathom::ir {
void parse_typed_des(const nlohmann::json&,Model&,std::set<std::string>&);
std::vector<Row> run_typed_des(const Model&,const std::map<std::string,double>&,
    std::uint64_t seed,std::uint32_t scenario,std::uint32_t replication);
}
