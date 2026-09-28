#pragma once

#include "ankurafathom/abm/sync_population.hpp"
#include "ankurafathom/des/resource_pool.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ankurafathom::hybrid {

// Samples a committed ABM population and publishes its integer resource capacity.
// The population is observed, never modified, and must outlive this atomic.
template <typename Agent>
class PopulationToCapacity final : public devs::Atomic<des::ResourceMessage> {
public:
    static constexpr std::uint32_t trigger_port = 0;
    static constexpr std::uint32_t output_port = 1;
    using Contribution = std::function<std::size_t(const Agent&)>;

    PopulationToCapacity(const abm::SyncPopulation<Agent>& population, Contribution contribution)
        : population_(&population), contribution_(std::move(contribution)) {
        if (!contribution_) throw std::invalid_argument("empty agent capacity contribution");
    }

    std::unique_ptr<devs::Atomic<des::ResourceMessage>> clone() const override {
        return std::make_unique<PopulationToCapacity>(*this);
    }

    double time_advance() const override {
        return pending_ ? 0 : std::numeric_limits<double>::infinity();
    }

    std::vector<devs::PortValue<des::ResourceMessage>> output() const override {
        if (!pending_) return {};
        return {{output_port, des::SetCapacity{pending_capacity_}}};
    }

    void internal_transition() override {
        if (!pending_) throw std::logic_error("population capacity has no internal event");
        pending_ = false;
    }

    void external_transition(double elapsed,
                             const std::vector<devs::Input<des::ResourceMessage>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || !std::isfinite(clock_ + elapsed) ||
            pending_ || bag.size() != 1 || bag[0].port != trigger_port ||
            !std::holds_alternative<des::CapacitySample>(bag[0].value))
            throw std::invalid_argument("invalid population capacity trigger");

        std::size_t total = 0;
        for (const auto& record : population_->records()) {
            if (!record.alive) continue;
            const auto amount = contribution_(record.value);
            if (amount > std::numeric_limits<std::size_t>::max() - total)
                throw std::overflow_error("agent capacity aggregate overflow");
            total += amount;
        }
        clock_ += elapsed;
        pending_capacity_ = total;
        pending_ = true;
    }

    void confluent_transition(const std::vector<devs::Input<des::ResourceMessage>>& bag) override {
        if (!pending_) throw std::logic_error("population capacity is not imminent");
        PopulationToCapacity candidate(*this);
        candidate.pending_ = false;
        candidate.external_transition(0, bag);
        *this = std::move(candidate);
    }

    std::size_t pending_capacity() const noexcept { return pending_capacity_; }

private:
    const abm::SyncPopulation<Agent>* population_;
    Contribution contribution_;
    double clock_ = 0;
    std::size_t pending_capacity_ = 0;
    bool pending_ = false;
};

} // namespace ankurafathom::hybrid
