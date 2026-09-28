#pragma once

#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/hybrid/agent_pool_process.hpp"

#include <variant>

namespace ankurafathom::hybrid {

template <typename Agent>
using AgentPoolProcessMessage = std::variant<typename AgentPoolProcess<Agent>::Change,
                                            typename AgentPoolProcess<Agent>::Result>;

// Completion detection emits nothing. The transition combines due completions
// with same-step inputs and publishes only its committed result one microstep later.
template <typename Agent, typename Message = AgentPoolProcessMessage<Agent>>
class AgentPoolProcessAtomic final : public devs::Atomic<Message> {
public:
    using Core = AgentPoolProcess<Agent>;
    using Change = typename Core::Change;
    using Result = typename Core::Result;
    using Pool = typename Core::Pool;
    static constexpr std::uint32_t input_port = 0;
    static constexpr std::uint32_t output_port = 1;

    AgentPoolProcessAtomic(typename Pool::Population population,
                           typename Pool::Capacity capacity, std::size_t max_request_units)
        : AgentPoolProcessAtomic(Pool(std::move(population), std::move(capacity), max_request_units)) {}
    explicit AgentPoolProcessAtomic(Pool pool)
        : core_(std::move(pool)), clock_(core_.system().now()) {}

    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<AgentPoolProcessAtomic>(*this);
    }
    double time_advance() const override {
        if (pending_) return 0;
        return core_.next_completion() - clock_;
    }
    std::optional<double> next_event_time() const override {
        return pending_ ? clock_ : core_.next_completion();
    }
    std::vector<devs::PortValue<Message>> output() const override {
        if (!pending_) return {};
        return {{output_port, Message{*pending_}}};
    }
    void internal_transition() override {
        if (pending_) {
            pending_.reset();
            return;
        }
        const double time = core_.next_completion();
        if (!std::isfinite(time)) throw std::logic_error("delivery atomic has no internal event");
        Change completion;
        completion.workforce.time = time;
        commit(completion);
    }
    void external_transition(double elapsed, const std::vector<devs::Input<Message>>& bag) override {
        if (bag.empty()) throw std::invalid_argument("empty delivery input bag");
        const auto* first = std::get_if<Change>(&bag.front().value);
        if (!first) throw std::invalid_argument("invalid delivery input payload");
        external_transition_at(first->workforce.time, elapsed, bag);
    }
    void external_transition_at(double time, double elapsed,
                                const std::vector<devs::Input<Message>>& bag) override {
        if (pending_ || !std::isfinite(elapsed) || elapsed < 0 || !std::isfinite(time) ||
            time < clock_ || time - clock_ != elapsed)
            throw std::invalid_argument("invalid delivery timestamp, elapsed time, or pending result");
        // The kernel supplies its exact timestamp: distinct absolute values can
        // subtract to the same rounded elapsed duration, so elapsed alone is insufficient.
        commit(combine(time, bag));
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag) override {
        const double time = pending_ ? clock_ : core_.next_completion();
        if (!std::isfinite(time)) throw std::logic_error("delivery atomic is not imminent");
        // If pending, output() has already published that result in this step.
        // A failure leaves it pending for whole-step rollback or direct retry.
        commit(combine(time, bag));
    }
    const Core& core() const noexcept { return core_; }

private:
    static Change combine(double time, const std::vector<devs::Input<Message>>& bag) {
        if (bag.empty()) throw std::invalid_argument("empty delivery input bag");
        Change combined;
        combined.workforce.time = time;
        bool has_workforce = false;
        for (const auto& input : bag) {
            const auto* change = std::get_if<Change>(&input.value);
            if (input.port != input_port || !change || change->workforce.time != time)
                throw std::invalid_argument("invalid delivery input port, payload, or timestamp");
            const auto& workforce = change->workforce;
            if (!workforce.requests.empty() || !workforce.releases.empty())
                throw std::invalid_argument("delivery owns staffing requests and releases");
            const bool has_actions = !workforce.updates.empty() || !workforce.departures.empty() ||
                !workforce.hires.empty() || workforce.run_registered_phases || !workforce.phases.empty();
            if (has_actions) {
                if (has_workforce)
                    throw std::invalid_argument("delivery input bag needs one consolidated workforce command");
                combined.workforce = workforce;
                has_workforce = true;
            }
            combined.arrivals.insert(combined.arrivals.end(), change->arrivals.begin(), change->arrivals.end());
        }
        return combined;
    }
    void commit(const Change& change) {
        auto result = core_.apply(change);
        pending_ = std::move(result);
        clock_ = change.workforce.time;
    }

    Core core_;
    double clock_ = 0;
    std::optional<Result> pending_;
};

} // namespace ankurafathom::hybrid
