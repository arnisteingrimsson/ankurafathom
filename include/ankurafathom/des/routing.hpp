#pragma once

#include "ankurafathom/des/single_server.hpp"
#include "ankurafathom/rng/philox.hpp"

namespace ankurafathom::des {

struct BernoulliRouting {
    double match_probability;
    std::uint64_t seed = 0;
    std::uint32_t scenario = 0;
    std::uint32_t replication = 0;
    std::uint32_t stream = 0;

    bool matches(std::uint64_t entity) const {
        return rng::bernoulli(match_probability,
            rng::draw(seed, {scenario, replication, entity, 0, stream, 0})[0]);
    }
};

// Addressed or priority-based binary selection. Publication is a zero-time DEVS event.
// The discard specialization consumes entities without turning them into completions.
template <typename Message = Entity, bool Discard = false>
class RoutingNode final : public devs::Atomic<Message> {
public:
    static constexpr std::uint32_t input_port = 0;
    static constexpr std::uint32_t match_port = 1;
    static constexpr std::uint32_t otherwise_port = 2;
    explicit RoutingNode(std::int32_t priority_at_most = 0) : threshold_(priority_at_most) {}
    explicit RoutingNode(BernoulliRouting probability) requires (!Discard)
        : threshold_(0), probability_(probability) {
        if (!std::isfinite(probability.match_probability) || probability.match_probability < 0 ||
            probability.match_probability > 1)
            throw std::invalid_argument("routing probability must be in [0,1]");
        (void)rng::pack_counter({probability.scenario, probability.replication, 0, 0, probability.stream, 0});
    }
    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<RoutingNode>(*this);
    }
    double time_advance() const override {
        return pending_.empty() ? std::numeric_limits<double>::infinity() : 0;
    }
    std::optional<double> next_event_time() const override {
        return pending_.empty() ? std::numeric_limits<double>::infinity() : clock_;
    }
    std::vector<devs::PortValue<Message>> output() const override { return pending_; }
    void internal_transition() override {
        if (pending_.empty()) throw std::logic_error("routing node has no publication");
        pending_.clear();
    }
    void external_transition(double elapsed, const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || (elapsed > 0 && clock_ + elapsed <= clock_))
            throw std::invalid_argument("invalid routing elapsed time");
        external_transition_at(clock_ + elapsed, (clock_ + elapsed) - clock_, bag);
    }
    void external_transition_at(double time, double elapsed,
                                const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(time) || !std::isfinite(elapsed) || elapsed < 0 || time - clock_ != elapsed ||
            (!pending_.empty() && elapsed != 0))
            throw std::invalid_argument("inconsistent routing timestamp");
        RoutingNode candidate(*this);
        candidate.clock_ = time;
        candidate.accept(bag);
        *this = std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag) override {
        if (pending_.empty()) throw std::logic_error("routing node has no publication");
        RoutingNode candidate(*this);
        candidate.pending_.clear();
        candidate.accept(bag);
        *this = std::move(candidate);
    }
    std::size_t received_count() const noexcept { return seen_.size(); }
    std::size_t matched_count() const noexcept { return matched_; }
    std::size_t otherwise_count() const noexcept { return seen_.size() - matched_; }
    const std::vector<Entity>& discarded() const noexcept { return discarded_; }
private:
    static const Entity& as_entity(const Message& message) {
        if constexpr (std::is_same_v<Message, Entity>) return message;
        else return std::get<Entity>(message);
    }
    void accept(const std::vector<devs::Input<Message>>& bag) {
        for (const auto& input : bag) {
            const auto& e = as_entity(input.value);
            if (input.port != input_port || !std::isfinite(e.arrived_at) || e.arrived_at < 0 ||
                e.arrived_at > clock_ || !std::isfinite(e.service_duration) || e.service_duration <= 0 ||
                (!std::isnan(e.completed_at) && (!std::isfinite(e.completed_at) ||
                    e.completed_at < e.arrived_at || e.completed_at > clock_)) ||
                !seen_.insert(e.id).second)
                throw std::invalid_argument("invalid or duplicate routing entity");
            if constexpr (Discard) discarded_.push_back(e);
            else {
                const bool match = probability_ ? probability_->matches(e.id) : e.priority <= threshold_;
                if (match) ++matched_;
                pending_.push_back({match ? match_port : otherwise_port, Message{e}});
            }
        }
    }
    std::int32_t threshold_;
    std::optional<BernoulliRouting> probability_;
    double clock_ = 0;
    std::set<std::uint64_t> seen_;
    std::vector<devs::PortValue<Message>> pending_;
    std::vector<Entity> discarded_;
    std::size_t matched_ = 0;
};

template <typename Message = Entity>
using PriorityRouter = RoutingNode<Message>;
template <typename Message = Entity>
using ProbabilityRouter = RoutingNode<Message>;
template <typename Message = Entity>
using DiscardSink = RoutingNode<Message, true>;

} // namespace ankurafathom::des
