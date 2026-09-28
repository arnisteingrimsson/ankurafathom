#pragma once

#include "ankurafathom/hybrid/transactional_agent_pool.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace ankurafathom::hybrid {

template <typename Agent>
using AgentPoolMessage = std::variant<typename TransactionalAgentPool<Agent>::Change,
                                      typename TransactionalAgentPool<Agent>::Result>;

// One DEVS atomic owns the workforce transaction. No pool/broker intermediate
// state is visible to other coupled atomics.
template <typename Agent>
class TransactionalAgentPoolAtomic final : public devs::Atomic<AgentPoolMessage<Agent>> {
public:
    static constexpr std::uint32_t input_port = 0;
    static constexpr std::uint32_t output_port = 1;
    using Core = TransactionalAgentPool<Agent>;
    using Message = AgentPoolMessage<Agent>;
    using Change = typename Core::Change;
    using Result = typename Core::Result;

    TransactionalAgentPoolAtomic(typename Core::Population population,
                                 typename Core::Capacity capacity,
                                 std::size_t max_request_units)
        : core_(std::move(population), std::move(capacity), max_request_units) {}

    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<TransactionalAgentPoolAtomic>(*this);
    }

    double time_advance() const override {
        return pending_ ? 0 : std::numeric_limits<double>::infinity();
    }

    std::vector<devs::PortValue<Message>> output() const override {
        if (!pending_) return {};
        return {{output_port, Message{pending_result_}}};
    }

    void internal_transition() override {
        if (!pending_) throw std::logic_error("agent pool atomic has no internal event");
        pending_ = false;
    }

    void external_transition(double elapsed,
                             const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || !std::isfinite(clock_ + elapsed) ||
            pending_ || bag.size() != 1 || bag[0].port != input_port ||
            !std::holds_alternative<Change>(bag[0].value))
            throw std::invalid_argument("invalid agent pool transaction input");
        const auto& change = std::get<Change>(bag[0].value);
        if (change.time != clock_ + elapsed)
            throw std::invalid_argument("agent pool transaction timestamp mismatch");
        Result result = core_.apply(change);
        pending_result_ = std::move(result);
        pending_ = true;
        clock_ += elapsed;
    }

    void confluent_transition(const std::vector<devs::Input<Message>>& bag) override {
        if (!pending_) throw std::logic_error("agent pool atomic is not imminent");
        pending_ = false;
        try {
            external_transition(0, bag);
        } catch (...) {
            pending_ = true;
            throw;
        }
    }

    const Core& core() const noexcept { return core_; }

private:
    Core core_;
    double clock_ = 0;
    Result pending_result_;
    bool pending_ = false;
};

} // namespace ankurafathom::hybrid
