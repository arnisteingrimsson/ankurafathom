#pragma once

#include "ankurafathom/abm/sync_population.hpp"
#include "ankurafathom/des/resource_pool.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ankurafathom::hybrid {

// Maps aggregate pool grants to stable-ID agent assignments. The population is
// observed and must outlive the broker. Allocation state lives in this ledger.
template <typename Agent>
class AgentPoolBroker final : public devs::Atomic<des::ResourceMessage> {
public:
    static constexpr std::uint32_t input_port = 0;
    static constexpr std::uint32_t output_port = 1;
    using Capacity = std::function<std::size_t(const Agent&)>;

    AgentPoolBroker(const abm::SyncPopulation<Agent>& population, Capacity capacity)
        : population_(&population), capacity_(std::move(capacity)) {
        if (!capacity_) throw std::invalid_argument("empty agent pool capacity function");
    }

    std::unique_ptr<devs::Atomic<des::ResourceMessage>> clone() const override {
        return std::make_unique<AgentPoolBroker>(*this);
    }

    double time_advance() const override {
        return pending_.empty() ? std::numeric_limits<double>::infinity() : 0;
    }

    std::vector<devs::PortValue<des::ResourceMessage>> output() const override {
        std::vector<devs::PortValue<des::ResourceMessage>> result;
        result.reserve(pending_.size());
        for (const auto& message : pending_) result.push_back({output_port, message});
        return result;
    }

    void internal_transition() override {
        if (pending_.empty()) throw std::logic_error("agent pool broker has no internal event");
        pending_.clear();
    }

    void external_transition(double elapsed,
                             const std::vector<devs::Input<des::ResourceMessage>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0 || !std::isfinite(clock_ + elapsed))
            throw std::invalid_argument("invalid agent pool broker elapsed time");
        accept_at(clock_ + elapsed, bag);
    }

    // A transaction owner already knows the absolute time. Preserve it exactly
    // so a later transaction at that same time cannot appear to run backward.
    void accept_at(double time, const std::vector<devs::Input<des::ResourceMessage>>& bag) {
        if (!std::isfinite(time) || time < clock_)
            throw std::invalid_argument("invalid agent pool broker timestamp");
        accept(bag, false);
        clock_ = time;
    }

    void confluent_transition(const std::vector<devs::Input<des::ResourceMessage>>& bag) override {
        if (pending_.empty()) throw std::logic_error("agent pool broker is not imminent");
        accept(bag, true);
    }

    std::size_t allocated_to(std::uint64_t agent_id) const noexcept {
        const auto it = used_.find(agent_id);
        return it == used_.end() ? 0 : it->second;
    }

    std::size_t allocated_units() const noexcept {
        std::size_t total = 0;
        for (const auto& [_, units] : used_) total += units;
        return total;
    }

    std::size_t allocations() const noexcept { return assignments_.size(); }
    double clock() const noexcept { return clock_; }

    AgentPoolBroker clone_for_population(const abm::SyncPopulation<Agent>& population) const {
        AgentPoolBroker copy(*this);
        copy.population_ = &population;
        return copy;
    }

    // Call before committing an ABM departure or capacity change. A failed
    // preflight leaves both the population and broker unchanged.
    void preflight(const typename abm::SyncPopulation<Agent>::Snapshot& candidate) const {
        std::map<std::uint64_t, std::size_t> capacities;
        std::size_t total = 0;
        for (const auto& record : candidate) {
            if (!record.alive) continue;
            const auto amount = capacity_(record.value);
            if (amount > std::numeric_limits<std::size_t>::max() - total)
                throw std::overflow_error("agent pool capacity aggregate overflow");
            total += amount;
            if (!capacities.emplace(record.id, amount).second)
                throw std::invalid_argument("duplicate agent ID in capacity candidate");
        }
        for (const auto& [id, used] : used_) {
            const auto it = capacities.find(id);
            if (it == capacities.end() || used > it->second)
                throw std::invalid_argument("agent capacity change strands active allocation");
        }
    }

    void preflight_current() const { preflight(population_->records()); }

private:
    void accept(const std::vector<devs::Input<des::ResourceMessage>>& bag,
                bool clear_pending) {
        for (const auto& input : bag)
            if (input.port != input_port ||
                (!std::holds_alternative<des::Grant>(input.value) &&
                 !std::holds_alternative<des::Release>(input.value)))
                throw std::invalid_argument("agent pool broker received invalid input or port");

        auto used = used_;
        auto assignments = assignments_;
        auto seen = seen_ids_;
        auto pending = pending_;
        if (clear_pending) pending.clear();
        for (const auto& input : bag) {
            const auto* release = std::get_if<des::Release>(&input.value);
            if (!release) continue;
            const auto it = assignments.find(release->request_id);
            if (it == assignments.end())
                throw std::invalid_argument("agent release references an inactive allocation");
            for (const auto& share : it->second) {
                auto used_it = used.find(share.agent_id);
                if (used_it == used.end() || used_it->second < share.units)
                    throw std::logic_error("agent allocation ledger is inconsistent");
                used_it->second -= share.units;
                if (used_it->second == 0) used.erase(used_it);
                pending.emplace_back(des::AgentUnassigned{release->request_id,
                                                          share.agent_id, share.units});
            }
            assignments.erase(it);
        }

        std::map<std::uint64_t, std::size_t> capacities;
        std::size_t total_capacity = 0;
        for (const auto& record : population_->records()) {
            if (!record.alive) continue;
            const auto amount = capacity_(record.value);
            if (amount > std::numeric_limits<std::size_t>::max() - total_capacity)
                throw std::overflow_error("agent pool capacity aggregate overflow");
            total_capacity += amount;
            if (!capacities.emplace(record.id, amount).second)
                throw std::logic_error("duplicate active agent ID");
        }
        for (const auto& [id, units] : used) {
            const auto it = capacities.find(id);
            if (it == capacities.end() || units > it->second)
                throw std::invalid_argument("agent population cannot cover active allocations");
        }

        for (const auto& input : bag) {
            const auto* grant = std::get_if<des::Grant>(&input.value);
            if (!grant) continue;
            if (grant->units == 0 || !seen.insert(grant->request_id).second)
                throw std::invalid_argument("invalid or duplicate agent grant");
            std::size_t remaining = grant->units;
            std::vector<des::AgentAssigned> shares;
            for (const auto& [id, capacity] : capacities) {
                const auto already = used.contains(id) ? used.at(id) : 0;
                const auto free = capacity - already;
                const auto take = free < remaining ? free : remaining;
                if (take == 0) continue;
                shares.push_back({grant->request_id, id, take});
                used[id] += take;
                remaining -= take;
                if (remaining == 0) break;
            }
            if (remaining != 0)
                throw std::invalid_argument("agent population cannot cover aggregate grant");
            for (const auto& share : shares) pending.emplace_back(share);
            assignments.emplace(grant->request_id, std::move(shares));
        }

        used_ = std::move(used);
        assignments_ = std::move(assignments);
        seen_ids_ = std::move(seen);
        pending_ = std::move(pending);
    }

    const abm::SyncPopulation<Agent>* population_;
    Capacity capacity_;
    double clock_ = 0;
    std::map<std::uint64_t, std::size_t> used_;
    std::map<std::uint64_t, std::vector<des::AgentAssigned>> assignments_;
    std::set<std::uint64_t> seen_ids_;
    std::vector<des::ResourceMessage> pending_;
};

} // namespace ankurafathom::hybrid
