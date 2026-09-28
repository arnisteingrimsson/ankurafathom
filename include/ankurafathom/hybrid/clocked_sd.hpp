#pragma once

#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/sd/model.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ankurafathom::hybrid {

// An SD model driven by the DEVS clock. A pulse's destination stock is its port.
// The model integrates to an event timestamp before applying that event's pulses.
template <typename Message = double>
class ClockedSD final : public devs::Atomic<Message> {
public:
    ClockedSD(sd::Model model, double dt) : model_(std::move(model)), dt_(dt), next_tick_(dt) {
        if (!std::isfinite(dt) || dt <= 0) throw std::invalid_argument("invalid SD tick interval");
    }

    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<ClockedSD>(*this);
    }

    double time_advance() const override { return next_tick_ - time_; }
    std::optional<double> next_event_time() const override { return next_tick_; }
    std::vector<devs::PortValue<Message>> output() const override { return {}; }

    void internal_transition() override {
        integrate_to(next_tick_);
        advance_tick();
    }

    void external_transition(double elapsed, const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0)
            throw std::invalid_argument("invalid SD elapsed time");
        integrate_to(time_ + elapsed);
        apply_pulses(bag);
    }

    void external_transition_at(double time, double elapsed,
                                const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || time - time_ != elapsed)
            throw std::invalid_argument("invalid SD elapsed time");
        integrate_to(time);
        apply_pulses(bag);
    }

    void confluent_transition(const std::vector<devs::Input<Message>>& bag) override {
        integrate_to(next_tick_);
        apply_pulses(bag);
        advance_tick();
    }

    const sd::Model& model() const noexcept { return model_; }
    double time() const noexcept { return time_; }

private:
    void integrate_to(double target) {
        const double elapsed = target - time_;
        if (!std::isfinite(target) || elapsed < 0 || target > next_tick_ + 1e-9)
            throw std::invalid_argument("invalid SD event timestamp");
        if (elapsed > 0) model_.step(time_, elapsed);
        time_ = target;
    }

    static double as_pulse(const Message& message) {
        if constexpr (std::is_same_v<Message, double>) return message;
        else return std::get<double>(message);
    }

    void apply_pulses(const std::vector<devs::Input<Message>>& bag) {
        for (const auto& input : bag) model_.add_to_stock(input.port, as_pulse(input.value));
    }

    void advance_tick() {
        if (tick_index_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("SD tick index exhausted");
        ++tick_index_;
        next_tick_ = static_cast<double>(tick_index_) * dt_;
        if (!std::isfinite(next_tick_) || next_tick_ <= time_)
            throw std::overflow_error("SD tick time did not advance");
    }

    sd::Model model_;
    double dt_;
    double time_ = 0;
    std::uint64_t tick_index_ = 1;
    double next_tick_;
};

} // namespace ankurafathom::hybrid
