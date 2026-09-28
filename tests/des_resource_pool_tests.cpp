#include "ankurafathom/des/resource_pool.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>

namespace {

using ankurafathom::des::Grant;
using ankurafathom::des::Release;
using ankurafathom::des::ResourceMessage;
using ankurafathom::des::ResourcePool;
using ankurafathom::des::Seize;
using ankurafathom::des::SetCapacity;
using ankurafathom::devs::Simulator;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint64_t> grant_ids(const ankurafathom::devs::StepResult<ResourceMessage>& step) {
    std::vector<std::uint64_t> ids;
    for (const auto& emission : step.emissions)
        if (const auto* grant = std::get_if<Grant>(&emission.value)) ids.push_back(grant->request_id);
    return ids;
}

void test_fifo_release_and_microsteps() {
    Simulator<ResourceMessage> simulator;
    auto pool = std::make_unique<ResourcePool>(2);
    auto* observed = pool.get();
    const auto id = simulator.add(std::move(pool));
    simulator.inject(0, id, 0, Seize{1, 1});
    simulator.inject(0, id, 0, Seize{2, 2});
    simulator.inject(0, id, 0, Seize{3, 1});
    simulator.inject(1, id, 0, Release{1});
    simulator.inject(1, id, 0, Seize{4, 1});
    simulator.inject(2, id, 0, Release{2});
    const auto trace = simulator.run_until(2);
    require(trace.size() == 6 && trace[0].time == 0 && trace[1].time == 0 &&
            trace[2].time == 1 && trace[3].time == 1 &&
            trace[4].time == 2 && trace[5].time == 2,
            "resource grants must use zero-time microsteps");
    require(grant_ids(trace[1]) == std::vector<std::uint64_t>({1}) &&
            grant_ids(trace[3]) == std::vector<std::uint64_t>({2}) &&
            grant_ids(trace[5]) == std::vector<std::uint64_t>({3, 4}),
            "FIFO or head-of-line grant order is wrong");
    require(observed->available() == 0 && observed->waiting() == 0 && observed->allocated() == 2 &&
            observed->available() + observed->allocated_units() == observed->capacity(),
            "resource capacity accounting is wrong");
}

void test_confluent_grant_and_atomic_validation() {
    Simulator<ResourceMessage> simulator;
    auto pool = std::make_unique<ResourcePool>(2);
    auto* observed = pool.get();
    const auto id = simulator.add(std::move(pool));
    simulator.inject(0, id, 0, Seize{1, 1});
    const auto first = simulator.step();
    require(first && first->time == 0 && first->emissions.empty(),
            "seize admission must precede grant output");
    simulator.inject(0, id, 0, Seize{2, 1});
    const auto confluent = simulator.step();
    require(confluent && grant_ids(*confluent) == std::vector<std::uint64_t>({1}) &&
            confluent->injections.size() == 1,
            "pending grant must emit before same-time request is applied");
    const auto next = simulator.step();
    require(next && grant_ids(*next) == std::vector<std::uint64_t>({2}) &&
            observed->available() == 0,
            "new same-time grant must emit in a later microstep");

    ResourcePool invalid(2);
    const std::vector<ankurafathom::devs::Input<ResourceMessage>> duplicates{
        {0, 0, Seize{7, 1}}, {0, 0, Seize{7, 1}}
    };
    bool caught = false;
    try { invalid.external_transition(0, duplicates); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught && invalid.available() == 2 && invalid.allocated() == 0 &&
            invalid.waiting() == 0,
            "duplicate request bag must fail without partial admission");
}

void test_capacity_updates_preserve_allocations_and_fifo() {
    ResourcePool pool(1);
    const auto request = [](std::uint64_t id, std::size_t units) {
        return ankurafathom::devs::Input<ResourceMessage>{0, ResourcePool::input_port,
                                                           Seize{id, units}};
    };
    const auto release = [](std::uint64_t id) {
        return ankurafathom::devs::Input<ResourceMessage>{0, ResourcePool::input_port,
                                                           Release{id}};
    };
    const auto resize = [](std::size_t units) {
        return ankurafathom::devs::Input<ResourceMessage>{0, ResourcePool::capacity_port,
                                                           SetCapacity{units}};
    };

    pool.external_transition(0, {request(1, 1), request(2, 1)});
    require(pool.allocated() == 1 && pool.waiting() == 1, "initial resource queue is wrong");
    pool.internal_transition();
    pool.external_transition(1, {resize(2)});
    require(pool.capacity() == 2 && pool.allocated() == 2 && pool.waiting() == 0 &&
            std::get<Grant>(pool.output()[0].value).request_id == 2,
            "capacity expansion did not grant oldest waiter");
    pool.internal_transition();

    bool rejected = false;
    try { pool.external_transition(0, {resize(1)}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && pool.capacity() == 2 && pool.allocated_units() == 2,
            "capacity shrink must not revoke active allocations");

    pool.external_transition(0, {resize(1), release(1)});
    require(pool.capacity() == 1 && pool.allocated_units() == 1 && pool.available() == 0,
            "same-bag release must precede capacity shrink");
    pool.external_transition(0, {release(2), resize(0)});
    require(pool.capacity() == 0 && pool.allocated() == 0 && pool.available() == 0,
            "empty workforce should permit zero capacity");

    rejected = false;
    try { pool.external_transition(0, {resize(1), resize(2)}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && pool.capacity() == 0,
            "multiple capacity updates must reject the entire bag");
    rejected = false;
    try { pool.external_transition(0, {resize(1), request(3, 2)}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && pool.capacity() == 0 && pool.waiting() == 0,
            "oversized request must roll back simultaneous capacity update");

    pool.external_transition(0, {request(3, 1)});
    require(pool.waiting() == 1 && pool.allocated() == 0,
            "valid requests must queue while capacity is zero");
    pool.external_transition(0, {resize(1)});
    require(pool.waiting() == 0 && pool.allocated_units() == 1 &&
            std::get<Grant>(pool.output()[0].value).request_id == 3,
            "capacity recovery must grant a request queued at zero capacity");

    ResourcePool empty(0, 2);
    empty.external_transition(0, {request(8, 2)});
    require(empty.waiting() == 1 && empty.capacity() == 0,
            "explicit request limit must admit multi-unit demand at zero capacity");
    empty.external_transition(0, {resize(2)});
    require(empty.allocated_units() == 2 && empty.waiting() == 0,
            "multi-unit demand must grant when capacity becomes available");
}

} // namespace

int main() {
    try {
        test_fifo_release_and_microsteps();
        test_confluent_grant_and_atomic_validation();
        test_capacity_updates_preserve_allocations_and_fifo();
        std::cout << "DES resource pool tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
