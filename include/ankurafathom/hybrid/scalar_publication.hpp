#pragma once
#include <cstdint>
#include <vector>

namespace ankurafathom::hybrid {
struct ScalarPublication {
    double time;
    std::uint64_t revision;
    std::vector<double> values;
    bool operator==(const ScalarPublication&)const=default;
};
} // namespace ankurafathom::hybrid
