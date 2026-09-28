#pragma once

#include "ankurafathom/hybrid/agent_pool_process.hpp"

namespace ankurafathom::hybrid {

// One constant stock-unit amount per completed engagement, regardless of staffing units.
template <typename Agent, typename Message>
class AgentPoolCompletionToPulse final : public devs::Atomic<Message> {
public:
    using Result = typename AgentPoolProcess<Agent>::Result;
    static constexpr std::uint32_t input_port = 0;
    static constexpr std::uint32_t output_port = 1;

    explicit AgentPoolCompletionToPulse(double amount) : amount_(amount) {
        if (!std::isfinite(amount) || amount <= 0)
            throw std::invalid_argument("completion pulse amount must be finite and positive");
    }
    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<AgentPoolCompletionToPulse>(*this);
    }
    double time_advance() const override {
        return pending_ > 0 ? 0 : std::numeric_limits<double>::infinity();
    }
    std::vector<devs::PortValue<Message>> output() const override {
        if (pending_ == 0) return {};
        return {{output_port, Message{pending_}}};
    }
    void internal_transition() override { pending_ = 0; }
    void external_transition(double elapsed, const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0)
            throw std::invalid_argument("invalid completion bridge elapsed time");
        accept(bag, pending_);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag) override {
        accept(bag, 0);
    }

private:
    void accept(const std::vector<devs::Input<Message>>& bag, double candidate) {
        for (const auto& input : bag) {
            const auto* result = std::get_if<Result>(&input.value);
            if (input.port != input_port || !result)
                throw std::invalid_argument("invalid completion bridge port or payload");
            candidate += static_cast<double>(result->completed.size()) * amount_;
            if (!std::isfinite(candidate)) throw std::overflow_error("completion pulse overflow");
        }
        pending_ = candidate;
    }
    double amount_;
    double pending_ = 0;
};

} // namespace ankurafathom::hybrid
