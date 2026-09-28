#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ankurafathom::sd {

enum class Extrapolation { clamp, linear };

class LookupTable {
public:
    LookupTable(std::vector<double> x, std::vector<double> y, Extrapolation policy)
        : x_(std::move(x)), y_(std::move(y)), policy_(policy) {
        if (x_.size() < 2 || x_.size() != y_.size())
            throw std::invalid_argument("lookup table needs at least two matching x/y knots");
        for (std::size_t i = 0; i < x_.size(); ++i) {
            if (!std::isfinite(x_[i]) || !std::isfinite(y_[i]) ||
                (i > 0 && !(x_[i] > x_[i - 1])))
                throw std::invalid_argument("lookup knots must be finite with strictly increasing x");
        }
    }

    double evaluate(double x) const {
        if (!std::isfinite(x)) throw std::invalid_argument("lookup query must be finite");
        if (x <= x_.front())
            return x == x_.front() || policy_ == Extrapolation::clamp
                ? y_.front() : interpolate(0, x);
        if (x >= x_.back())
            return x == x_.back() || policy_ == Extrapolation::clamp
                ? y_.back() : interpolate(x_.size() - 2, x);
        const auto upper = std::upper_bound(x_.begin(), x_.end(), x);
        const std::size_t left = static_cast<std::size_t>(upper - x_.begin() - 1);
        if (x == x_[left]) return y_[left];
        return interpolate(left, x);
    }

    std::size_t size() const noexcept { return x_.size(); }
    Extrapolation extrapolation() const noexcept { return policy_; }

private:
    double interpolate(std::size_t left, double x) const {
        const double fraction = (x - x_[left]) / (x_[left + 1] - x_[left]);
        const double value = std::lerp(y_[left], y_[left + 1], fraction);
        if (!std::isfinite(value)) throw std::overflow_error("lookup interpolation overflow");
        return value;
    }

    std::vector<double> x_;
    std::vector<double> y_;
    Extrapolation policy_;
};

} // namespace ankurafathom::sd
