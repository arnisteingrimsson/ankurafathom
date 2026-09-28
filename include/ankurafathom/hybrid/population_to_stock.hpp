#pragma once

#include "ankurafathom/abm/sync_population.hpp"
#include "ankurafathom/devs/simulator.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ankurafathom::hybrid {

// Publishes the change in an active-agent aggregate after a committed ABM phase.
// The population is observed, never modified, and must outlive this atomic.
template <typename Agent, typename Message = double>
class PopulationToStock final : public devs::Atomic<Message> {
public:
    static constexpr std::uint32_t trigger_port = 0;
    static constexpr std::uint32_t output_port = 1;
    using Contribution = std::function<double(const Agent&)>;

    PopulationToStock(const abm::SyncPopulation<Agent>& population, Contribution contribution)
        : population_(&population), contribution_(std::move(contribution)) {
        if (!contribution_) throw std::invalid_argument("empty agent aggregate contribution");
    }

    std::unique_ptr<devs::Atomic<Message>> clone() const override {
        return std::make_unique<PopulationToStock>(*this);
    }

    double time_advance() const override {
        return pending_ ? 0 : std::numeric_limits<double>::infinity();
    }
    std::vector<devs::PortValue<Message>> output() const override {
        if (!pending_) return {};
        return {{output_port, Message{pending_delta_}}};
    }
    void internal_transition() override {
        pending_ = false;
        pending_delta_ = 0;
    }
    void external_transition(double elapsed, const std::vector<devs::Input<Message>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || !std::isfinite(clock_ + elapsed) ||
            pending_ || bag.size() != 1 || bag[0].port != trigger_port ||
            as_double(bag[0].value) != 0)
            throw std::invalid_argument("invalid population aggregate trigger");
        double total = 0;
        const auto snapshot = population_->records();
        for (const auto& record : snapshot) {
            if (!record.alive) continue;
            const double amount = contribution_(record.value);
            if (!std::isfinite(amount) || amount < 0 || !std::isfinite(total + amount))
                throw std::invalid_argument("invalid agent aggregate contribution");
            total += amount;
        }
        const double delta = total - published_total_;
        if (!std::isfinite(delta)) throw std::overflow_error("agent aggregate delta overflow");
        clock_ += elapsed;
        published_total_ = total;
        pending_delta_ = delta;
        pending_ = delta != 0;
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag) override {
        PopulationToStock candidate(*this);
        candidate.pending_ = false;
        candidate.pending_delta_ = 0;
        candidate.external_transition(0, bag);
        *this = std::move(candidate);
    }

    double published_total() const noexcept { return published_total_; }

private:
    static double as_double(const Message& message) {
        if constexpr (std::is_same_v<Message, double>) return message;
        else return std::get<double>(message);
    }

    const abm::SyncPopulation<Agent>* population_;
    Contribution contribution_;
    double clock_ = 0;
    double published_total_ = 0;
    double pending_delta_ = 0;
    bool pending_ = false;
};

} // namespace ankurafathom::hybrid
