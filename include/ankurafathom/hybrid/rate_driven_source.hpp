#pragma once

#include "ankurafathom/des/single_server.hpp"
#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/rng/philox.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace ankurafathom::hybrid {

// A bounded Poisson source with a piecewise-constant rate. Rate changes preserve
// the remaining unit-exponential hazard for the next entity.
template <typename Message>
class RateDrivenSource final : public devs::Atomic<Message> {
public:
    static constexpr std::uint32_t rate_port = 0;
    static constexpr std::uint32_t output_port = 1;
    static constexpr std::uint64_t max_entity_id = 0xFFFFFFFFFFFFULL;

    RateDrivenSource(std::uint64_t seed, std::uint32_t scenario, std::uint32_t replication,
                     std::uint64_t first_id, std::size_t max_count,
                     std::uint32_t stream, double service_rate)
        : seed_(seed), scenario_(scenario), replication_(replication),
          first_id_(first_id), max_count_(max_count), stream_(stream),
          service_rate_(service_rate) {
        if (scenario > 65535 || replication > 65535 || first_id > max_entity_id ||
            max_count == 0 || max_count > 1000000 || max_count - 1 > max_entity_id - first_id ||
            stream >= 65535 || !std::isfinite(service_rate) || service_rate <= 0)
            throw std::invalid_argument("invalid rate-driven source configuration");
        remaining_hazard_ = next_hazard(first_id_);
    }

    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<RateDrivenSource>(*this);
    }

    double time_advance() const override {
        if (generated_ == max_count_ || rate_ == 0)
            return std::numeric_limits<double>::infinity();
        if (remaining_hazard_ == 0) return 0;
        const double delay = remaining_hazard_ / rate_;
        if (!std::isfinite(delay) || delay <= 0 || clock_ + delay <= clock_)
            throw std::overflow_error("rate-driven arrival time did not advance");
        return delay;
    }

    std::vector<devs::PortValue<Message>> output() const override {
        const double delay = time_advance();
        if (!std::isfinite(delay)) return {};
        const std::uint64_t entity_id = first_id_ + generated_;
        const auto word = rng::draw(seed_, rng::DrawAddress{
            scenario_, replication_, entity_id, 0, stream_ + 1, 0})[0];
        const double service = rng::exponential(service_rate_, word);
        return {{output_port, Message{des::Entity{entity_id, clock_ + delay, service}}}};
    }

    void internal_transition() override {
        const double delay = time_advance();
        if (!std::isfinite(delay)) throw std::logic_error("rate source has no arrival");
        clock_ += delay;
        ++generated_;
        if (generated_ < max_count_) remaining_hazard_ = next_hazard(first_id_ + generated_);
        else remaining_hazard_ = 0;
    }

    void external_transition(double elapsed, const std::vector<devs::Input<Message>>& bag) override {
        RateDrivenSource candidate(*this);
        candidate.apply_rate(elapsed, bag);
        *this = std::move(candidate);
    }

    void confluent_transition(const std::vector<devs::Input<Message>>& bag) override {
        RateDrivenSource candidate(*this);
        candidate.internal_transition();
        candidate.apply_rate(0, bag);
        *this = std::move(candidate);
    }

    std::size_t generated_count() const noexcept { return generated_; }
    double rate() const noexcept { return rate_; }
    double remaining_hazard() const noexcept { return remaining_hazard_; }

private:
    double next_hazard(std::uint64_t entity_id) const {
        const auto word = rng::draw(seed_, rng::DrawAddress{
            scenario_, replication_, entity_id, 0, stream_, 0})[0];
        return rng::exponential(1, word);
    }

    void apply_rate(double elapsed, const std::vector<devs::Input<Message>>& bag) {
        if (!std::isfinite(elapsed) || elapsed < 0 || !std::isfinite(clock_ + elapsed) ||
            bag.size() != 1 || bag[0].port != rate_port)
            throw std::invalid_argument("invalid rate update");
        const double new_rate = std::get<double>(bag[0].value);
        if (!std::isfinite(new_rate) || new_rate < 0)
            throw std::invalid_argument("rate must be finite and nonnegative");
        if (generated_ < max_count_ && rate_ > 0) {
            const double consumed = rate_ * elapsed;
            const double tolerance = 1e-12 * std::max(1.0, remaining_hazard_);
            if (!std::isfinite(consumed) || consumed > remaining_hazard_ + tolerance)
                throw std::invalid_argument("rate update passed a scheduled arrival");
            remaining_hazard_ = std::max(0.0, remaining_hazard_ - consumed);
        }
        clock_ += elapsed;
        rate_ = new_rate;
    }

    std::uint64_t seed_;
    std::uint32_t scenario_;
    std::uint32_t replication_;
    std::uint64_t first_id_;
    std::size_t max_count_;
    std::uint32_t stream_;
    double service_rate_;
    std::size_t generated_ = 0;
    double clock_ = 0;
    double rate_ = 0;
    double remaining_hazard_ = 0;
};

} // namespace ankurafathom::hybrid
