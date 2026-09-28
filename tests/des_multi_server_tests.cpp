#include "ankurafathom/des/multi_server.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using ankurafathom::des::Entity;
using ankurafathom::des::MultiServer;
using ankurafathom::des::SingleServer;
using ankurafathom::devs::Simulator;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_two_servers_fifo_confluence_and_statistics() {
    Simulator<Entity> simulator;
    auto station = std::make_unique<MultiServer<>>(2);
    auto* observed = station.get();
    const auto id = simulator.add(std::move(station));
    simulator.inject(0, id, 0, Entity{1, 0, 2});
    simulator.inject(0, id, 0, Entity{2, 0, 1});
    simulator.inject(0, id, 0, Entity{3, 0, 1});
    simulator.inject(1, id, 0, Entity{4, 1, 1});
    const auto trace = simulator.run_until(3);
    require(trace.size() == 4 && trace[0].time == 0 && trace[1].time == 1 &&
            trace[2].time == 2 && trace[3].time == 3,
            "multi-server event times are wrong");
    require(trace[1].emissions.size() == 1 && trace[1].emissions[0].value.id == 2 &&
            trace[1].emissions[0].value.completed_at == 1,
            "first completion or confluent time is wrong");
    require(trace[2].emissions.size() == 2 && trace[2].emissions[0].value.id == 1 &&
            trace[2].emissions[1].value.id == 3,
            "simultaneous completions must emit in slot order");
    require(trace[3].emissions.size() == 1 && trace[3].emissions[0].value.id == 4,
            "new arrival overtook existing waiting entity");
    require(observed->accepted_count() == 4 && observed->completed_count() == 4 &&
            observed->waiting() == 0 && observed->busy_servers() == 0,
            "multi-server accounting counts are wrong");
    require(observed->total_waiting_time() == 2 &&
            std::abs(observed->mean_queue_length(3) - 2.0 / 3.0) < 1e-12 &&
            std::abs(observed->utilization(3) - 5.0 / 6.0) < 1e-12,
            "queue area, waiting time, or utilization is wrong");
}

void test_one_server_matches_reference() {
    Simulator<Entity> multi_sim;
    Simulator<Entity> single_sim;
    auto multi = std::make_unique<MultiServer<>>(1);
    auto single = std::make_unique<SingleServer<>>();
    auto* multi_ptr = multi.get();
    auto* single_ptr = single.get();
    const auto multi_id = multi_sim.add(std::move(multi));
    const auto single_id = single_sim.add(std::move(single));
    for (const Entity& entity : {Entity{1, 0, 1}, Entity{2, 0, 2}, Entity{3, 1, 1}}) {
        multi_sim.inject(entity.arrived_at, multi_id, 0, entity);
        single_sim.inject(entity.arrived_at, single_id, 0, entity);
    }
    const auto multi_trace = multi_sim.run_until(4);
    const auto single_trace = single_sim.run_until(4);
    require(multi_trace.size() == single_trace.size(), "one-server event count differs");
    for (std::size_t i = 0; i < multi_trace.size(); ++i) {
        require(multi_trace[i].time == single_trace[i].time &&
                multi_trace[i].emissions.size() == single_trace[i].emissions.size(),
                "one-server event trace differs");
        for (std::size_t j = 0; j < multi_trace[i].emissions.size(); ++j)
            require(multi_trace[i].emissions[j].value.id == single_trace[i].emissions[j].value.id,
                    "one-server completion order differs");
    }
    require(multi_ptr->total_waiting_time() == single_ptr->total_waiting_time() &&
            multi_ptr->mean_queue_length(4) == single_ptr->mean_queue_length(4) &&
            multi_ptr->utilization(4) == single_ptr->utilization(4),
            "one-server statistics differ from the reference primitive");
}

void test_invalid_capacity() {
    bool caught = false;
    try { (void)MultiServer<>(0); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "zero server capacity must be rejected");
    caught = false;
    try { (void)MultiServer<>(1, 0); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "nonpositive service scale must be rejected");

    MultiServer<> station(2);
    const std::vector<ankurafathom::devs::Input<Entity>> duplicate{
        {0, 0, Entity{9, 0, 1}}, {0, 0, Entity{9, 0, 1}}
    };
    caught = false;
    try { station.external_transition(0, duplicate); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught && station.accepted_count() == 0 && station.waiting() == 0 &&
            station.busy_servers() == 0,
            "duplicate arrival bag must be rejected without partial admission");
}

void test_invalid_bag_rolls_back_transitions() {
    MultiServer<> station(2);
    station.external_transition(0, {{0, 0, Entity{1, 0, 2}},
                                    {0, 0, Entity{2, 0, 3}}});
    const std::vector<ankurafathom::devs::Input<Entity>> invalid{
        {0, 0, Entity{3, 1, 1}}, {0, 0, Entity{3, 1, 1}}
    };
    bool caught = false;
    try { station.external_transition(1, invalid); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught && station.time_advance() == 2 && station.accepted_count() == 2 &&
            station.waiting() == 0 && station.utilization(1) == 1,
            "rejected external bag changed multi-server state");
    caught = false;
    try { station.confluent_transition(invalid); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught && station.time_advance() == 2 && station.completed_count() == 0 &&
            station.output().at(0).value.id == 1,
            "rejected confluent bag changed multi-server state");
    station.internal_transition();
    require(station.completed_count() == 1 && station.busy_servers() == 1,
            "multi server did not recover after rejected bag");
}

} // namespace

int main() {
    try {
        test_two_servers_fifo_confluence_and_statistics();
        test_one_server_matches_reference();
        test_invalid_capacity();
        test_invalid_bag_rolls_back_transitions();
        std::cout << "Multi-server DES tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
