#pragma once
#include "ankurafathom/ir/model.hpp"
#include <nlohmann/json.hpp>
#include <set>
namespace ankurafathom::ir {
void parse_agent_stock_sd(const nlohmann::json&,Model&,std::set<std::string>&);
std::vector<Row> run_agent_stock_sd(const Model&,const std::map<std::string,double>&);
}
