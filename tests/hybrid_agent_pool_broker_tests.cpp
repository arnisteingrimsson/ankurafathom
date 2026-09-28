#include "ankurafathom/hybrid/agent_pool_broker.hpp"
#include "ankurafathom/hybrid/population_to_capacity.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>

namespace {

using ankurafathom::abm::SyncPopulation;
using ankurafathom::des::AgentAssigned;
using ankurafathom::des::AgentUnassigned;
using ankurafathom::des::CapacitySample;
using ankurafathom::des::Grant;
using ankurafathom::des::Release;
using ankurafathom::des::ResourceMessage;
using ankurafathom::des::ResourcePool;
using ankurafathom::des::Seize;
using ankurafathom::devs::Simulator;
using ankurafathom::hybrid::AgentPoolBroker;
using ankurafathom::hybrid::PopulationToCapacity;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_grants_release_and_safe_departure() {
    SyncPopulation<std::size_t> population;
    const auto first = population.spawn(1);
    const auto second = population.spawn(2);
    Simulator<ResourceMessage> simulator;
    auto pool = std::make_unique<ResourcePool>(3, 2);
    auto* pool_ptr = pool.get();
    auto broker = std::make_unique<AgentPoolBroker<std::size_t>>(
        population, [](std::size_t units) { return units; });
    auto* broker_ptr = broker.get();
    const auto pool_id = simulator.add(std::move(pool));
    const auto broker_id = simulator.add(std::move(broker));
    const auto bridge_id = simulator.add(
        std::make_unique<PopulationToCapacity<std::size_t>>(
            population, [](std::size_t units) { return units; }));
    simulator.connect(pool_id, ResourcePool::output_port, broker_id,
                      AgentPoolBroker<std::size_t>::input_port);
    simulator.connect(bridge_id, PopulationToCapacity<std::size_t>::output_port,
                      pool_id, ResourcePool::capacity_port);

    simulator.inject(0, bridge_id, 0, CapacitySample{});
    (void)simulator.run_until(0);
    simulator.inject(0, pool_id, ResourcePool::input_port, Seize{10, 2});
    simulator.inject(0, pool_id, ResourcePool::input_port, Seize{11, 1});
    const auto trace = simulator.run_until(0);
    std::vector<AgentAssigned> assignments;
    for (const auto& event : trace)
        for (const auto& emission : event.emissions)
            if (const auto* assigned = std::get_if<AgentAssigned>(&emission.value))
                assignments.push_back(*assigned);
    require(assignments.size() == 3 &&
            assignments[0].request_id == 10 && assignments[0].agent_id == first &&
            assignments[0].units == 1 &&
            assignments[1].request_id == 10 && assignments[1].agent_id == second &&
            assignments[1].units == 1 &&
            assignments[2].request_id == 11 && assignments[2].agent_id == second &&
            assignments[2].units == 1 &&
            broker_ptr->allocated_to(first) == 1 && broker_ptr->allocated_to(second) == 2 &&
            broker_ptr->allocated_units() == pool_ptr->allocated_units(),
            "aggregate grants must map to stable-ID agent shares");

    auto departing = population.records();
    departing[static_cast<std::size_t>(first)].alive = false;
    bool rejected = false;
    try { broker_ptr->preflight(departing); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && population.active_count() == 2 && broker_ptr->allocated_to(first) == 1,
            "departure preflight must reject an agent with active work");

    simulator.inject(1, pool_id, ResourcePool::input_port, Release{10});
    simulator.inject(1, broker_id, AgentPoolBroker<std::size_t>::input_port, Release{10});
    const auto released = simulator.run_until(1);
    std::vector<AgentUnassigned> removals;
    for (const auto& event : released)
        for (const auto& emission : event.emissions)
            if (const auto* removed = std::get_if<AgentUnassigned>(&emission.value))
                removals.push_back(*removed);
    require(removals.size() == 2 && broker_ptr->allocated_to(first) == 0 &&
            broker_ptr->allocated_to(second) == 1 &&
            broker_ptr->allocated_units() == pool_ptr->allocated_units(),
            "release must free exactly its agent shares");

    broker_ptr->preflight(departing);
    population.despawn(first);
    simulator.inject(1, bridge_id, 0, CapacitySample{});
    (void)simulator.run_until(1);
    require(pool_ptr->capacity() == 2 && pool_ptr->allocated_units() == 1 &&
            broker_ptr->allocated_to(second) == 1,
            "safe departure must resize the pool without losing remaining work");
}

void test_invalid_grant_bag_rolls_back() {
    SyncPopulation<std::size_t> population;
    (void)population.spawn(2);
    AgentPoolBroker<std::size_t> broker(population,
        [](std::size_t units) { return units; });
    const std::vector<ankurafathom::devs::Input<ResourceMessage>> duplicate{
        {0, 0, Grant{7, 1}}, {0, 0, Grant{7, 1}}};
    bool rejected = false;
    try { broker.external_transition(0, duplicate); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && broker.allocations() == 0 && broker.allocated_units() == 0 &&
            broker.output().empty(),
            "duplicate grant bag must not leave partial agent allocations");

    const std::vector<ankurafathom::devs::Input<ResourceMessage>> too_large{
        {0, 0, Grant{8, 3}}};
    rejected = false;
    try { broker.external_transition(0, too_large); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && broker.allocations() == 0 && broker.allocated_units() == 0,
            "unassignable grant must leave broker state unchanged");

    broker.external_transition(0, {{0, 0, Grant{7, 1}}});
    broker.internal_transition();
    broker.external_transition(0, {{0, 0, Release{7}}});
    broker.internal_transition();
    rejected = false;
    try { broker.external_transition(0, {{0, 0, Grant{7, 1}}}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && broker.allocations() == 0 && broker.allocated_units() == 0,
            "released request IDs must never be reused");

    auto candidate = population.records();
    candidate.push_back({1, std::numeric_limits<std::size_t>::max(), true});
    rejected = false;
    try { broker.preflight(candidate); }
    catch (const std::overflow_error&) { rejected = true; }
    require(rejected && broker.allocations() == 0,
            "population capacity overflow must reject preflight");
}

} // namespace

int main() {
    try {
        test_grants_release_and_safe_departure();
        test_invalid_grant_bag_rolls_back();
        std::cout << "agent pool broker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
