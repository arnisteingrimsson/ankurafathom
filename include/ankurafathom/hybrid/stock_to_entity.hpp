#pragma once

#include "ankurafathom/des/single_server.hpp"
#include "ankurafathom/devs/simulator.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace ankurafathom::hybrid {

// A tick-sampled stock level may create at most one DES entity per observation.
// The orchestrator injects the committed stock after draining all prior events at that time.
template <typename Message>
class StockToEntity final : public devs::Atomic<Message> {
public:
    static constexpr std::uint32_t input_port = 0;
    static constexpr std::uint32_t output_port = 1;
    static constexpr std::uint64_t max_entity_id = 0xFFFFFFFFFFFFULL;

    StockToEntity(double threshold, double service_duration,
                  std::uint64_t first_id, std::size_t max_count)
        : threshold_(threshold), service_duration_(service_duration),
          first_id_(first_id), max_count_(max_count) {
        if (!std::isfinite(threshold) || !std::isfinite(service_duration) ||
            service_duration <= 0 || max_count == 0 || first_id > max_entity_id ||
            max_count - 1 > max_entity_id - first_id)
            throw std::invalid_argument("invalid stock-to-entity bridge configuration");
    }

    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<StockToEntity>(*this);
    }

    double time_advance() const override {
        return pending_ ? 0 : std::numeric_limits<double>::infinity();
    }
    std::vector<devs::PortValue<Message>> output() const override {
        if (!pending_) return {};
        return {{output_port, Message{*pending_}}};
    }
    void internal_transition() override { pending_.reset(); }

    void external_transition(double elapsed, const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || !std::isfinite(clock_ + elapsed) ||
            pending_ || bag.size() != 1 || bag[0].port != input_port)
            throw std::invalid_argument("invalid stock observation");
        const double level = std::get<double>(bag[0].value);
        if (!std::isfinite(level)) throw std::invalid_argument("non-finite stock observation");
        const double time = clock_ + elapsed;
        if (level >= threshold_ && generated_ < max_count_) {
            pending_ = des::Entity{first_id_ + generated_, time, service_duration_};
            ++generated_;
        }
        clock_ = time;
    }

    void confluent_transition(const std::vector<devs::Input<Message>>& bag) override {
        StockToEntity candidate(*this);
        candidate.pending_.reset();
        candidate.external_transition(0, bag);
        *this = std::move(candidate);
    }

    std::size_t generated_count() const noexcept { return generated_; }

private:
    double threshold_;
    double service_duration_;
    std::uint64_t first_id_;
    std::size_t max_count_;
    std::size_t generated_ = 0;
    double clock_ = 0;
    std::optional<des::Entity> pending_;
};

} // namespace ankurafathom::hybrid
