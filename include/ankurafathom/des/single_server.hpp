#pragma once

#include "ankurafathom/devs/simulator.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ankurafathom::des {

struct Entity {
    std::uint64_t id = 0;
    double arrived_at = 0;
    double service_duration = 0;
    double completed_at = std::numeric_limits<double>::quiet_NaN();
    double entered_at = std::numeric_limits<double>::quiet_NaN();
    std::int32_t priority = 0;
};

template <typename Message = Entity>
class SingleServer final : public devs::Atomic<Message> {
public:
    static constexpr std::uint32_t input_port = 0;
    static constexpr std::uint32_t output_port = 1;

    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<SingleServer>(*this);
    }

    double time_advance() const override {
        return active_ ? deadline_ - clock_ : std::numeric_limits<double>::infinity();
    }

    std::optional<double> next_event_time() const override {
        return active_ ? deadline_ : std::numeric_limits<double>::infinity();
    }

    std::vector<devs::PortValue<Message>> output() const override {
        if (!active_) return {};
        Entity completed = *active_;
        completed.completed_at = deadline_;
        return {{output_port, Message{completed}}};
    }

    void internal_transition() override {
        if (!active_) throw std::logic_error("internal transition while server is idle");
        SingleServer candidate(*this);
        candidate.advance_to(candidate.deadline_);
        candidate.complete_active();
        candidate.start_next();
        *this = std::move(candidate);
    }

    void external_transition(double elapsed, const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || (elapsed > 0 && clock_ + elapsed <= clock_))
            throw std::invalid_argument("invalid server elapsed time");
        external_transition_at(clock_ + elapsed, (clock_ + elapsed) - clock_, bag);
    }

    void external_transition_at(double time, double elapsed,
                                const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || time - clock_ != elapsed)
            throw std::invalid_argument("inconsistent server timestamp");
        SingleServer candidate(*this);
        candidate.advance_to(time);
        candidate.accept(bag);
        candidate.start_next();
        *this = std::move(candidate);
    }

    void confluent_transition(const std::vector<devs::Input<Message>>& bag) override {
        if (!active_) throw std::logic_error("confluent transition while server is idle");
        SingleServer candidate(*this);
        candidate.advance_to(candidate.deadline_);
        candidate.complete_active();
        candidate.accept(bag);
        candidate.start_next();
        *this = std::move(candidate);
    }

    std::size_t waiting() const noexcept { return waiting_.size(); }
    bool busy() const noexcept { return active_.has_value(); }
    std::uint64_t accepted_count() const noexcept { return accepted_; }
    std::uint64_t completed_count() const noexcept { return completed_; }
    double total_waiting_time() const noexcept { return total_waiting_time_; }

    double mean_queue_length(double horizon) const {
        validate_horizon(horizon);
        return (queue_area_ + waiting_.size() * (horizon - clock_)) / horizon;
    }

    double utilization(double horizon) const {
        validate_horizon(horizon);
        return (busy_area_ + (active_ ? horizon - clock_ : 0)) / horizon;
    }

private:
    void validate_horizon(double horizon) const {
        if (!std::isfinite(horizon) || horizon <= 0 || horizon < clock_)
            throw std::invalid_argument("statistics horizon must be positive and not precede the last transition");
    }

    void advance_to(double time) {
        if (!std::isfinite(time) || time < clock_ || (active_ && time > deadline_))
            throw std::invalid_argument("invalid server event time");
        const double elapsed = time - clock_;
        queue_area_ += waiting_.size() * elapsed;
        if (active_) busy_area_ += elapsed;
        if (!std::isfinite(queue_area_) || !std::isfinite(busy_area_))
            throw std::overflow_error("server time-weighted statistics overflow");
        clock_ = time;
    }

    static const Entity& as_entity(const Message& message) {
        if constexpr (std::is_same_v<Message, Entity>) return message;
        else return std::get<Entity>(message);
    }

    void accept(const std::vector<devs::Input<Message>>& bag) {
        std::set<std::uint64_t> new_ids;
        for (const auto& input : bag) {
            const Entity& entity = as_entity(input.value);
            if (input.port != input_port || !std::isfinite(entity.arrived_at) ||
                entity.arrived_at < 0 ||
                !std::isfinite(entity.service_duration) || entity.service_duration <= 0 ||
                entity.arrived_at > clock_ || std::isfinite(entity.completed_at) ||
                seen_ids_.contains(entity.id) || !new_ids.insert(entity.id).second)
                throw std::invalid_argument("invalid arrival entity or port");
        }
        for (const auto& input : bag) {
            const Entity& entity = as_entity(input.value);
            waiting_.push_back(entity);
            seen_ids_.insert(entity.id);
            ++accepted_;
        }
    }

    void complete_active() {
        ++completed_;
        active_.reset();
        deadline_ = std::numeric_limits<double>::infinity();
    }

    void start_next() {
        if (active_ || waiting_.empty()) return;
        active_ = waiting_.front();
        waiting_.pop_front();
        total_waiting_time_ += clock_ - active_->arrived_at;
        if (!std::isfinite(total_waiting_time_))
            throw std::overflow_error("server waiting-time overflow");
        deadline_ = clock_ + active_->service_duration;
        if (!std::isfinite(deadline_) || deadline_ <= clock_)
            throw std::overflow_error("service deadline is not representable");
    }

    double clock_ = 0;
    double deadline_ = std::numeric_limits<double>::infinity();
    double queue_area_ = 0;
    double busy_area_ = 0;
    double total_waiting_time_ = 0;
    std::uint64_t accepted_ = 0;
    std::uint64_t completed_ = 0;
    std::deque<Entity> waiting_;
    std::optional<Entity> active_;
    std::set<std::uint64_t> seen_ids_;
};

} // namespace ankurafathom::des
