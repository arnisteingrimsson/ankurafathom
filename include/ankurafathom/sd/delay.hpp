#pragma once

#include <cmath>
#include <array>
#include <cstddef>
#include <numeric>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ankurafathom::sd {

enum class DelayKind { material, information, fixed, history2 };

inline constexpr std::size_t max_delay_order = 255;

// Verified second-order history contract. The boundary output uses the previous
// duration; stage 0 uses the current duration. Both stages share old contents.
// Do not generalize this recurrence to other orders without independent evidence.
class HistoryDelay2 {
public:
    HistoryDelay2(double duration, double initial, double dt)
        : previous_duration_(duration), dt_(dt) {
        validate(duration);
        const double quantity=initial*(duration/2);
        if (!std::isfinite(initial) || !std::isfinite(quantity))
            throw std::invalid_argument("history delay initialization must be finite");
        stages_.fill(quantity);
    }
    const std::array<double,2>& stages() const noexcept { return stages_; }
    double previous_duration() const noexcept { return previous_duration_; }
    double output(double duration) const {
        validate(duration);
        const double value=stages_[1]/(previous_duration_/2);
        if (!std::isfinite(value)) throw std::overflow_error("history delay output overflow");
        return value;
    }
    double pipeline() const {
        const double total=stages_[0]+stages_[1];
        if (!std::isfinite(total)) throw std::overflow_error("history pipeline overflow");
        return total;
    }
    void step(double input,double duration,double dt) {
        validate(duration);
        if (!std::isfinite(input) || dt!=dt_)
            throw std::invalid_argument("history delay requires finite input and fixed dt");
        const double transfer=stages_[0]/(duration/2), outgoing=output(duration);
        const std::array<double,2> next{stages_[0]+dt*(input-transfer),
                                      stages_[1]+dt*(transfer-outgoing)};
        for (double value:next) if (!std::isfinite(value))
            throw std::overflow_error("history delay stage overflow");
        stages_=next;
        previous_duration_=duration;
    }
private:
    void validate(double duration) const {
        if (!std::isfinite(dt_) || dt_<=0 || !std::isfinite(duration) || duration<=0 || dt_>duration/2)
            throw std::invalid_argument("history delay duration must cover two fixed ticks");
    }
    std::array<double,2> stages_;
    double previous_duration_,dt_;
};

// A sampled transport delay: y[k] = initial for k < N, then input[k-N].
// Duration and dt are fixed at initialization; no rounding to a different lag.
class FixedDelay {
public:
    static std::size_t ticks(double duration, double dt) {
        if (!std::isfinite(duration) || !std::isfinite(dt) || duration <= 0 || dt <= 0)
            throw std::invalid_argument("fixed delay duration and dt must be finite and positive");
        const double count = duration / dt;
        const double nearest = std::round(count);
        if (!std::isfinite(count) || nearest < 1 || nearest > 1000000 ||
            std::abs(count - nearest) > 8 * std::numeric_limits<double>::epsilon() * nearest)
            throw std::invalid_argument("fixed delay requires 1..1000000 whole ticks");
        return static_cast<std::size_t>(nearest);
    }

    FixedDelay(double duration, double dt, double initial)
        : duration_(duration), dt_(dt), samples_(ticks(duration, dt), initial) {
        if (!std::isfinite(initial)) throw std::invalid_argument("invalid initial fixed delay output");
    }
    double output(double duration) const {
        if (duration != duration_) throw std::invalid_argument("fixed delay duration cannot change");
        return samples_[cursor_];
    }
    void step(double input, double duration, double dt) {
        if (!std::isfinite(input) || duration != duration_ || dt != dt_)
            throw std::invalid_argument("fixed delay requires finite input and unchanged duration/dt");
        samples_[cursor_] = input;
        cursor_ = (cursor_ + 1) % samples_.size();
    }
private:
    double duration_, dt_;
    std::vector<double> samples_;
    std::size_t cursor_ = 0;
};

class EulerDelay {
public:
    EulerDelay(DelayKind kind, std::size_t order, double initial_delay_time,
               double initial_output)
        : kind_(kind), stages_(validated_order(order), initial_output) {
        if (kind != DelayKind::material && kind != DelayKind::information)
            throw std::invalid_argument("Euler delay requires material or information kind");
        const double stage_duration = validated_stage_duration(initial_delay_time);
        if (!std::isfinite(initial_output) ||
            (kind_ == DelayKind::material && initial_output < 0))
            throw std::invalid_argument("invalid initial delay output");
        if (kind_ == DelayKind::material) {
            const double initial_quantity = initial_output * stage_duration;
            if (!std::isfinite(initial_quantity))
                throw std::overflow_error("initial delay pipeline overflow");
            for (double& stage : stages_) stage = initial_quantity;
        }
    }

    std::size_t order() const noexcept { return stages_.size(); }
    DelayKind kind() const noexcept { return kind_; }
    const std::vector<double>& stages() const noexcept { return stages_; }

    double output(double delay_time) const {
        const double stage_duration = validated_stage_duration(delay_time);
        const double value = kind_ == DelayKind::material
            ? stages_.back() / stage_duration : stages_.back();
        if (!std::isfinite(value)) throw std::overflow_error("delay output overflow");
        return value;
    }

    double pipeline() const {
        if (kind_ != DelayKind::material)
            throw std::logic_error("information delay has no material pipeline");
        const double total = std::accumulate(stages_.begin(), stages_.end(), 0.0);
        if (!std::isfinite(total)) throw std::overflow_error("delay pipeline overflow");
        return total;
    }

    void step(double input, double delay_time, double dt) {
        const double stage_duration = validated_stage_duration(delay_time);
        if (!std::isfinite(input) || (kind_ == DelayKind::material && input < 0))
            throw std::invalid_argument("invalid delay input");
        if (!std::isfinite(dt) || dt <= 0 || dt > stage_duration)
            throw std::invalid_argument("delay step must be positive and no larger than stage duration");
        std::vector<double> next(stages_.size());
        if (kind_ == DelayKind::material) {
            std::vector<double> outflow(stages_.size());
            for (std::size_t i = 0; i < stages_.size(); ++i)
                outflow[i] = stages_[i] / stage_duration;
            for (std::size_t i = 0; i < stages_.size(); ++i) {
                const double incoming = i == 0 ? input : outflow[i - 1];
                next[i] = stages_[i] + dt * (incoming - outflow[i]);
            }
        } else {
            for (std::size_t i = 0; i < stages_.size(); ++i) {
                const double target = i == 0 ? input : stages_[i - 1];
                next[i] = stages_[i] + dt * (target - stages_[i]) / stage_duration;
            }
        }
        for (double value : next) {
            if (!std::isfinite(value) || (kind_ == DelayKind::material && value < 0))
                throw std::overflow_error("delay stage left its valid numeric domain");
        }
        stages_ = std::move(next);
    }

private:
    static std::size_t validated_order(std::size_t order) {
        if (order < 1 || order > max_delay_order)
            throw std::invalid_argument("Euler delay supports orders 1..255");
        return order;
    }

    static void validate_duration(double duration) {
        if (!std::isfinite(duration) || duration <= 0)
            throw std::invalid_argument("delay time must be finite and positive");
    }

    double validated_stage_duration(double delay_time) const {
        validate_duration(delay_time);
        const double result = delay_time / static_cast<double>(order());
        if (result <= 0 || !std::isfinite(result))
            throw std::invalid_argument("delay stage duration is invalid");
        return result;
    }

    DelayKind kind_;
    std::vector<double> stages_;
};

} // namespace ankurafathom::sd
