#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ankurafathom::hybrid {
struct StockPulse {
    double time;
    std::string channel;
    std::uint64_t revision;
    std::vector<double> amounts;
    bool operator==(const StockPulse&) const = default;
};
} // namespace ankurafathom::hybrid
