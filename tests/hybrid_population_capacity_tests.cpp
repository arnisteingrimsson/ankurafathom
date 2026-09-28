#include "ankurafathom/hybrid/population_to_capacity.hpp"

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ankurafathom::abm::SyncPopulation;
using ankurafathom::des::CapacitySample;
using ankurafathom::des::Grant;
using ankurafathom::des::Release;
using ankurafathom::des::ResourceMessage;
using ankurafathom::des::ResourcePool;
using ankurafathom::des::Seize;
using ankurafathom::devs::Simulator;
using ankurafathom::hybrid::PopulationToCapacity;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_population_capacity_drives_fifo_pool() {
    SyncPopulation<std::size_t> population;
    const auto first = population.spawn(1);
    Simulator<ResourceMessage> simulator;
    auto pool = std::make_unique<ResourcePool>(1);
    auto* observed = pool.get();
    const auto pool_id = simulator.add(std::move(pool));
    const auto bridge_id = simulator.add(
        std::make_unique<PopulationToCapacity<std::size_t>>(
            population, [](std::size_t units) { return units; }));
    simulator.connect(bridge_id, PopulationToCapacity<std::size_t>::output_port,
                      pool_id, ResourcePool::capacity_port);

    simulator.inject(0, bridge_id, 0, CapacitySample{});
    (void)simulator.run_until(0);
    simulator.inject(0, pool_id, ResourcePool::input_port, Seize{1, 1});
    simulator.inject(0, pool_id, ResourcePool::input_port, Seize{2, 1});
    const auto initial = simulator.run_until(0);
    require(initial.size() == 2 && initial[1].emissions.size() == 1 &&
            std::get<Grant>(initial[1].emissions[0].value).request_id == 1 &&
            observed->capacity() == 1 && observed->waiting() == 1,
            "initial agent capacity must match a one-unit static pool");

    const auto second = population.spawn(1);
    simulator.inject(1, bridge_id, 0, CapacitySample{});
    const auto expanded = simulator.run_until(1);
    std::vector<std::uint64_t> grants;
    for (const auto& event : expanded)
        for (const auto& emission : event.emissions)
            if (const auto* grant = std::get_if<Grant>(&emission.value))
                grants.push_back(grant->request_id);
    require(grants == std::vector<std::uint64_t>{2} && observed->capacity() == 2 &&
            observed->allocated_units() == 2 && observed->waiting() == 0,
            "new agent capacity must grant the existing FIFO waiter");

    population.despawn(first);
    simulator.inject(2, pool_id, ResourcePool::input_port, Release{1});
    (void)simulator.run_until(2);
    simulator.inject(2, bridge_id, 0, CapacitySample{});
    (void)simulator.run_until(2);
    require(observed->capacity() == 1 && observed->allocated_units() == 1 &&
            observed->available() == 0,
            "departed agent must reduce capacity after work is released");

    simulator.inject(3, pool_id, ResourcePool::input_port, Release{2});
    (void)simulator.run_until(3);
    population.despawn(second);
    simulator.inject(3, bridge_id, 0, CapacitySample{});
    (void)simulator.run_until(3);
    require(observed->capacity() == 0 && observed->available() == 0 &&
            observed->allocated_units() == 0,
            "zero active agents must yield zero pool capacity");

    simulator.inject(4, pool_id, ResourcePool::input_port, Seize{3, 1});
    (void)simulator.run_until(4);
    require(observed->waiting() == 1 && observed->allocated_units() == 0,
            "a staffing request must wait when no agents are active");
    (void)population.spawn(1);
    simulator.inject(5, bridge_id, 0, CapacitySample{});
    const auto recovered = simulator.run_until(5);
    grants.clear();
    for (const auto& event : recovered)
        for (const auto& emission : event.emissions)
            if (const auto* grant = std::get_if<Grant>(&emission.value))
                grants.push_back(grant->request_id);
    require(grants == std::vector<std::uint64_t>{3} && observed->capacity() == 1 &&
            observed->allocated_units() == 1,
            "new hire must release demand queued during a zero-capacity interval");
}

void test_capacity_aggregate_overflow_rolls_back() {
    SyncPopulation<std::size_t> population;
    (void)population.spawn(std::numeric_limits<std::size_t>::max());
    (void)population.spawn(1);
    PopulationToCapacity<std::size_t> bridge(population,
        [](std::size_t units) { return units; });
    const std::vector<ankurafathom::devs::Input<ResourceMessage>> trigger{
        {0, 0, CapacitySample{}}};
    bool rejected = false;
    try { bridge.external_transition(0, trigger); }
    catch (const std::overflow_error&) { rejected = true; }
    require(rejected && !std::isfinite(bridge.time_advance()),
            "overflowed capacity sample must leave bridge passive");
    population.despawn(1);
    bridge.external_transition(0, trigger);
    require(std::get<ankurafathom::des::SetCapacity>(bridge.output()[0].value).units ==
                std::numeric_limits<std::size_t>::max(),
            "valid sample must work after an overflowed sample");
}

void test_constant_population_matches_static_pool() {
    SyncPopulation<std::size_t> population;
    (void)population.spawn(1);
    (void)population.spawn(1);
    Simulator<ResourceMessage> agent_sim;
    const auto agent_pool_id = agent_sim.add(std::make_unique<ResourcePool>(1));
    const auto bridge_id = agent_sim.add(
        std::make_unique<PopulationToCapacity<std::size_t>>(
            population, [](std::size_t units) { return units; }));
    agent_sim.connect(bridge_id, 1, agent_pool_id, ResourcePool::capacity_port);
    agent_sim.inject(0, bridge_id, 0, CapacitySample{});
    (void)agent_sim.run_until(0);

    Simulator<ResourceMessage> static_sim;
    const auto static_pool_id = static_sim.add(std::make_unique<ResourcePool>(2));
    for (auto* simulator : {&agent_sim, &static_sim}) {
        const auto pool_id = simulator == &agent_sim ? agent_pool_id : static_pool_id;
        simulator->inject(0, pool_id, ResourcePool::input_port, Seize{1, 1});
        simulator->inject(0, pool_id, ResourcePool::input_port, Seize{2, 1});
        simulator->inject(0, pool_id, ResourcePool::input_port, Seize{3, 1});
        simulator->inject(1, pool_id, ResourcePool::input_port, Release{1});
        simulator->inject(2, pool_id, ResourcePool::input_port, Release{2});
    }
    const auto trace_agent = agent_sim.run_until(2);
    const auto trace_static = static_sim.run_until(2);
    const auto grants = [](const auto& trace) {
        std::vector<std::pair<double, std::uint64_t>> result;
        for (const auto& event : trace)
            for (const auto& emission : event.emissions)
                if (const auto* grant = std::get_if<Grant>(&emission.value))
                    result.emplace_back(event.time, grant->request_id);
        return result;
    };
    require(grants(trace_agent) == grants(trace_static) &&
            grants(trace_agent) ==
                std::vector<std::pair<double, std::uint64_t>>{{0, 1}, {0, 2}, {1, 3}},
            "constant agent-backed capacity must match static pool grant trace");
}

} // namespace

int main() {
    try {
        test_population_capacity_drives_fifo_pool();
        test_capacity_aggregate_overflow_rolls_back();
        test_constant_population_matches_static_pool();
        std::cout << "population-to-capacity hybrid tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
