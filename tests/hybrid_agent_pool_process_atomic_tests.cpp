#include "ankurafathom/hybrid/agent_pool_process_atomic.hpp"
#include "ankurafathom/hybrid/clocked_sd.hpp"

#include <iostream>
#include <tuple>

namespace {
using Process = ankurafathom::hybrid::AgentPoolProcess<std::size_t>;
using Change = Process::Change;
using Result = Process::Result;
// A wider message family permits explicit adapters to other hybrid components.
using Message = std::variant<Change, Result, double>;
using Atomic = ankurafathom::hybrid::AgentPoolProcessAtomic<std::size_t, Message>;
using Simulator = ankurafathom::devs::Simulator<Message>;
using Input = ankurafathom::devs::Input<Message>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename Callback>
void rejects(Callback callback) {
    bool rejected = false;
    try { callback(); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "invalid operation was accepted");
}
Process::Pool make_pool(std::initializer_list<std::size_t> capacities) {
    Process::Pool::Population population;
    for (auto capacity : capacities) (void)population.spawn(capacity);
    return Process::Pool(std::move(population), [](auto capacity) { return capacity; }, 2);
}
const Atomic& delivery(const Simulator& simulator, std::size_t id) {
    return dynamic_cast<const Atomic&>(simulator.model(id));
}
void same_result(const Result& a, const Result& b) {
    require(a.completed == b.completed && a.staffing.time == b.staffing.time &&
            a.staffing.hired_ids == b.staffing.hired_ids, "delivery result differs from reference");
    auto grants = [](const auto& source) {
        std::vector<std::pair<std::uint64_t, std::size_t>> result;
        for (const auto& value : source) result.emplace_back(value.request_id, value.units);
        return result;
    };
    auto shares = [](const auto& source) {
        std::vector<std::tuple<std::uint64_t, std::uint64_t, std::size_t>> result;
        for (const auto& value : source)
            result.emplace_back(value.request_id, value.agent_id, value.units);
        return result;
    };
    require(grants(a.staffing.grants) == grants(b.staffing.grants) &&
            shares(a.staffing.assignments) == shares(b.staffing.assignments) &&
            shares(a.staffing.unassignments) == shares(b.staffing.unassignments),
            "delivery grant or ownership trace differs from reference");
}

// Test adapter: committed delivery completions become pulses to an SD stock.
// Optional failure injection exercises rollback after the sender has transitioned.
class CompletionPulse final : public ankurafathom::devs::Atomic<Message> {
public:
    explicit CompletionPulse(const bool* fail = nullptr) : fail_(fail) {}
    std::unique_ptr<ankurafathom::devs::Atomic<Message>> clone() const override {
        return std::make_unique<CompletionPulse>(*this);
    }
    double time_advance() const override {
        return pending_ ? 0 : std::numeric_limits<double>::infinity();
    }
    std::vector<ankurafathom::devs::PortValue<Message>> output() const override {
        if (!pending_) return {};
        return {{0, pending_}};
    }
    void internal_transition() override { pending_ = 0; }
    void external_transition(double, const std::vector<Input>& bag) override {
        for (const auto& input : bag) {
            const auto& result = std::get<Result>(input.value);
            ++received_;
            pending_ += static_cast<double>(result.completed.size());
            if (fail_ && *fail_ && !result.completed.empty())
                throw std::runtime_error("receiver failure after mutation");
        }
    }
    void confluent_transition(const std::vector<Input>& bag) override {
        internal_transition();
        external_transition(0, bag);
    }
    std::size_t received() const { return received_; }
private:
    const bool* fail_;
    double pending_ = 0;
    std::size_t received_ = 0;
};

std::vector<Change> hand_schedule() {
    std::vector<Change> changes(5);
    changes[0].arrivals = {{10, 1, 1}, {11, 1, 2}, {12, 1, 0.5}, {13, 2, 1}};
    changes[1].workforce.time = 1;
    changes[1].workforce.departures = {0};
    changes[2].workforce.time = 1.5;
    changes[2].workforce.hires = {1};
    changes[3].workforce.time = 2;
    changes[3].arrivals = {{14, 1, 0.25}};
    changes[3].workforce.phases = {[](std::size_t i, const auto& snapshot, const auto& broker) {
        require(broker.allocated_units() == 0, "phase must observe all due releases");
        return snapshot[i].value;
    }};
    changes[4].workforce.time = 3;
    changes[4].workforce.departures = {1};
    return changes;
}

void test_reference_trace_and_sd_coupling() {
    Process reference(make_pool({1, 1}));
    std::vector<Result> expected;
    const auto changes = hand_schedule();
    std::size_t index = 0;
    while (index < changes.size() || std::isfinite(reference.next_completion())) {
        if (index < changes.size() && changes[index].workforce.time <= reference.next_completion())
            expected.push_back(reference.apply(changes[index++]));
        else {
            Change due;
            due.workforce.time = reference.next_completion();
            expected.push_back(reference.apply(due));
        }
    }
    Simulator simulator;
    const auto id = simulator.add(std::make_unique<Atomic>(make_pool({1, 1})));
    const auto adapter = simulator.add(std::make_unique<CompletionPulse>());
    ankurafathom::sd::Model stock;
    (void)stock.add_stock("completed", 0);
    using SD = ankurafathom::hybrid::ClockedSD<Message>;
    const auto sd = simulator.add(std::make_unique<SD>(std::move(stock), 0.5));
    simulator.connect(id, Atomic::output_port, adapter, 0);
    simulator.connect(adapter, 0, sd, 0);
    for (const auto& change : changes)
        simulator.inject(change.workforce.time, id, Atomic::input_port, change);
    std::vector<Result> actual;
    for (const auto& step : simulator.run_until_transactional(4))
        for (const auto& event : step.emissions)
            if (event.source == id) {
                actual.push_back(std::get<Result>(event.value));
                require(step.time == actual.back().staffing.time, "publication moved physical time");
            }
    require(actual.size() == expected.size() && actual.size() == 6, "result count differs");
    for (std::size_t i = 0; i < actual.size(); ++i) same_result(actual[i], expected[i]);
    const auto& core = delivery(simulator, id).core();
    const auto& totals = core.statistics();
    const auto areas = core.time_statistics(4);
    require(totals.accepted == 5 && totals.started == 5 && totals.completed == 5 &&
            totals.wait_total == 4.5 && totals.cycle_total == 9.25 &&
            areas.capacity_time == 6.5 && areas.allocated_time == 5.75 &&
            areas.utilization() == 23.0 / 26.0 && core.system().broker().allocated_units() == 0,
            "DEVS delivery differs from hand-computed staffing totals");
    require(dynamic_cast<const SD&>(simulator.model(sd)).model().state()[0] == 5,
            "delivery completions did not reach SD exactly once");
}

void test_publication_confluence_and_copy() {
    Atomic original(make_pool({1}));
    Change first;
    first.arrivals = {{1, 1, 1}, {2, 1, 2}};
    original.external_transition(0, {{0, Atomic::input_port, first}});
    auto copy_base = original.clone();
    auto& copy = dynamic_cast<Atomic&>(*copy_base);
    same_result(std::get<Result>(copy.output()[0].value),
                std::get<Result>(original.output()[0].value));
    Change hire;
    hire.workforce.hires = {1};
    copy.confluent_transition({{0, Atomic::input_port, hire}});
    require(copy.core().records().at(2).started == 0 && !original.core().records().at(2).started &&
            copy.core().system().pool().capacity() == 2 && original.core().system().pool().capacity() == 1,
            "clone shares population or ownership state");
    copy.internal_transition(); // publish hire result
    require(copy.output().empty() && copy.next_event_time() == 1, "completion detection must be silent");
    copy.internal_transition(); // detect completion
    require(copy.core().statistics().completed == 1 && original.core().statistics().completed == 0 &&
            copy.core().time_statistics(1).allocated_time == 2 &&
            original.core().time_statistics(1).allocated_time == 1,
            "clone shares completion calendar or statistics");
    Process assigned(make_pool({2}));
    assigned = original.core();
    Change completion;
    completion.workforce.time = 1;
    (void)assigned.apply(completion);
    require(assigned.records().at(2).started == 1 && !original.core().records().at(2).started,
            "copy assignment shares delivery state");

    Simulator simulator;
    const auto id = simulator.add(std::make_unique<Atomic>(make_pool({1})));
    simulator.inject(0, id, Atomic::input_port, first);
    require(simulator.step()->emissions.empty(), "admission must precede publication");
    simulator.inject(0, id, Atomic::input_port, hire);
    const auto prior = simulator.step();
    const auto next = simulator.step();
    require(prior->time == 0 && next->time == 0 && prior->emissions.size() == 1 &&
            next->emissions.size() == 1 &&
            std::get<Result>(prior->emissions[0].value).staffing.grants[0].request_id == 1 &&
            std::get<Result>(next->emissions[0].value).staffing.grants[0].request_id == 2,
            "publication confluence lost or duplicated a committed result");
}

void test_bags_and_output_purity() {
    Atomic atomic(make_pool({1}));
    std::size_t calls = 0;
    Change workforce;
    workforce.workforce.hires = {1};
    workforce.workforce.phases = {[&](std::size_t i, const auto& snapshot, const auto&) {
        ++calls;
        return snapshot[i].value;
    }};
    Change first;
    first.arrivals = {{4, 1, 1}};
    Change second;
    second.arrivals = {{5, 1, 2}, {6, 1, 1}};
    atomic.external_transition(0, {{0, 0, first}, {1, 0, workforce}, {2, 0, second}});
    const auto result = std::get<Result>(atomic.output()[0].value);
    require(result.staffing.grants.size() == 2 && result.staffing.grants[0].request_id == 4 &&
            result.staffing.grants[1].request_id == 5 && calls == 2,
            "arrival bags lost FIFO order or reran workforce phases");
    for (int i = 0; i < 3; ++i) same_result(result, std::get<Result>(atomic.output()[0].value));
    require(calls == 2 && atomic.core().statistics().accepted == 3,
            "output reevaluated transaction callbacks");
    Change wrong_time;
    wrong_time.workforce.time = 1;
    Change manual;
    manual.workforce.releases = {{4}};
    Change manual_request;
    manual_request.workforce.requests = {{8, 1}};
    Change duplicate;
    duplicate.arrivals = {{8, 1, 1}, {8, 1, 2}};
    const std::vector<std::vector<Input>> invalid{
        {}, {{0, 7, first}}, {{0, 0, Message{3.0}}}, {{0, 0, wrong_time}},
        {{0, 0, manual}}, {{0, 0, manual_request}}, {{0, 0, duplicate}},
        {{0, 0, workforce}, {1, 0, workforce}}
    };
    for (const auto& bag : invalid) {
        rejects([&] { atomic.confluent_transition(bag); });
        same_result(result, std::get<Result>(atomic.output()[0].value));
        require(atomic.core().statistics().accepted == 3 && atomic.core().system().pool().capacity() == 2,
                "invalid confluent bag altered delivery state");
    }
    atomic.internal_transition();
    rejects([&] { atomic.external_transition(0.5, {{0, 0, wrong_time}}); });
    rejects([&] { atomic.external_transition(-1, {{0, 0, first}}); });
    require(atomic.core().system().now() == 0 && atomic.output().empty(),
            "invalid elapsed time changed committed state");
}

void test_checked_completion_and_publication_retry() {
    bool fail_phase = true;
    bool fail_receiver = true;
    Simulator simulator;
    const auto id = simulator.add(std::make_unique<Atomic>(make_pool({1, 1})));
    const auto receiver = simulator.add(std::make_unique<CompletionPulse>(&fail_receiver));
    simulator.connect(id, Atomic::output_port, receiver, 0);
    Change first;
    first.arrivals = {{1, 1, 1}, {2, 1, 2}, {3, 1, 1}};
    simulator.inject(0, id, Atomic::input_port, first);
    (void)simulator.run_until_transactional(0);
    std::size_t phase_calls = 0;
    Change departure;
    departure.workforce.time = 1;
    departure.workforce.departures = {0};
    departure.workforce.phases = {[&](std::size_t i, const auto& snapshot, const auto& broker) {
        ++phase_calls;
        require(broker.allocated_to(0) == 0, "due release did not precede departure phase");
        if (fail_phase) throw std::runtime_error("injected phase failure");
        return snapshot[i].value;
    }};
    simulator.inject(1, id, Atomic::input_port, departure);
    rejects([&] { (void)simulator.step_transactional(); });
    const auto& rolled_back = delivery(simulator, id).core();
    require(simulator.now() == 0 && simulator.next_time() == 1 &&
            rolled_back.system().now() == 0 && rolled_back.next_completion() == 1 &&
            rolled_back.statistics().completed == 0 && !rolled_back.records().at(1).completed &&
            rolled_back.system().population().active_count() == 2 &&
            rolled_back.system().broker().allocated_to(0) == 1 &&
            rolled_back.time_statistics(1).allocated_time == 2,
            "failed completion did not restore all delivery and simulator state");
    fail_phase = false;
    require(simulator.step_transactional()->emissions.empty(), "completion commit emitted speculative output");
    require(delivery(simulator, id).core().statistics().completed == 1 &&
            !delivery(simulator, id).core().records().at(3).started,
            "departure failed to precede regrant on retry");
    const auto committed_calls = phase_calls;
    rejects([&] { (void)simulator.step_transactional(); });
    require(simulator.now() == 1 && simulator.next_time() == 1 &&
            delivery(simulator, id).core().statistics().completed == 1 &&
            delivery(simulator, id).output().size() == 1 &&
            dynamic_cast<const CompletionPulse&>(simulator.model(receiver)).received() == 1,
            "receiver failure did not restore pending publication and receiver state");
    fail_receiver = false;
    const auto published = simulator.step_transactional();
    require(published->emissions.size() == 1 &&
            std::get<Result>(published->emissions[0].value).completed == std::vector<std::uint64_t>{1} &&
            delivery(simulator, id).output().empty() && phase_calls == committed_calls &&
            dynamic_cast<const CompletionPulse&>(simulator.model(receiver)).received() == 2,
            "publication retry repeated the transaction or lost its result");
    (void)simulator.run_until_transactional(3);
    require(delivery(simulator, id).core().statistics().completed == 3,
            "delivery did not continue after checked retries");
}

void test_exact_deadline_and_external_clock() {
    constexpr double intermediate = 8.943065046370474;
    constexpr double deadline = 50.058611305029835;
    require(intermediate + (deadline - intermediate) != deadline,
            "clock regression fixture no longer exercises binary64 rounding");
    for (bool with_completion : {false, true}) {
        Simulator simulator;
        const auto id = simulator.add(std::make_unique<Atomic>(make_pool({1})));
        if (with_completion) {
            Change first;
            first.arrivals = {{1, 1, deadline}, {2, 1, 1}};
            simulator.inject(0, id, 0, first);
        }
        Change heartbeat;
        heartbeat.workforce.time = intermediate;
        simulator.inject(intermediate, id, 0, heartbeat);
        (void)simulator.run_until_transactional(intermediate);
        if (with_completion)
            require(simulator.next_time() == deadline, "relative scheduling shifted the absolute deadline");
        Change departure;
        departure.workforce.time = deadline;
        departure.workforce.departures = {0};
        simulator.inject(deadline, id, 0, departure);
        const auto detected = simulator.step_transactional();
        const auto published = simulator.step_transactional();
        const auto& core = delivery(simulator, id).core();
        require(detected->time == deadline && detected->emissions.empty() &&
                published->time == deadline && published->emissions.size() == 1 &&
                core.system().now() == deadline && core.system().pool().capacity() == 0 &&
                std::isinf(simulator.next_time()), "exact deadline or external timestamp was reconstructed incorrectly");
        if (with_completion)
            require(core.statistics().completed == 1 && !core.records().at(2).started &&
                    core.records().at(1).completed == deadline,
                    "rounding split simultaneous completion and departure");
    }

    // Ownership last changed at intermediate, so release must not reconstruct
    // deadline by adding an elapsed duration to the broker's prior clock.
    Simulator simulator;
    const auto id = simulator.add(std::make_unique<Atomic>(make_pool({1, 1})));
    Change first;
    first.arrivals = {{1, 1, deadline}};
    simulator.inject(0, id, 0, first);
    Change second;
    second.workforce.time = intermediate;
    second.arrivals = {{2, 1, 100}};
    simulator.inject(intermediate, id, 0, second);
    (void)simulator.run_until_transactional(intermediate);
    (void)simulator.step_transactional(); // commit completion at deadline
    Change later_microstep;
    later_microstep.workforce.time = deadline;
    later_microstep.arrivals = {{3, 1, 1}};
    simulator.inject(deadline, id, 0, later_microstep);
    (void)simulator.run_until_transactional(deadline);
    const auto& core = delivery(simulator, id).core();
    require(core.system().broker().clock() == deadline && core.records().at(3).started == deadline &&
            core.statistics().completed == 1 && core.system().broker().allocated_units() == 2,
            "broker rounding prevented a later same-time transaction");

    Simulator mismatch;
    const auto target = mismatch.add(std::make_unique<Atomic>(make_pool({1})));
    Change heartbeat;
    heartbeat.workforce.time = intermediate;
    mismatch.inject(intermediate, target, 0, heartbeat);
    (void)mismatch.run_until_transactional(intermediate);
    Change wrong;
    wrong.workforce.time = std::nextafter(deadline, std::numeric_limits<double>::infinity());
    require(wrong.workforce.time != deadline && wrong.workforce.time - intermediate == deadline - intermediate,
            "mismatched timestamp fixture must share the same rounded elapsed duration");
    mismatch.inject(deadline, target, 0, wrong);
    rejects([&] { (void)mismatch.step_transactional(); });
    require(mismatch.now() == intermediate && mismatch.next_time() == deadline &&
            delivery(mismatch, target).core().system().now() == intermediate,
            "elapsed-time equality accepted a distinct absolute timestamp");
}
} // namespace

int main() {
    try {
        test_reference_trace_and_sd_coupling();
        test_publication_confluence_and_copy();
        test_bags_and_output_purity();
        test_checked_completion_and_publication_retry();
        test_exact_deadline_and_external_clock();
        std::cout << "agent-pool delivery atomic tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
