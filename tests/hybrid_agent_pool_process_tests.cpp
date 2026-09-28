#include "ankurafathom/hybrid/agent_pool_process.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using Pool = ankurafathom::hybrid::TransactionalAgentPool<std::size_t>;
using Process = ankurafathom::hybrid::AgentPoolProcess<std::size_t>;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

Process make_process(std::initializer_list<std::size_t> capacities) {
    ankurafathom::abm::SyncPopulation<std::size_t> population;
    for (auto capacity : capacities) (void)population.spawn(capacity);
    return Process(Pool(std::move(population), [](const auto& value) { return value; }, 2));
}

void test_delivery_and_rollback() {
    auto process = make_process({1, 1});
    Process::Change arrival;
    arrival.arrivals = {{10, 1, 1}, {11, 1, 2}, {12, 1, 0.5}, {13, 2, 1}};
    auto result = process.apply(arrival);
    require(result.staffing.grants.size() == 2 && process.next_completion() == 1,
            "only staffed engagements may schedule completions");

    Process::Change skipped;
    skipped.workforce.time = 2;
    bool rejected = false;
    try { (void)process.apply(skipped); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && process.system().now() == 0, "pending completions cannot be skipped");

    Process::Change departure;
    departure.workforce.time = 1;
    departure.workforce.departures = {0};
    departure.workforce.phases = {[](std::size_t, const auto&, const auto&) -> std::size_t {
        throw std::runtime_error("phase failure after staged completion");
    }};
    rejected = false;
    try { (void)process.apply(departure); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected && process.system().now() == 0 && process.next_completion() == 1 &&
            process.statistics().completed == 0 && !process.records().at(10).completed &&
            process.system().population().active_count() == 2 &&
            process.system().broker().allocated_to(0) == 1 && process.system().pool().waiting() == 2,
            "phase failure must roll back completion, workforce, ownership, calendar and time");
    departure.workforce.phases.clear();
    result = process.apply(departure);
    require(result.completed == std::vector<std::uint64_t>{10} && result.staffing.grants.empty() &&
            process.system().pool().capacity() == 1 && process.next_completion() == 2,
            "departure must take effect before a completed assignment is regranted");

    Process::Change hire;
    hire.workforce.time = 1.5;
    hire.workforce.hires = {1};
    result = process.apply(hire);
    require(result.staffing.grants.size() == 1 && result.staffing.grants[0].request_id == 12 &&
            process.records().at(12).started == 1.5 && process.next_completion() == 2,
            "delivery duration must start at grant time, not arrival time");

    Process::Change simultaneous;
    simultaneous.workforce.time = 2;
    simultaneous.arrivals = {{14, 1, 0.25}};
    simultaneous.workforce.phases = {[](std::size_t i, const auto& snapshot, const auto& broker) {
        require(broker.allocated_units() == 0, "phase must see all simultaneous completion releases");
        return snapshot[i].value;
    }};
    result = process.apply(simultaneous);
    require(result.completed == std::vector<std::uint64_t>({11, 12}) &&
            result.staffing.grants.size() == 1 && result.staffing.grants[0].request_id == 13 &&
            result.staffing.assignments.size() == 2 &&
            process.system().broker().allocated_to(1) == 1 &&
            process.system().broker().allocated_to(2) == 1 && !process.records().at(14).started,
            "FIFO engagement must hold both agent shares before later arrivals can start");

    Process::Change last_departure;
    last_departure.workforce.time = 3;
    last_departure.workforce.departures = {1};
    result = process.apply(last_departure);
    require(result.completed == std::vector<std::uint64_t>{13} &&
            process.records().at(14).started == 3 && process.next_completion() == 3.25,
            "completion and departure must safely admit the next engagement");
    Process::Change finish;
    finish.workforce.time = 3.25;
    (void)process.apply(finish);
    const auto& statistics = process.statistics();
    require(statistics.accepted == 5 && statistics.started == 5 && statistics.completed == 5 &&
            statistics.wait_total == 4.5 && statistics.cycle_total == 9.25 &&
            process.system().pool().allocated() == 0 && process.system().pool().available() == 1 &&
            std::isinf(process.next_completion()), "delivery totals do not match the hand schedule");
    for (const auto& [id, record] : process.records()) {
        (void)id;
        require(record.completed && *record.completed - *record.started == record.duration,
                "fixed-duration delivery changed while in service");
    }
}

void test_calendar_failure_rollback() {
    for (const auto& [time, duration] : std::vector<std::pair<double, double>>{
             {1e308, 1e308}, {1e20, 1}, {0, 0}, {0, -1},
             {0, std::numeric_limits<double>::infinity()},
             {0, std::numeric_limits<double>::quiet_NaN()}}) {
        auto process = make_process({1});
        Process::Change change;
        change.workforce.time = time;
        change.arrivals = {{1, 1, duration}};
        bool rejected = false;
        try { (void)process.apply(change); }
        catch (const std::exception&) { rejected = true; }
        require(rejected && process.system().now() == 0 && process.records().empty() &&
                process.statistics().accepted == 0 && process.system().pool().available() == 1 &&
                std::isinf(process.next_completion()), "invalid completion must roll back an already staged grant");
        change.workforce.time = 0;
        change.arrivals[0].duration = 1;
        require(process.apply(change).staffing.grants.size() == 1,
                "failed transaction must not consume engagement ID");
    }
    auto process = make_process({1});
    Process::Change first;
    first.arrivals = {{1, 1, 1}};
    (void)process.apply(first);
    Process::Change duplicate;
    duplicate.workforce.time = 1;
    duplicate.arrivals = {{1, 1, 1}};
    bool rejected = false;
    try { (void)process.apply(duplicate); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && !process.records().at(1).completed && process.next_completion() == 1,
            "duplicate arrival must roll back a simultaneous completion");
}

void test_fifo_and_fixed_duration() {
    auto process = make_process({1});
    Process::Change arrivals;
    arrivals.arrivals = {{20, 2, 1}, {21, 1, 0.5}};
    require(process.apply(arrivals).staffing.grants.empty() &&
            process.system().pool().available() == 1 && std::isinf(process.next_completion()),
            "smaller engagement must not bypass the FIFO head");
    Process::Change hire;
    hire.workforce.time = 1;
    hire.workforce.hires = {1};
    require(process.apply(hire).staffing.grants.at(0).request_id == 20,
            "hire should start the blocked FIFO head");
    Process::Change completion;
    completion.workforce.time = 2;
    require(process.apply(completion).staffing.grants.at(0).request_id == 21,
            "completion should start the next engagement");
    Process::Change capacity;
    capacity.workforce.time = 2.25;
    capacity.workforce.updates = {{0, 2}};
    (void)process.apply(capacity);
    require(process.next_completion() == 2.5, "capacity updates must not reschedule in-service work");
    completion.workforce.time = 2.5;
    (void)process.apply(completion);
    require(process.statistics().wait_total == 3 && process.statistics().cycle_total == 4.5,
            "FIFO waiting and delivery totals are incorrect");
}
} // namespace

int main() {
    try {
        test_delivery_and_rollback();
        test_calendar_failure_rollback();
        test_fifo_and_fixed_duration();
        std::cout << "agent-pool delivery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
