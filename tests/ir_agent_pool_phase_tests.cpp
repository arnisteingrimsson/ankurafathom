#include "ankurafathom/ir/model.hpp"
#include "ankurafathom/hybrid/transactional_agent_pool.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace ankurafathom;
using Pool = hybrid::TransactionalAgentPool<std::size_t>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_trajectory(const ir::Model& model) {
    const auto rows = ir::run(model);
    // capacity, allocated, available, waiting, active, agent capacities 0/1/2,
    // agent 0 allocated, request 3 ever granted. Derived by hand.
    const std::array<std::array<double, 10>, 5> expected{{
        {{6, 6, 0, 1, 2, 2, 4, 0, 2, 0}},
        {{12, 6, 6, 0, 2, 2, 10, 0, 2, 1}},
        {{6, 2, 4, 0, 2, 4, 0, 2, 2, 1}},
        {{24, 2, 22, 0, 2, 16, 0, 8, 2, 1}},
        {{0, 0, 0, 0, 2, 0, 0, 0, 0, 1}}
    }};
    require(rows.size() == 50, "wrong phase observation count");
    for (std::size_t step = 0; step < expected.size(); ++step)
        for (std::size_t metric = 0; metric < expected[step].size(); ++metric) {
            const auto& row = rows.at(step * 10 + metric);
            require(row.time == step, "wrong phase observation time");
            require(row.value == expected[step][metric], "phase trajectory differs from hand oracle");
        }

    // Independent C++ rules, without the IR expression evaluator.
    abm::SyncPopulation<std::size_t> population;
    (void)population.spawn(2);
    (void)population.spawn(4);
    Pool reference(std::move(population), [](const auto& value) { return value; }, 4);
    const Pool::Phase balance = [](std::size_t i, const auto& snapshot, const auto& broker) {
        std::size_t total = 0, active = 0;
        for (const auto& agent : snapshot) {
            if (!agent.alive) continue;
            total += agent.value;
            ++active;
        }
        return broker.allocated_to(snapshot[i].id) + (total - broker.allocated_units()) / active;
    };
    const Pool::Phase grow = [](std::size_t i, const auto& snapshot, const auto&) {
        return snapshot[i].value * 2;
    };
    const Pool::Phase retain = [](std::size_t i, const auto& snapshot, const auto& broker) {
        return broker.allocated_to(snapshot[i].id);
    };
    for (std::size_t step = 0; step < 5; ++step) {
        Pool::Change change;
        change.time = step;
        if (step == 0) change.requests = {{1, 2}, {2, 4}, {3, 2}};
        if (step == 1) {
            change.time = 0.5;
            change.releases = {{1}};
            change.phases = {balance, grow};
        }
        if (step == 2) {
            change.releases = {{2}};
            change.departures = {1};
            change.hires = {4};
            change.phases = {balance};
        }
        if (step == 3) change.phases = {grow, grow};
        if (step == 4) {
            change.releases = {{3}};
            change.phases = {retain};
        }
        const auto result = reference.apply(change);
        if (step == 1)
            require(result.grants.size() == 1 && result.grants[0].request_id == 3,
                    "queued grant must follow all phases");
        require(rows[step * 10].value == reference.pool().capacity(), "C++ phase capacity differs");
        require(rows[step * 10 + 1].value == reference.pool().allocated_units(),
                "C++ phase allocations differ");
        require(rows[step * 10 + 3].value == reference.pool().waiting(), "C++ phase queue differs");
        for (std::size_t id = 0; id < reference.population().records().size(); ++id) {
            const auto& agent = reference.population().records()[id];
            require(rows[step * 10 + 5 + id].value == (agent.alive ? agent.value : 0),
                    "C++ individual phase state differs");
        }
    }
}

void test_failures(const ir::Model& model) {
    for (const auto* source : {"capacity / 3", "capacity * -1", "capacity * 1000000",
                               "capacity / (1 - agent_id)"}) {
        auto bad = model;
        bad.agent_pool->phases[0].capacity_expression = ir::Expression(source);
        bool caught = false;
        try { (void)ir::run(bad); }
        catch (const ir::Error& error) {
            caught = error.code == "IR_AGENT_POOL_PHASE" &&
                     error.pointer == "/agent_pool/schedule/1/phases/0";
        }
        require(caught, "invalid phase result needs an invocation diagnostic");
    }
    auto unsafe = model;
    unsafe.agent_pool->phases[0].capacity_expression = ir::Expression("capacity * 0");
    bool caught = false;
    try { (void)ir::run(unsafe); }
    catch (const ir::Error& error) {
        caught = error.code == "IR_AGENT_POOL_RUNTIME" &&
                 error.pointer == "/agent_pool/schedule/1";
    }
    require(caught, "capacity rule must not strand existing assignments");
    test_trajectory(model);
}

void test_empty_population(const ir::Model& model) {
    auto empty = model;
    empty.agent_pool->initial_capacities.clear();
    empty.agent_pool->schedule.clear();
    ir::AgentPoolChange change{};
    change.phases = {0}; // Aggregate expression divides by active only when an agent exists.
    empty.agent_pool->schedule.push_back(change);
    const auto rows = ir::run(empty);
    for (const auto& row : rows) require(row.value == 0, "empty population phase must be a no-op");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected phase fixture path");
        const auto model = ir::load_file(argv[1]);
        test_trajectory(model);
        test_failures(model);
        test_empty_population(model);
        std::cout << "agent-pool phase tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
