#pragma once

#include "ankurafathom/des/single_server.hpp"
#include "ankurafathom/rng/philox.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ankurafathom::des {

// Finite deterministic arrival source; see docs/SEMANTICS.md, DES process path.
template <typename Message = Entity>
class ScheduledSource final : public devs::Atomic<Message> {
public:
    static constexpr std::uint32_t output_port = 0;

    explicit ScheduledSource(std::vector<Entity> schedule) : schedule_(std::move(schedule)) {
        std::set<std::uint64_t> ids;
        double previous = 0;
        for (const auto& entity : schedule_) {
            if (!std::isfinite(entity.arrived_at) || entity.arrived_at < previous ||
                !std::isfinite(entity.service_duration) || entity.service_duration <= 0 ||
                std::isfinite(entity.completed_at) || !ids.insert(entity.id).second)
                throw std::invalid_argument("invalid scheduled entity or ordering");
            previous = entity.arrived_at;
        }
    }

    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<ScheduledSource>(*this);
    }

    double time_advance() const override {
        return index_ < schedule_.size() ? schedule_[index_].arrived_at - clock_
                                         : std::numeric_limits<double>::infinity();
    }

    std::optional<double> next_event_time() const override {
        return index_ < schedule_.size() ? schedule_[index_].arrived_at
                                        : std::numeric_limits<double>::infinity();
    }

    std::vector<devs::PortValue<Message>> output() const override {
        std::vector<devs::PortValue<Message>> result;
        if (index_ >= schedule_.size()) return result;
        const double time = schedule_[index_].arrived_at;
        for (std::size_t i = index_; i < schedule_.size() && schedule_[i].arrived_at == time; ++i)
            result.push_back({output_port, Message{schedule_[i]}});
        return result;
    }

    void internal_transition() override {
        if (index_ >= schedule_.size()) throw std::logic_error("source has no internal event");
        const double time = schedule_[index_].arrived_at;
        clock_ = time;
        do { ++index_; }
        while (index_ < schedule_.size() && schedule_[index_].arrived_at == time);
    }

    void external_transition(double, const std::vector<devs::Input<Message>>&) override {
        throw std::logic_error("scheduled source cannot receive input");
    }
    void confluent_transition(const std::vector<devs::Input<Message>>&) override {
        throw std::logic_error("scheduled source cannot receive input");
    }

    std::size_t emitted_count() const noexcept { return index_; }

private:
    std::vector<Entity> schedule_;
    std::size_t index_ = 0;
    double clock_ = 0;
};

// Deterministic M/M arrival and service schedule from independent addressed streams.
// The caller chooses distinct stream pairs and entity ID ranges for independent sources.
inline std::vector<Entity> exponential_schedule(std::size_t count, double arrival_rate,
                                                double service_rate, double start,
                                                std::uint64_t seed, rng::DrawAddress base) {
    constexpr std::uint64_t max_entity = 0xFFFFFFFFFFFFULL;
    if (!std::isfinite(arrival_rate) || arrival_rate <= 0 ||
        !std::isfinite(service_rate) || service_rate <= 0 ||
        !std::isfinite(start) || start < 0 || base.stream >= 65535 ||
        base.entity > max_entity || count > max_entity - base.entity + 1)
        throw std::invalid_argument("invalid exponential schedule parameters or address range");
    (void)rng::pack_counter(base);
    std::vector<Entity> schedule;
    schedule.reserve(count);
    double arrival = start;
    const std::uint64_t first_id = base.entity;
    for (std::size_t i = 0; i < count; ++i) {
        base.entity = first_id + i;
        const auto arrival_word = rng::draw(seed, base)[0];
        rng::DrawAddress service_address = base;
        ++service_address.stream;
        const auto service_word = rng::draw(seed, service_address)[0];
        const double previous_arrival = arrival;
        arrival += rng::exponential(arrival_rate, arrival_word);
        const double duration = rng::exponential(service_rate, service_word);
        if (!std::isfinite(arrival) || arrival <= previous_arrival ||
            !std::isfinite(duration) || duration <= 0)
            throw std::overflow_error("exponential schedule produced an invalid time");
        schedule.push_back(Entity{base.entity, arrival, duration});
    }
    return schedule;
}

// Passive completion recorder; see docs/SEMANTICS.md, DES process path.
template <typename Message = Entity>
class CompletionSink final : public devs::Atomic<Message> {
public:
    static constexpr std::uint32_t input_port = 0;

    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<CompletionSink>(*this);
    }

    double time_advance() const override { return std::numeric_limits<double>::infinity(); }
    std::vector<devs::PortValue<Message>> output() const override { return {}; }
    void internal_transition() override { throw std::logic_error("sink has no internal event"); }
    void confluent_transition(const std::vector<devs::Input<Message>>&) override {
        throw std::logic_error("sink has no internal event");
    }

    void external_transition(double elapsed, const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || (elapsed > 0 && clock_ + elapsed <= clock_))
            throw std::invalid_argument("invalid sink elapsed time");
        external_transition_at(clock_ + elapsed, (clock_ + elapsed) - clock_, bag);
    }

    void external_transition_at(double time, double elapsed,
                                const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(time) || !std::isfinite(elapsed) || elapsed < 0 || time - clock_ != elapsed)
            throw std::invalid_argument("inconsistent sink timestamp");
        auto seen = seen_ids_;
        auto completed = completed_;
        double total_cycle = total_cycle_time_;
        for (const auto& input : bag) {
            const Entity& entity = as_entity(input.value);
            const double tolerance = 1e-9 * std::max(1.0, std::abs(time));
            if (input.port != input_port || !std::isfinite(entity.arrived_at) ||
                !std::isfinite(entity.completed_at) || !std::isfinite(entity.service_duration) ||
                entity.service_duration <= 0 || entity.arrived_at > entity.completed_at ||
                std::abs(entity.completed_at - time) > tolerance ||
                !seen.insert(entity.id).second)
                throw std::invalid_argument("invalid or duplicate completed entity");
            total_cycle += entity.completed_at - entity.arrived_at;
            if (!std::isfinite(total_cycle)) throw std::overflow_error("sink cycle-time overflow");
            completed.push_back(entity);
        }
        clock_ = time;
        seen_ids_ = std::move(seen);
        completed_ = std::move(completed);
        total_cycle_time_ = total_cycle;
    }

    const std::vector<Entity>& completed() const noexcept { return completed_; }
    std::size_t completed_count() const noexcept { return completed_.size(); }
    double total_cycle_time() const noexcept { return total_cycle_time_; }
    double mean_cycle_time() const {
        if (completed_.empty()) throw std::logic_error("mean cycle time needs completed entities");
        return total_cycle_time_ / static_cast<double>(completed_.size());
    }

private:
    static const Entity& as_entity(const Message& message) {
        if constexpr (std::is_same_v<Message, Entity>) return message;
        else return std::get<Entity>(message);
    }

    double clock_ = 0;
    double total_cycle_time_ = 0;
    std::set<std::uint64_t> seen_ids_;
    std::vector<Entity> completed_;
};

} // namespace ankurafathom::des
