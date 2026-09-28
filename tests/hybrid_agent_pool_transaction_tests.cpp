#include "ankurafathom/hybrid/transactional_agent_pool.hpp"
#include "ankurafathom/hybrid/transactional_agent_pool_atomic.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ankurafathom::abm::SyncPopulation;
using ankurafathom::des::Release;
using ankurafathom::des::Seize;
using ankurafathom::hybrid::TransactionalAgentPool;
using ankurafathom::hybrid::TransactionalAgentPoolAtomic;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_release_departure_and_queue_commit_together() {
    SyncPopulation<std::size_t> population;
    const auto first = population.spawn(1);
    const auto second = population.spawn(1);
    TransactionalAgentPool<std::size_t> agent_pool(
        std::move(population), [](std::size_t value) { return value; }, 1);

    TransactionalAgentPool<std::size_t>::Change initial;
    initial.requests = {Seize{1, 1}, Seize{2, 1}, Seize{3, 1}};
    const auto started = agent_pool.apply(initial);
    require(started.grants.size() == 2 && started.grants[0].request_id == 1 &&
            started.grants[1].request_id == 2 && started.assignments.size() == 2 &&
            agent_pool.pool().waiting() == 1 && agent_pool.broker().allocated_to(first) == 1 &&
            agent_pool.broker().allocated_to(second) == 1,
            "initial grants must commit with agent ownership");

    TransactionalAgentPool<std::size_t>::Change unsafe;
    unsafe.time = 1;
    unsafe.departures = {first};
    bool rejected = false;
    try { (void)agent_pool.apply(unsafe); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && agent_pool.now() == 0 && agent_pool.population().active_count() == 2 &&
            agent_pool.pool().capacity() == 2 && agent_pool.pool().waiting() == 1 &&
            agent_pool.broker().allocated_to(first) == 1,
            "unsafe departure must roll back population, pool, broker, and time");

    TransactionalAgentPool<std::size_t>::Change departure;
    departure.time = 1;
    departure.releases = {Release{1}};
    departure.departures = {first};
    const auto departed = agent_pool.apply(departure);
    require(departed.unassignments.size() == 1 && departed.grants.empty() &&
            agent_pool.now() == 1 && agent_pool.population().active_count() == 1 &&
            agent_pool.pool().capacity() == 1 && agent_pool.pool().waiting() == 1 &&
            agent_pool.pool().allocated_units() == 1 && agent_pool.broker().allocated_to(first) == 0,
            "release and departure must shrink capacity before a waiter can regrant");

    TransactionalAgentPool<std::size_t>::Change invalid_request;
    invalid_request.time = 2;
    invalid_request.releases = {Release{2}};
    invalid_request.departures = {second};
    invalid_request.requests = {Seize{4, 2}};
    rejected = false;
    try { (void)agent_pool.apply(invalid_request); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && agent_pool.now() == 1 && agent_pool.population().active_count() == 1 &&
            agent_pool.pool().waiting() == 1 && agent_pool.pool().allocated_units() == 1 &&
            agent_pool.broker().allocated_to(second) == 1,
            "invalid request must roll back staged release and departure");

    TransactionalAgentPool<std::size_t>::Change hire;
    hire.time = 2;
    hire.hires = {1};
    const auto hired = agent_pool.apply(hire);
    require(hired.hired_ids == std::vector<std::uint64_t>{2} &&
            hired.grants.size() == 1 && hired.grants[0].request_id == 3 &&
            agent_pool.pool().capacity() == 2 && agent_pool.pool().waiting() == 0 &&
            agent_pool.broker().allocated_units() == agent_pool.pool().allocated_units(),
            "hire must expand capacity and grant the oldest waiter atomically");
}

void test_capacity_update_and_callback_failure_roll_back() {
    SyncPopulation<std::size_t> population;
    const auto agent = population.spawn(1);
    TransactionalAgentPool<std::size_t> pool(std::move(population),
        [](std::size_t value) -> std::size_t {
            if (value == 99) throw std::invalid_argument("bad capacity value");
            return value;
        }, 1);
    TransactionalAgentPool<std::size_t>::Change request;
    request.requests = {Seize{7, 1}};
    (void)pool.apply(request);

    TransactionalAgentPool<std::size_t>::Change shrink;
    shrink.time = 1;
    shrink.updates = {{agent, 0}};
    bool rejected = false;
    try { (void)pool.apply(shrink); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && pool.now() == 0 && pool.population().records()[0].value == 1 &&
            pool.pool().capacity() == 1 && pool.broker().allocated_to(agent) == 1,
            "capacity reduction below active work must roll back");

    shrink.updates = {{agent, 99}};
    rejected = false;
    try { (void)pool.apply(shrink); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && pool.now() == 0 && pool.population().records()[0].value == 1,
            "capacity callback failure must roll back");

    shrink.releases = {Release{7}};
    shrink.updates = {{agent, 0}};
    (void)pool.apply(shrink);
    require(pool.now() == 1 && pool.population().records()[0].value == 0 &&
            pool.pool().capacity() == 0 && pool.broker().allocated_units() == 0,
            "release and capacity reduction must commit together");
}

void test_devs_atomic_exposes_only_committed_results() {
    using Atomic = TransactionalAgentPoolAtomic<std::size_t>;
    using Message = ankurafathom::hybrid::AgentPoolMessage<std::size_t>;
    SyncPopulation<std::size_t> population;
    (void)population.spawn(1);
    auto atomic = std::make_unique<Atomic>(
        std::move(population), [](std::size_t value) { return value; }, 1);
    auto* observed = atomic.get();
    ankurafathom::devs::Simulator<Message> simulator;
    const auto id = simulator.add(std::move(atomic));

    Atomic::Change initial;
    initial.time = 0;
    initial.requests = {Seize{1, 1}, Seize{2, 1}};
    simulator.inject(0, id, Atomic::input_port, initial);
    const auto admitted = simulator.step();
    require(admitted && admitted->emissions.empty() && observed->core().pool().waiting() == 1,
            "transaction must commit before its zero-time result output");

    Atomic::Change hire;
    hire.time = 0;
    hire.hires = {1};
    simulator.inject(0, id, Atomic::input_port, hire);
    const auto confluent = simulator.step();
    require(confluent && confluent->emissions.size() == 1 &&
            std::get<Atomic::Result>(confluent->emissions[0].value).grants[0].request_id == 1 &&
            observed->core().pool().capacity() == 2 && observed->core().pool().waiting() == 0,
            "confluent transaction must output prior result before committing new change");
    const auto next = simulator.step();
    require(next && next->emissions.size() == 1 &&
            std::get<Atomic::Result>(next->emissions[0].value).grants[0].request_id == 2 &&
            observed->core().broker().allocated_units() == 2,
            "new transaction result must emit at a later same-time microstep");

    Atomic::Change mismatch;
    mismatch.time = 2;
    const std::vector<ankurafathom::devs::Input<Message>> wrong_time{
        {0, Atomic::input_port, mismatch}};
    bool rejected = false;
    try { observed->external_transition(1, wrong_time); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && observed->core().now() == 0 && observed->core().pool().capacity() == 2,
            "timestamp mismatch must not alter the committed transaction state");

    SyncPopulation<std::size_t> other_population;
    (void)other_population.spawn(1);
    Atomic direct(std::move(other_population),
                  [](std::size_t value) { return value; }, 1);
    direct.external_transition(0, {{0, Atomic::input_port, initial}});
    rejected = false;
    try { direct.confluent_transition(wrong_time); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && direct.output().size() == 1 &&
            std::get<Atomic::Result>(direct.output()[0].value).grants[0].request_id == 1 &&
            direct.core().now() == 0,
            "failed confluent input must preserve the prior pending result");
}

void test_transactional_abm_phases_read_staged_ledger() {
    SyncPopulation<std::size_t> population;
    const auto first = population.spawn(1);
    const auto second = population.spawn(1);
    population.add_phase([](std::size_t index, const auto& snapshot) {
        return snapshot[index].value + 1;
    });
    TransactionalAgentPool<std::size_t> pool(std::move(population),
        [](std::size_t value) { return value; }, 1);
    TransactionalAgentPool<std::size_t>::Change start;
    start.requests = {Seize{1, 1}};
    (void)pool.apply(start);

    TransactionalAgentPool<std::size_t>::Change invalid;
    invalid.time = 1;
    invalid.phases = {[](std::size_t, const auto&, const auto&) -> std::size_t {
        throw std::runtime_error("phase failed");
    }};
    bool rejected = false;
    try { (void)pool.apply(invalid); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected && pool.now() == 0 && pool.population().records()[0].value == 1 &&
            pool.broker().allocated_to(first) == 1,
            "throwing ABM phase must roll back the whole workforce transaction");

    invalid.phases = {[](std::size_t, const auto&, const auto&) { return std::size_t{0}; }};
    rejected = false;
    try { (void)pool.apply(invalid); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && pool.now() == 0 && pool.population().records()[0].value == 1,
            "ABM phase cannot reduce an assigned agent below its allocation");

    TransactionalAgentPool<std::size_t>::Change committed;
    committed.time = 1;
    committed.releases = {Release{1}};
    committed.run_registered_phases = true;
    committed.phases = {
        [](std::size_t index, const auto& snapshot, const auto& broker) {
            return broker.allocated_to(snapshot[index].id) == 0
                ? snapshot[index].value - 1 : snapshot[index].value;
        },
        [](std::size_t index, const auto& snapshot, const auto&) {
            return snapshot[index].value + 2;
        }
    };
    (void)pool.apply(committed);
    require(pool.now() == 1 && pool.population().records()[first].value == 3 &&
            pool.population().records()[second].value == 3 && pool.pool().capacity() == 6 &&
            pool.broker().allocated_units() == 0,
            "registered and broker-aware phases must compose from staged snapshots");
}

void test_failed_phase_injection_retries_through_simulator() {
    using Atomic = TransactionalAgentPoolAtomic<std::size_t>;
    using Message = ankurafathom::hybrid::AgentPoolMessage<std::size_t>;
    bool fail = true;
    SyncPopulation<std::size_t> population;
    (void)population.spawn(1);
    ankurafathom::devs::Simulator<Message> simulator;
    const auto id = simulator.add(std::make_unique<Atomic>(
        std::move(population), [](std::size_t value) { return value; }, 1));
    Atomic::Change phase;
    phase.time = 1;
    phase.phases = {[&fail](std::size_t index, const auto& snapshot, const auto&) {
        if (fail) throw std::runtime_error("injected phase failure");
        return snapshot[index].value + 1;
    }};
    simulator.inject(1, id, Atomic::input_port, phase);
    bool rejected = false;
    try { (void)simulator.step_transactional(); }
    catch (const std::runtime_error&) { rejected = true; }
    const auto& rolled_back = dynamic_cast<const Atomic&>(simulator.model(id));
    require(rejected && simulator.now() == 0 && simulator.next_time() == 1 &&
            rolled_back.core().now() == 0 && rolled_back.core().pool().capacity() == 1 &&
            rolled_back.core().population().records()[0].value == 1,
            "failed ABM command must roll back both the atomic and simulator injection");
    fail = false;
    const auto admitted = simulator.step_transactional();
    const auto emitted = simulator.step_transactional();
    const auto& committed = dynamic_cast<const Atomic&>(simulator.model(id));
    require(admitted && admitted->time == 1 && admitted->emissions.empty() &&
            emitted && emitted->time == 1 && emitted->emissions.size() == 1 &&
            committed.core().now() == 1 && committed.core().pool().capacity() == 2 &&
            committed.core().population().records()[0].value == 2,
            "corrected ABM command must retry at the original time and emit once");
}

} // namespace

int main() {
    try {
        test_release_departure_and_queue_commit_together();
        test_capacity_update_and_callback_failure_roll_back();
        test_devs_atomic_exposes_only_committed_results();
        test_transactional_abm_phases_read_staged_ledger();
        test_failed_phase_injection_retries_through_simulator();
        std::cout << "transactional agent pool tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
