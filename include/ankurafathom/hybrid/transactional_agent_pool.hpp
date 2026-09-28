#pragma once

#include "ankurafathom/abm/sync_population.hpp"
#include "ankurafathom/des/resource_pool.hpp"
#include "ankurafathom/hybrid/agent_pool_broker.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace ankurafathom::hybrid {

// Correctness-first transaction owner for one agent-backed resource pool.
// It stages every component on copies, then publishes all state with one swap.
template <typename Agent>
class TransactionalAgentPool {
public:
    using Population = abm::SyncPopulation<Agent>;
    using Capacity = typename AgentPoolBroker<Agent>::Capacity;
    using Phase = std::function<Agent(std::size_t, const typename Population::Snapshot&,
                                      const AgentPoolBroker<Agent>&)>;

    struct Change {
        double time = 0;
        std::vector<des::Release> releases;
        std::vector<std::pair<std::uint64_t, Agent>> updates;
        std::vector<std::uint64_t> departures;
        std::vector<Agent> hires;
        bool run_registered_phases = false;
        std::vector<Phase> phases;
        std::vector<des::Seize> requests;
    };

    struct Result {
        double time = 0;
        std::vector<std::uint64_t> hired_ids;
        std::vector<des::Grant> grants;
        std::vector<des::AgentAssigned> assignments;
        std::vector<des::AgentUnassigned> unassignments;
    };

    TransactionalAgentPool(Population population, Capacity capacity,
                           std::size_t max_request_units)
        : state_(std::make_unique<State>(std::move(population), std::move(capacity),
                                         max_request_units)) {}

    TransactionalAgentPool(const TransactionalAgentPool& source)
        : state_(std::make_unique<State>(*source.state_)), now_(source.now_) {}

    TransactionalAgentPool& operator=(const TransactionalAgentPool& source) {
        if (this == &source) return *this;
        auto next = std::make_unique<State>(*source.state_);
        state_ = std::move(next);
        now_ = source.now_;
        return *this;
    }

    TransactionalAgentPool(TransactionalAgentPool&&) noexcept = default;
    TransactionalAgentPool& operator=(TransactionalAgentPool&&) noexcept = default;

    Result apply(const Change& change) {
        if (!std::isfinite(change.time) || change.time < now_)
            throw std::invalid_argument("agent pool transaction time is invalid");
        auto candidate = std::make_unique<State>(*state_);
        Result result;
        result.time = change.time;

        if (!change.releases.empty()) {
            std::vector<devs::Input<des::ResourceMessage>> bag;
            bag.reserve(change.releases.size());
            for (const auto& release : change.releases)
                bag.push_back({0, AgentPoolBroker<Agent>::input_port, release});
            candidate->broker.accept_at(change.time, bag);
            for (const auto& event : candidate->broker.output())
                result.unassignments.push_back(std::get<des::AgentUnassigned>(event.value));
            candidate->broker.internal_transition();
        }

        for (const auto& [id, value] : change.updates)
            candidate->population.replace(id, value);
        for (const auto id : change.departures)
            candidate->population.despawn(id);
        for (const auto& value : change.hires)
            result.hired_ids.push_back(candidate->population.spawn(value));
        if (change.run_registered_phases) candidate->population.step();
        for (const auto& phase : change.phases) {
            if (!phase) throw std::invalid_argument("empty transactional agent phase");
            const auto snapshot = candidate->population.records();
            std::vector<std::pair<std::uint64_t, Agent>> next;
            next.reserve(candidate->population.active_count());
            for (std::size_t i = 0; i < snapshot.size(); ++i)
                if (snapshot[i].alive)
                    next.emplace_back(snapshot[i].id, phase(i, snapshot, candidate->broker));
            for (auto& [id, value] : next)
                candidate->population.replace(id, std::move(value));
        }
        candidate->broker.preflight_current();
        const auto capacity = total_capacity(candidate->population, candidate->capacity);

        std::vector<devs::Input<des::ResourceMessage>> pool_bag;
        pool_bag.reserve(change.releases.size() + change.requests.size() + 1);
        for (const auto& release : change.releases)
            pool_bag.push_back({0, des::ResourcePool::input_port, release});
        pool_bag.push_back({0, des::ResourcePool::capacity_port, des::SetCapacity{capacity}});
        for (const auto& request : change.requests)
            pool_bag.push_back({0, des::ResourcePool::input_port, request});
        candidate->pool.external_transition(change.time - now_, pool_bag);

        std::vector<devs::Input<des::ResourceMessage>> grants;
        for (const auto& event : candidate->pool.output()) {
            const auto grant = std::get<des::Grant>(event.value);
            result.grants.push_back(grant);
            grants.push_back({0, AgentPoolBroker<Agent>::input_port, grant});
        }
        if (!grants.empty()) {
            candidate->pool.internal_transition();
            candidate->broker.accept_at(change.time, grants);
            for (const auto& event : candidate->broker.output())
                result.assignments.push_back(std::get<des::AgentAssigned>(event.value));
            candidate->broker.internal_transition();
        }

        if (candidate->pool.capacity() != capacity ||
            candidate->pool.allocated_units() != candidate->broker.allocated_units() ||
            candidate->pool.allocated() != candidate->broker.allocations() ||
            candidate->pool.available() + candidate->pool.allocated_units() != capacity)
            throw std::logic_error("agent pool transaction invariant failed");

        state_ = std::move(candidate);
        now_ = change.time;
        return result;
    }

    double now() const noexcept { return now_; }
    const Population& population() const noexcept { return state_->population; }
    const des::ResourcePool& pool() const noexcept { return state_->pool; }
    const AgentPoolBroker<Agent>& broker() const noexcept { return state_->broker; }

private:
    static std::size_t total_capacity(const Population& population, const Capacity& capacity) {
        if (!capacity) throw std::invalid_argument("empty agent pool capacity function");
        std::size_t total = 0;
        for (const auto& record : population.records()) {
            if (!record.alive) continue;
            const auto amount = capacity(record.value);
            if (amount > std::numeric_limits<std::size_t>::max() - total)
                throw std::overflow_error("agent pool capacity aggregate overflow");
            total += amount;
        }
        return total;
    }

    struct State {
        Population population;
        Capacity capacity;
        des::ResourcePool pool;
        AgentPoolBroker<Agent> broker;

        State(Population source, Capacity contribution, std::size_t max_request_units)
            : population(std::move(source)), capacity(std::move(contribution)),
              pool(total_capacity(population, capacity), max_request_units),
              broker(population, capacity) {
            if (!capacity) throw std::invalid_argument("empty agent pool capacity function");
        }

        State(const State& source)
            : population(source.population), capacity(source.capacity), pool(source.pool),
              broker(source.broker.clone_for_population(population)) {}
    };

    std::unique_ptr<State> state_;
    double now_ = 0;
};

} // namespace ankurafathom::hybrid
