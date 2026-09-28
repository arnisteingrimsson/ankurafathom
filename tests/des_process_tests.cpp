#include "ankurafathom/des/process.hpp"
#include "ankurafathom/des/multi_server.hpp"

#include <iostream>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using ankurafathom::des::CompletionSink;
using ankurafathom::des::Entity;
using ankurafathom::des::MultiServer;
using ankurafathom::des::ScheduledSource;
using ankurafathom::des::exponential_schedule;
using ankurafathom::devs::Simulator;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_source_server_sink_queue() {
    Simulator<Entity> simulator;
    auto source = std::make_unique<ScheduledSource<>>(std::vector<Entity>{
        {1, 0, 2}, {2, 0, 1}, {3, 0, 1}, {4, 1, 1}
    });
    auto* source_ptr = source.get();
    const auto source_id = simulator.add(std::move(source));
    auto server = std::make_unique<MultiServer<>>(2);
    auto* server_ptr = server.get();
    const auto server_id = simulator.add(std::move(server));
    auto sink = std::make_unique<CompletionSink<>>();
    auto* sink_ptr = sink.get();
    const auto sink_id = simulator.add(std::move(sink));
    simulator.connect(source_id, ScheduledSource<>::output_port,
                      server_id, MultiServer<>::input_port);
    simulator.connect(server_id, MultiServer<>::output_port,
                      sink_id, CompletionSink<>::input_port);
    const auto trace = simulator.run_until(3);
    require(trace.size() == 4 && trace[0].time == 0 && trace[1].time == 1 &&
            trace[2].time == 2 && trace[3].time == 3,
            "source-server-sink event times are wrong");
    require(trace[0].emissions.size() == 3 && trace[1].emissions.size() == 2,
            "same-time source bag or server completion is wrong");
    require(source_ptr->emitted_count() == 4 && server_ptr->completed_count() == 4 &&
            sink_ptr->completed_count() == 4,
            "entities were lost in the process path");
    const auto& entities = sink_ptr->completed();
    require(entities[0].id == 2 && entities[1].id == 1 &&
            entities[2].id == 3 && entities[3].id == 4,
            "completion order differs from the exact FIFO schedule");
    require(sink_ptr->total_cycle_time() == 7 && sink_ptr->mean_cycle_time() == 1.75,
            "cycle-time statistics are wrong");
    require(server_ptr->total_waiting_time() == 2 &&
            std::abs(server_ptr->mean_queue_length(3) - 2.0 / 3.0) < 1e-12 &&
            std::abs(server_ptr->utilization(3) - 5.0 / 6.0) < 1e-12,
            "process path violates its exact queue and busy-time schedule");
}

void test_transactional_process_trace() {
    Simulator<Entity> simulator;
    const auto source = simulator.add(std::make_unique<ScheduledSource<>>(
        std::vector<Entity>{{1, 0, 2}, {2, 0, 1}, {3, 1, 1}}));
    const auto server = simulator.add(std::make_unique<MultiServer<>>(1));
    const auto sink = simulator.add(std::make_unique<CompletionSink<>>());
    simulator.connect(source, ScheduledSource<>::output_port,
                      server, MultiServer<>::input_port);
    simulator.connect(server, MultiServer<>::output_port,
                      sink, CompletionSink<>::input_port);
    const auto trace = simulator.run_until_transactional(4);
    require(trace.size() == 5 && trace[0].time == 0 && trace[1].time == 1 &&
            trace[2].time == 2 && trace[3].time == 3 && trace[4].time == 4 &&
            dynamic_cast<const CompletionSink<>&>(simulator.model(sink)).completed_count() == 3,
            "cloneable built-in process must run through transactional simulator steps");
}

void test_invalid_source_and_sink() {
    bool caught = false;
    try { (void)ScheduledSource<>(std::vector<Entity>{{1, 1, 1}, {2, 0, 1}}); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "source arrival schedule must be sorted");
    caught = false;
    try { (void)ScheduledSource<>(std::vector<Entity>{{1, 0, 1}, {1, 1, 1}}); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "source entity IDs must be unique");
    CompletionSink<> sink;
    const std::vector<ankurafathom::devs::Input<Entity>> bad{
        {0, 0, Entity{1, 0, 1, 2}}
    };
    caught = false;
    try { sink.external_transition(1, bad); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught && sink.completed_count() == 0,
            "sink must reject a completion timestamp different from event time");
}

void test_addressed_exponential_schedule() {
    const ankurafathom::rng::DrawAddress base{2, 3, 100, 0, 12, 0};
    const auto schedule = exponential_schedule(32, 1.5, 2.5, 4, 987, base);
    const auto replay = exponential_schedule(32, 1.5, 2.5, 4, 987, base);
    for (std::size_t i = 0; i < schedule.size(); ++i)
        require(schedule[i].id == replay[i].id &&
                schedule[i].arrived_at == replay[i].arrived_at &&
                schedule[i].service_duration == replay[i].service_duration,
                "same draw address did not reproduce the same process schedule");
    require(schedule.front().id == 100 && schedule.back().id == 131 &&
            schedule.front().arrived_at > 4,
            "schedule entity range or first arrival is wrong");
    for (std::size_t i = 1; i < schedule.size(); ++i)
        require(schedule[i].arrived_at > schedule[i - 1].arrived_at &&
                schedule[i].service_duration > 0,
                "exponential schedule arrivals must strictly increase");
    const auto changed = exponential_schedule(32, 1.5, 2.5, 4, 988, base);
    require(changed.front().arrived_at != schedule.front().arrived_at,
            "changing seed did not change schedule");

    Simulator<Entity> simulator;
    const auto source_id = simulator.add(std::make_unique<ScheduledSource<>>(schedule));
    auto server = std::make_unique<MultiServer<>>(2);
    auto* server_ptr = server.get();
    const auto server_id = simulator.add(std::move(server));
    auto sink = std::make_unique<CompletionSink<>>();
    auto* sink_ptr = sink.get();
    const auto sink_id = simulator.add(std::move(sink));
    simulator.connect(source_id, ScheduledSource<>::output_port,
                      server_id, MultiServer<>::input_port);
    simulator.connect(server_id, MultiServer<>::output_port,
                      sink_id, CompletionSink<>::input_port);
    (void)simulator.run_until(1000);
    require(server_ptr->accepted_count() == 32 && server_ptr->completed_count() == 32 &&
            sink_ptr->completed_count() == 32 && server_ptr->waiting() == 0,
            "stochastic process lost or duplicated entities");

    bool caught = false;
    try { (void)exponential_schedule(1, 1, 1, 0, 1, {0, 0, 0, 0, 65535, 0}); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "schedule must reserve two valid independent streams");
}

void test_two_stage_tandem_queue() {
    Simulator<Entity> simulator;
    const auto source_id = simulator.add(std::make_unique<ScheduledSource<>>(
        std::vector<Entity>{{1, 0, 1}, {2, 0, 1}}));
    auto first = std::make_unique<MultiServer<>>(1);
    auto* first_ptr = first.get();
    const auto first_id = simulator.add(std::move(first));
    auto second = std::make_unique<MultiServer<>>(1, 2);
    auto* second_ptr = second.get();
    const auto second_id = simulator.add(std::move(second));
    auto sink = std::make_unique<CompletionSink<>>();
    auto* sink_ptr = sink.get();
    const auto sink_id = simulator.add(std::move(sink));
    simulator.connect(source_id, ScheduledSource<>::output_port,
                      first_id, MultiServer<>::input_port);
    simulator.connect(first_id, MultiServer<>::output_port,
                      second_id, MultiServer<>::input_port);
    simulator.connect(second_id, MultiServer<>::output_port,
                      sink_id, CompletionSink<>::input_port);
    const auto trace = simulator.run_until(5);
    require(trace.size() == 5 && trace[0].time == 0 && trace[1].time == 1 &&
            trace[2].time == 2 && trace[3].time == 3 && trace[4].time == 5,
            "two-stage event schedule is wrong");
    require(first_ptr->accepted_count() == 2 && first_ptr->completed_count() == 2 &&
            second_ptr->accepted_count() == 2 && second_ptr->completed_count() == 2 &&
            sink_ptr->completed_count() == 2 &&
            sink_ptr->completed()[0].id == 1 && sink_ptr->completed()[1].id == 2,
            "two-stage process lost or reordered entities");
    require(first_ptr->total_waiting_time() == 1 && second_ptr->total_waiting_time() == 1 &&
            std::abs(first_ptr->mean_queue_length(5) - 0.2) < 1e-12 &&
            std::abs(second_ptr->mean_queue_length(5) - 0.2) < 1e-12 &&
            std::abs(second_ptr->utilization(5) - 0.8) < 1e-12,
            "stage-local queue area or utilization differs from the exact schedule");
    require(sink_ptr->completed()[0].arrived_at == 0 &&
            sink_ptr->completed()[0].completed_at == 3 &&
            sink_ptr->completed()[1].completed_at == 5 &&
            sink_ptr->total_cycle_time() == 8 && sink_ptr->mean_cycle_time() == 4,
            "end-to-end cycle times are wrong across two stages");
}

void test_tandem_handoff_at_downstream_completion() {
    Simulator<Entity> simulator;
    const auto source_id = simulator.add(std::make_unique<ScheduledSource<>>(
        std::vector<Entity>{{1, 0, 1}, {2, 1, 1}, {3, 2, 1}}));
    const auto first_id = simulator.add(std::make_unique<MultiServer<>>(1));
    auto second = std::make_unique<MultiServer<>>(1, 2);
    auto* second_ptr = second.get();
    const auto second_id = simulator.add(std::move(second));
    auto sink = std::make_unique<CompletionSink<>>();
    auto* sink_ptr = sink.get();
    const auto sink_id = simulator.add(std::move(sink));
    simulator.connect(source_id, ScheduledSource<>::output_port,
                      first_id, MultiServer<>::input_port);
    simulator.connect(first_id, MultiServer<>::output_port,
                      second_id, MultiServer<>::input_port);
    simulator.connect(second_id, MultiServer<>::output_port,
                      sink_id, CompletionSink<>::input_port);
    (void)simulator.run_until(3);
    require(second_ptr->accepted_count() == 3 && second_ptr->completed_count() == 1 &&
            second_ptr->waiting() == 1 && second_ptr->output().at(0).value.id == 2 &&
            sink_ptr->completed_count() == 1 && sink_ptr->completed()[0].id == 1,
            "confluent handoff overtook the downstream waiting entity");
    (void)simulator.run_until(7);
    require(sink_ptr->completed_count() == 3 && sink_ptr->completed()[1].id == 2 &&
            sink_ptr->completed()[2].id == 3 && sink_ptr->total_cycle_time() == 12 &&
            second_ptr->total_waiting_time() == 3 &&
            std::abs(second_ptr->mean_queue_length(7) - 3.0 / 7.0) < 1e-12 &&
            std::abs(second_ptr->utilization(7) - 6.0 / 7.0) < 1e-12,
            "confluent tandem schedule or stage statistics are wrong");
}

} // namespace

int main() {
    try {
        test_source_server_sink_queue();
        test_transactional_process_trace();
        test_invalid_source_and_sink();
        test_addressed_exponential_schedule();
        test_two_stage_tandem_queue();
        test_tandem_handoff_at_downstream_completion();
        std::cout << "DES process path tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
