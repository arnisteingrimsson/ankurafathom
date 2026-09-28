#include "ankurafathom/des/process.hpp"
#include "ankurafathom/des/multi_server.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_mm1_closed_form() {
    constexpr double arrival_rate = 0.5;
    constexpr double service_rate = 1.0;
    constexpr double horizon = 2000;
    constexpr std::uint32_t replications = 16;
    constexpr double rho = arrival_rate / service_rate;
    constexpr double expected_queue = rho * rho / (1 - rho);
    constexpr double expected_wait = expected_queue / arrival_rate;

    double total_queue = 0;
    double total_busy = 0;
    double total_wait = 0;
    std::uint64_t arrivals = 0;
    for (std::uint32_t replication = 0; replication < replications; ++replication) {
        const auto schedule = ankurafathom::des::exponential_schedule(4000, arrival_rate,
            service_rate, 0, 99173,
            ankurafathom::rng::DrawAddress{0, replication, 0, 0, 10, 0});
        ankurafathom::devs::Simulator<ankurafathom::des::Entity> simulator;
        auto source = std::make_unique<ankurafathom::des::ScheduledSource<>>(schedule);
        auto* source_ptr = source.get();
        const auto source_id = simulator.add(std::move(source));
        auto station = std::make_unique<ankurafathom::des::MultiServer<>>(1);
        auto* station_ptr = station.get();
        const auto station_id = simulator.add(std::move(station));
        simulator.connect(source_id, ankurafathom::des::ScheduledSource<>::output_port,
                          station_id, ankurafathom::des::MultiServer<>::input_port);
        (void)simulator.run_until(horizon);
        require(station_ptr->accepted_count() == source_ptr->emitted_count() &&
                station_ptr->accepted_count() >= station_ptr->completed_count() &&
                station_ptr->accepted_count() == station_ptr->completed_count() +
                    station_ptr->waiting() + station_ptr->busy_servers(),
                "M/M/1 entity conservation failed");
        total_queue += station_ptr->mean_queue_length(horizon);
        total_busy += station_ptr->utilization(horizon);
        total_wait += station_ptr->total_waiting_time();
        arrivals += station_ptr->accepted_count();
    }
    const double mean_queue = total_queue / replications;
    const double mean_busy = total_busy / replications;
    const double mean_wait = total_wait / static_cast<double>(arrivals);
    // About 16,000 arrivals spread across independent 2,000-time-unit runs at rho=0.5.
    // The absolute gates are deliberately several standard errors wide because queue
    // observations are autocorrelated and each run starts empty. Exact deterministic
    // schedules and conservation are tested separately.
    require(std::abs(mean_busy - rho) < 0.08,
            "M/M/1 utilization differs from the closed-form value");
    require(std::abs(mean_queue - expected_queue) < 0.20,
            "M/M/1 mean queue length differs from the closed-form value");
    require(std::abs(mean_wait - expected_wait) < 0.30,
            "M/M/1 mean wait differs from the closed-form value");
    std::cout << "M/M/1 oracle: arrivals=" << arrivals << " utilization=" << mean_busy
              << " queue=" << mean_queue << " wait=" << mean_wait << '\n';
}

void test_mm2_erlang_c() {
    constexpr double arrival_rate = 1.2;
    constexpr double service_rate = 1.0;
    constexpr double horizon = 1000;
    constexpr std::uint32_t replications = 8;
    constexpr double servers = 2;
    constexpr double utilization = arrival_rate / (servers * service_rate);
    constexpr double offered_load = arrival_rate / service_rate;
    constexpr double empty_probability = 1.0 /
        (1.0 + offered_load + offered_load * offered_load /
            (2.0 * (1.0 - utilization)));
    constexpr double wait_probability = offered_load * offered_load * empty_probability /
        (2.0 * (1.0 - utilization));
    constexpr double expected_wait = wait_probability / (servers * service_rate - arrival_rate);
    constexpr double expected_queue = arrival_rate * expected_wait;

    double total_queue = 0;
    double total_utilization = 0;
    double total_wait = 0;
    std::uint64_t arrivals = 0;
    for (std::uint32_t replication = 0; replication < replications; ++replication) {
        const auto schedule = ankurafathom::des::exponential_schedule(3000, arrival_rate,
            service_rate, 0, 88371,
            ankurafathom::rng::DrawAddress{1, replication, 0, 0, 20, 0});
        ankurafathom::devs::Simulator<ankurafathom::des::Entity> simulator;
        auto source = std::make_unique<ankurafathom::des::ScheduledSource<>>(schedule);
        auto* source_ptr = source.get();
        const auto source_id = simulator.add(std::move(source));
        auto station = std::make_unique<ankurafathom::des::MultiServer<>>(2);
        auto* station_ptr = station.get();
        const auto station_id = simulator.add(std::move(station));
        simulator.connect(source_id, ankurafathom::des::ScheduledSource<>::output_port,
                          station_id, ankurafathom::des::MultiServer<>::input_port);
        (void)simulator.run_until(horizon);
        require(station_ptr->accepted_count() == source_ptr->emitted_count() &&
                station_ptr->accepted_count() == station_ptr->completed_count() +
                    station_ptr->waiting() + station_ptr->busy_servers(),
                "M/M/2 entity conservation failed");
        total_queue += station_ptr->mean_queue_length(horizon);
        total_utilization += station_ptr->utilization(horizon);
        total_wait += station_ptr->total_waiting_time();
        arrivals += station_ptr->accepted_count();
    }
    const double mean_queue = total_queue / replications;
    const double mean_utilization = total_utilization / replications;
    const double mean_wait = total_wait / static_cast<double>(arrivals);
    // Roughly 9,600 arrivals in independent runs; gates allow startup bias and
    // autocorrelation while still detecting a wrong server count or queue rule.
    require(std::abs(mean_utilization - utilization) < 0.10,
            "M/M/2 utilization differs from Erlang C");
    require(std::abs(mean_queue - expected_queue) < 0.25,
            "M/M/2 queue length differs from Erlang C");
    require(std::abs(mean_wait - expected_wait) < 0.20,
            "M/M/2 wait differs from Erlang C");
    std::cout << "M/M/2 oracle: arrivals=" << arrivals << " utilization="
              << mean_utilization << " queue=" << mean_queue << " wait=" << mean_wait << '\n';
}

} // namespace

int main() {
    try {
        test_mm1_closed_form();
        test_mm2_erlang_c();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
