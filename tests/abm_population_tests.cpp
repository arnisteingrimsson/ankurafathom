#include "ankurafathom/abm/sync_population.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_jacobi_and_ordered_phases() {
    ankurafathom::abm::SyncPopulation<int> population;
    population.spawn(0);
    population.spawn(1);
    population.spawn(2);
    population.add_phase([](std::size_t i, const auto& snapshot) {
        return snapshot[(i + 1) % snapshot.size()].value;
    });
    population.add_phase([](std::size_t i, const auto& snapshot) {
        return snapshot[i].value + 1;
    });
    population.step();
    const auto& agents = population.records();
    require(agents[0].value == 2 && agents[1].value == 3 && agents[2].value == 1,
            "phase updates must be Jacobi within each phase and ordered between phases");
}

void test_stable_ids_and_inactive_agents() {
    ankurafathom::abm::SyncPopulation<int> population;
    const auto first = population.spawn(10);
    const auto second = population.spawn(20);
    population.despawn(first);
    const auto third = population.spawn(30);
    require(first == 0 && second == 1 && third == 2, "agent IDs must be monotonic and never reused");
    require(population.active_count() == 2, "active count is wrong after despawn and spawn");
    population.add_phase([](std::size_t i, const auto& snapshot) { return snapshot[i].value + 5; });
    population.step();
    require(population.records()[0].value == 10 && !population.records()[0].alive,
            "inactive agent must not update");
    require(population.records()[1].value == 25 && population.records()[2].value == 35,
            "active agents did not update");
}

void test_phase_mutation_rejected_without_partial_commit() {
    ankurafathom::abm::SyncPopulation<int> population;
    population.spawn(1);
    population.spawn(2);
    population.add_phase([](std::size_t i, const auto& snapshot) { return snapshot[i].value + 1; });
    population.add_phase([&population](std::size_t i, const auto& snapshot) {
        if (i == 1) population.spawn(9);
        return snapshot[i].value;
    });
    bool caught = false;
    try { population.step(); } catch (const std::logic_error&) { caught = true; }
    require(caught, "phase-time spawn must be rejected");
    require(population.active_count() == 2 && population.records()[0].value == 1 &&
            population.records()[1].value == 2,
            "failed population step must not partially commit");
}

struct ThrowingAgent {
    int value;
    static inline bool fail=false;
    explicit ThrowingAgent(int v):value(v) {}
    ThrowingAgent(const ThrowingAgent&)=default;
    ThrowingAgent(ThrowingAgent&& other):value(other.value) {
        if(fail) throw std::runtime_error("injected spawn move failure");
    }
    ThrowingAgent& operator=(const ThrowingAgent&)=default;
};

void test_failed_spawn_preserves_identity() {
    ankurafathom::abm::SyncPopulation<ThrowingAgent> population;
    population.spawn(ThrowingAgent(4));
    ThrowingAgent::fail=true;
    bool caught=false;
    try { population.spawn(ThrowingAgent(9)); } catch(const std::runtime_error&) { caught=true; }
    ThrowingAgent::fail=false;
    require(caught && population.active_count()==1 && population.records().size()==1,"failed spawn changed records");
    const auto id=population.spawn(ThrowingAgent(6));
    require(id==1 && population.records()[1].id==1,"failed spawn consumed an ID and broke indexed identity");
    population.despawn(id);
    require(population.active_count()==1,"retry agent cannot be retired by its ID");
}

} // namespace

int main() {
    try {
        test_jacobi_and_ordered_phases();
        test_stable_ids_and_inactive_agents();
        test_phase_mutation_rejected_without_partial_commit();
        test_failed_spawn_preserves_identity();
        std::cout << "ABM population tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
