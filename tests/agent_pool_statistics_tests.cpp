#include "ankurafathom/hybrid/agent_pool_process.hpp"
#include "ankurafathom/ir/model.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using Pool = ankurafathom::hybrid::TransactionalAgentPool<std::size_t>;
using Process = ankurafathom::hybrid::AgentPoolProcess<std::size_t>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void close(double actual, double expected, const char* message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <=
            1e-12 * std::max(1.0, std::abs(expected)), message);
}
Process make_process(std::initializer_list<std::size_t> capacities, double origin = 0) {
    ankurafathom::abm::SyncPopulation<std::size_t> population;
    for (auto capacity : capacities) (void)population.spawn(capacity);
    Pool pool(std::move(population), [](const auto& value) { return value; }, 2);
    Pool::Change change;
    change.time = origin;
    (void)pool.apply(change);
    return Process(std::move(pool));
}

void test_weighted_capacity_and_read_only_sampling() {
    auto process = make_process({1});
    Process::Change start;
    start.arrivals = {{1, 1, 2}};
    (void)process.apply(start);
    const auto initial = process.time_statistics(0);
    require(initial.utilization() == 0 && initial.mean_headcount() == 0,
            "zero-duration observation uses the documented zero convention");
    for (double horizon : {0.25, 0.75, 0.5, 0.25}) {
        const auto measured = process.time_statistics(horizon);
        close(measured.allocated_time, horizon, "sampling must not accumulate duplicate time");
        require(process.system().now() == 0, "sampling must not advance the event clock");
    }
    Process::Change hire;
    hire.workforce.time = 1;
    hire.workforce.hires = {3};
    (void)process.apply(hire);
    auto measured = process.time_statistics(2);
    require(measured.capacity_time == 5 && measured.allocated_time == 2 &&
            measured.available_time == 3 && measured.population_time == 3,
            "capacity and headcount areas must use the pre-event state on each interval");
    close(measured.utilization(), 2.0 / 5, "utilization must use capacity-time as denominator");
    bool rejected = false;
    try { (void)process.time_statistics(2.25); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "statistics must not project through an unprocessed completion");

    Process::Change finish;
    finish.workforce.time = 2;
    (void)process.apply(finish);
    measured = process.time_statistics(3);
    require(measured.capacity_time == 9 && measured.allocated_time == 2 &&
            measured.available_time == 7 && measured.service_time == 2 &&
            measured.population_time == 5 && measured.elapsed == 3,
            "post-completion idle time must remain in the observation window");
    close(measured.utilization(), 2.0 / 9, "averaging instantaneous utilization gives the wrong answer");
    close(measured.mean_headcount(), 5.0 / 3, "mean headcount is time weighted");
    require(process.system().now() == 2 && process.time_statistics(2).capacity_time == 5,
            "tail projection must leave committed statistics unchanged");
}

void test_zero_capacity_and_origin() {
    auto process = make_process({0});
    Process::Change arrival;
    arrival.arrivals = {{1, 1, 1}};
    (void)process.apply(arrival);
    const auto waiting = process.time_statistics(3);
    require(waiting.queue_time == 3 && waiting.capacity_time == 0 && waiting.utilization() == 0 &&
            waiting.population_time == 3 && waiting.mean_queue() == 1 && waiting.mean_headcount() == 1,
            "zero-capacity agents and unfinished queue time must still be counted");
    require(process.statistics().wait_total == 0 && process.statistics().cycle_total == 0,
            "cohort totals must remain separate from unfinished-work time integrals");
    auto empty = make_process({});
    require(empty.time_statistics(2).mean_headcount() == 0 && empty.time_statistics(2).utilization() == 0,
            "empty population must produce finite zero statistics");
    auto shifted = make_process({1}, 5);
    const auto measured = shifted.time_statistics(7);
    require(measured.elapsed == 2 && measured.capacity_time == 2 && measured.mean_headcount() == 1,
            "C++ statistics window starts when delivery takes ownership of the pool");
    for (double time : {4.0, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
        bool rejected = false;
        try { (void)shifted.time_statistics(time); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid statistics horizon must be rejected");
    }
}

void test_rollback_and_overflow() {
    auto process = make_process({2});
    Process::Change arrival;
    arrival.arrivals = {{1, 1, 2}};
    (void)process.apply(arrival);
    Process::Change failure;
    failure.workforce.time = 1;
    failure.workforce.phases = {[](std::size_t, const auto&, const auto&) -> std::size_t {
        throw std::runtime_error("phase failure");
    }};
    bool rejected = false;
    try { (void)process.apply(failure); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected && process.system().now() == 0 && process.time_statistics(0).capacity_time == 0,
            "failed workforce transaction must roll back its time integrals");
    failure.workforce.phases.clear();
    failure.workforce.updates = {{0, 3}};
    (void)process.apply(failure);
    const auto measured = process.time_statistics(2);
    require(measured.capacity_time == 5 && measured.allocated_time == 2 && measured.available_time == 3,
            "retry must not double-count the rolled-back interval");

    auto overflow = make_process({2});
    rejected = false;
    try { (void)overflow.time_statistics(std::numeric_limits<double>::max()); }
    catch (const std::overflow_error&) { rejected = true; }
    require(rejected && overflow.system().now() == 0, "projection overflow must not mutate state");
    Process::Change advance;
    advance.workforce.time = std::numeric_limits<double>::max();
    rejected = false;
    try { (void)overflow.apply(advance); }
    catch (const std::overflow_error&) { rejected = true; }
    require(rejected && overflow.system().now() == 0 && overflow.time_statistics(0).capacity_time == 0,
            "time-integral overflow must reject the whole transaction");
    advance.workforce.time = 1;
    (void)overflow.apply(advance);
    require(overflow.time_statistics(1).capacity_time == 2, "retry after integral overflow must succeed");
}

void test_ir_oracle(const ankurafathom::ir::Model& model) {
    const auto rows = ankurafathom::ir::run(model);
    // Capacity, occupied, free, queue, service, population areas; utilization;
    // mean queue, service, headcount; started waiting and completed cycle totals.
    const std::array<std::array<double, 12>, 9> expected{{
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {{1, 1, 0, 1, 1, 1, 1, 2, 2, 2, 0, 0}},
        {{2, 2, 0, 2, 2, 2, 1, 2, 2, 2, 0, 1}},
        {{2.5, 2.5, 0, 3, 2.5, 2.5, 1, 2, 5.0/3, 5.0/3, 1.5, 1}},
        {{3.5, 3.5, 0, 3.5, 3.5, 3.5, 1, 1.75, 1.75, 1.75, 3.5, 5}},
        {{4.5, 4.5, 0, 4, 4, 4.5, 1, 1.6, 1.6, 1.8, 3.5, 5}},
        {{5.5, 5.5, 0, 4.5, 4.5, 5.5, 1, 1.5, 1.5, 11.0/6, 4.5, 8}},
        {{6, 5.75, 0.25, 4.5, 4.75, 6, 23.0/24, 9.0/7, 19.0/14, 12.0/7, 4.5, 9.25}},
        {{6.5, 5.75, 0.75, 4.5, 4.75, 6.5, 23.0/26, 9.0/8, 19.0/16, 13.0/8, 4.5, 9.25}}
    }};
    require(rows.size() == 108, "wrong statistics observation count");
    for (std::size_t step = 0; step < expected.size(); ++step) {
        for (std::size_t metric = 0; metric < 12; ++metric) {
            const auto& row = rows.at(step * 12 + metric);
            require(row.time == step * 0.5, "wrong statistics observation time");
            close(row.value, expected[step][metric], "time statistics differ from hand oracle");
        }
        close(rows[step * 12].value, rows[step * 12 + 1].value + rows[step * 12 + 2].value,
              "available and occupied capacity-time must conserve total capacity-time");
    }
    // Observations are projections, so output density cannot affect final sums.
    for (double dt : {0.125, 2.0}) {
        auto alternate = model;
        alternate.dt = dt;
        const auto sampled = ankurafathom::ir::run(alternate);
        for (std::size_t metric = 0; metric < 12; ++metric)
            require(sampled[sampled.size() - 12 + metric].value == rows[96 + metric].value,
                    "observation frequency must not change final time statistics");
    }
    auto unfinished = model;
    unfinished.horizon = 2.5;
    unfinished.agent_pool->schedule.pop_back();
    const auto partial = ankurafathom::ir::run(unfinished);
    require(partial.size() == 72 && partial[63].value == 4 && partial[64].value == 4 &&
            partial[70].value == 3.5 && partial[71].value == 5,
            "unfinished queue and service must contribute through the exact horizon");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected utilization IR fixture");
        test_weighted_capacity_and_read_only_sampling();
        test_zero_capacity_and_origin();
        test_rollback_and_overflow();
        test_ir_oracle(ankurafathom::ir::load_file(argv[1]));
        std::cout << "agent-pool time statistics tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
