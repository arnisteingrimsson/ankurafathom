#pragma once

#include "ankurafathom/sd/delay.hpp"
#include <optional>

namespace ankurafathom::sd {
// SMOOTH/SMOOTH3 initialize every stage to initial_input; an explicit initial
// value selects SMOOTHI/SMOOTH3I. State is owned here, never in a pure expression.
class EulerSmooth {
public:
    EulerSmooth(std::size_t order, double initial_input, double duration,
                std::optional<double> initial_value = std::nullopt)
        : stages_(DelayKind::information, order, duration, initial_value.value_or(initial_input)), duration_(duration) {
        if (!std::isfinite(initial_input)) throw std::invalid_argument("invalid initial smoothing input");
    }
    double output() const { return stages_.output(duration_); }
    const std::vector<double>& stages() const noexcept { return stages_.stages(); }
    void step(double input, double duration, double dt) {
        stages_.step(input, duration, dt);
        duration_ = duration;
    }
private:
    EulerDelay stages_;
    double duration_;
};
} // namespace ankurafathom::sd
