#pragma once
#include "ankurafathom/ir/model.hpp"
#include <nlohmann/json.hpp>

namespace ankurafathom::ir::validation {
void parse_checks(const nlohmann::json&, Model&);
nlohmann::json check(const Model&, const runtime::Experiment&, std::size_t threads);
int cli(int argc, char** argv);
}
