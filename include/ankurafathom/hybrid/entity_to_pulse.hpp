#pragma once

#include "ankurafathom/des/single_server.hpp"
#include "ankurafathom/devs/simulator.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <vector>

namespace ankurafathom::hybrid {

template <typename Message>
class EntityToPulse final : public devs::Atomic<Message> {
public:
    static constexpr std::uint32_t input_port = 0;
    static constexpr std::uint32_t output_port = 1;
    using Amount = std::function<double(const des::Entity&)>;

    explicit EntityToPulse(Amount amount) : amount_(std::move(amount)) {
        if (!amount_) throw std::invalid_argument("empty pulse amount function");
    }

    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<EntityToPulse>(*this);
    }

    double time_advance() const override {
        return pending_ ? 0 : std::numeric_limits<double>::infinity();
    }
    std::vector<devs::PortValue<Message>> output() const override {
        if (!pending_) return {};
        return {{output_port, Message{amount_pending_}}};
    }
    void internal_transition() override {
        pending_ = false;
        amount_pending_ = 0;
    }
    void external_transition(double elapsed, const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0) throw std::invalid_argument("invalid bridge elapsed time");
        append(bag);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag) override {
        pending_ = false;
        amount_pending_ = 0;
        append(bag);
    }

private:
    void append(const std::vector<devs::Input<Message>>& bag) {
        for (const auto& input : bag) {
            if (input.port != input_port) throw std::invalid_argument("wrong entity-to-pulse input port");
            const des::Entity& entity = std::get<des::Entity>(input.value);
            const double amount = amount_(entity);
            if (!std::isfinite(amount) || !std::isfinite(amount_pending_ + amount))
                throw std::invalid_argument("non-finite pulse amount");
            amount_pending_ += amount;
            pending_ = true;
        }
    }

    Amount amount_;
    double amount_pending_ = 0;
    bool pending_ = false;
};

} // namespace ankurafathom::hybrid
