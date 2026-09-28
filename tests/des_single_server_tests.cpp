#include "ankurafathom/des/single_server.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using ankurafathom::des::Entity;
using ankurafathom::devs::Atomic;
using ankurafathom::devs::Input;
using ankurafathom::devs::PortValue;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Source final : public Atomic<Entity> {
public:
    double time_advance() const override {
        return sent_ < 3 ? 1 : std::numeric_limits<double>::infinity();
    }
    std::vector<PortValue<Entity>> output() const override {
        const double time = sent_ + 1;
        return {{0, Entity{static_cast<std::uint64_t>(sent_ + 1), time, 2}}};
    }
    void internal_transition() override { ++sent_; }
    void external_transition(double, const std::vector<Input<Entity>>&) override {
        throw std::logic_error("unexpected source input");
    }
    void confluent_transition(const std::vector<Input<Entity>>&) override {
        throw std::logic_error("unexpected source confluence");
    }
private:
    int sent_ = 0;
};

void test_fifo_confluence_and_exact_statistics() {
    ankurafathom::devs::Simulator<Entity> simulator;
    const auto source = simulator.add(std::make_unique<Source>());
    auto server = std::make_unique<ankurafathom::des::SingleServer<Entity>>();
    auto* station = server.get();
    const auto station_id = simulator.add(std::move(server));
    simulator.connect(source, 0, station_id, ankurafathom::des::SingleServer<Entity>::input_port);
    const auto trace = simulator.run_until(7);
    std::vector<std::uint64_t> completed_ids;
    std::vector<double> completion_times;
    for (const auto& step : trace) {
        for (const auto& emission : step.emissions) {
            if (emission.source == station_id) {
                completed_ids.push_back(emission.value.id);
                completion_times.push_back(emission.value.completed_at);
                require(emission.value.completed_at == step.time, "completion timestamp is wrong");
            }
        }
    }
    require(completed_ids == std::vector<std::uint64_t>({1, 2, 3}),
            "FIFO order changed at simultaneous arrival and completion");
    require(completion_times == std::vector<double>({3, 5, 7}),
            "service completion times are wrong");
    require(station->accepted_count() == 3 && station->completed_count() == 3,
            "entity accounting is wrong");
    require(std::abs(station->total_waiting_time() - 3) < 1e-12,
            "waiting time should be 0 + 1 + 2");
    require(std::abs(station->mean_queue_length(7) - 3.0 / 7) < 1e-12,
            "time-weighted queue length is wrong");
    require(std::abs(station->utilization(7) - 6.0 / 7) < 1e-12,
            "time-weighted utilization is wrong");
}

void test_invalid_bag_rolls_back_transitions() {
    ankurafathom::des::SingleServer<> station;
    station.external_transition(0, {{0, 0, Entity{1, 0, 2}}});
    const std::vector<Input<Entity>> invalid{
        {0, 0, Entity{2, 0.5, 1}}, {0, 0, Entity{2, 0.5, 1}}
    };
    bool caught = false;
    try { station.external_transition(0.5, invalid); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught && station.time_advance() == 2 && station.accepted_count() == 1 &&
            station.waiting() == 0 && station.utilization(1) == 1,
            "rejected external bag changed single-server state");
    caught = false;
    try { station.confluent_transition(invalid); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught && station.time_advance() == 2 && station.completed_count() == 0 &&
            station.output().at(0).value.completed_at == 2,
            "rejected confluent bag changed single-server state");
    station.internal_transition();
    require(station.completed_count() == 1 && !station.busy(),
            "single server did not recover after rejected bag");
}

void test_arrival_just_before_completion_does_not_complete_early() {
    ankurafathom::des::SingleServer<> station;
    station.external_transition(0, {{0, 0, Entity{1, 0, 2}}});
    const double before_due = std::nextafter(2.0, 0.0);
    station.external_transition(before_due, {{0, 0, Entity{2, before_due, 1}}});
    require(station.time_advance() > 0 && station.completed_count() == 0 &&
            station.waiting() == 1 && station.output().at(0).value.completed_at == 2,
            "nearby external event completed service before its due time");
    station.internal_transition();
    require(station.completed_count() == 1 && station.busy() &&
            station.output().at(0).value.id == 2,
            "waiting entity did not start after exact completion");
}

} // namespace

int main() {
    try {
        test_fifo_confluence_and_exact_statistics();
        test_invalid_bag_rolls_back_transitions();
        test_arrival_just_before_completion_does_not_complete_early();
        std::cout << "DES single-server tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
