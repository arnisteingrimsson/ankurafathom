#include "ankurafathom/devs/simulator.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using ankurafathom::devs::Atomic;
using ankurafathom::devs::Input;
using ankurafathom::devs::PortValue;
using ankurafathom::devs::Simulator;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Generator final : public Atomic<int> {
public:
    explicit Generator(int limit, double interval = 1) : limit_(limit), interval_(interval) {}
    double time_advance() const override {
        return count_ < limit_ ? interval_ : std::numeric_limits<double>::infinity();
    }
    std::vector<PortValue<int>> output() const override { return {{0, count_ + 1}}; }
    void internal_transition() override { ++count_; }
    void external_transition(double, const std::vector<Input<int>>&) override {
        throw std::logic_error("generator received unexpected input");
    }
    void confluent_transition(const std::vector<Input<int>>&) override {
        throw std::logic_error("generator received unexpected confluent input");
    }
private:
    int count_ = 0;
    int limit_;
    double interval_;
};

class Recorder final : public Atomic<int> {
public:
    double time_advance() const override { return std::numeric_limits<double>::infinity(); }
    std::vector<PortValue<int>> output() const override { return {}; }
    void internal_transition() override { throw std::logic_error("recorder has no internal event"); }
    void external_transition(double elapsed, const std::vector<Input<int>>& bag) override {
        ++calls;
        elapsed_times.push_back(elapsed);
        for (const auto& input : bag) values.push_back(input.value);
        bag_sizes.push_back(bag.size());
    }
    void confluent_transition(const std::vector<Input<int>>&) override {
        throw std::logic_error("recorder has no internal event");
    }
    int calls = 0;
    std::vector<double> elapsed_times;
    std::vector<int> values;
    std::vector<std::size_t> bag_sizes;
};

class Confluent final : public Atomic<int> {
public:
    double time_advance() const override {
        return done ? std::numeric_limits<double>::infinity() : 1;
    }
    std::vector<PortValue<int>> output() const override { return {{0, 99}}; }
    void internal_transition() override { ++internal_count; done = true; }
    void external_transition(double, const std::vector<Input<int>>&) override { ++external_count; }
    void confluent_transition(const std::vector<Input<int>>& bag) override {
        ++confluent_count;
        received = bag.at(0).value;
        done = true;
    }
    bool done = false;
    int internal_count = 0;
    int external_count = 0;
    int confluent_count = 0;
    int received = 0;
};

class ZeroCycle final : public Atomic<int> {
public:
    double time_advance() const override { return 0; }
    std::vector<PortValue<int>> output() const override { return {}; }
    void internal_transition() override {}
    void external_transition(double, const std::vector<Input<int>>&) override {}
    void confluent_transition(const std::vector<Input<int>>&) override {}
};

class AbsoluteDeadline final : public Atomic<int> {
public:
    explicit AbsoluteDeadline(double deadline, double advance = 0)
        : deadline_(deadline), advance_(advance) {}
    std::unique_ptr<Atomic<int>> clone() const override {
        return std::make_unique<AbsoluteDeadline>(*this);
    }
    double time_advance() const override { return advance_; }
    std::optional<double> next_event_time() const override { return deadline_; }
    std::vector<PortValue<int>> output() const override { return {{0, 1}}; }
    void internal_transition() override { deadline_ = std::numeric_limits<double>::infinity(); }
    void external_transition(double, const std::vector<Input<int>>&) override { deadline_ = 0.5; }
    void confluent_transition(const std::vector<Input<int>>&) override { internal_transition(); }
private:
    double deadline_;
    double advance_;
};

void test_absolute_deadline_contract() {
    for (double deadline : {0.0, 0.75, std::numeric_limits<double>::infinity()}) {
        Simulator<int> sim;
        (void)sim.add(std::make_unique<AbsoluteDeadline>(deadline));
        require(sim.next_time() == deadline, "absolute deadline must be authoritative");
        if (std::isfinite(deadline)) {
            const auto event = sim.step_transactional();
            require(event && event->time == deadline && event->emissions.size() == 1,
                    "absolute deadline event did not fire");
        }
        require(!sim.step_transactional(), "infinite absolute deadline must passivate");
    }
    for (double deadline : {-1.0, -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        Simulator<int> sim;
        (void)sim.add(std::make_unique<AbsoluteDeadline>(deadline));
        bool rejected = false;
        try { (void)sim.step_transactional(); }
        catch (const std::overflow_error&) { rejected = true; }
        require(rejected && sim.now() == 0, "invalid absolute deadline must be rejected");
    }
    Simulator<int> past;
    const auto id = past.add(std::make_unique<AbsoluteDeadline>(2));
    past.inject(1, id, 0, 1); // test component attempts to reschedule to 0.5
    bool rejected = false;
    try { (void)past.step_transactional(); }
    catch (const std::overflow_error&) { rejected = true; }
    require(rejected && past.now() == 0 && past.next_time() == 1 &&
            past.model(id).next_event_time() == 2,
            "past reschedule must restore the checked step and absolute deadline");
    Simulator<int> invalid_advance;
    (void)invalid_advance.add(std::make_unique<AbsoluteDeadline>(2, -1));
    rejected = false;
    try { (void)invalid_advance.step_transactional(); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "absolute deadline must not bypass time-advance validation");
}

class CloneableGenerator final : public Atomic<int> {
public:
    std::unique_ptr<Atomic<int>> clone() const override {
        return std::make_unique<CloneableGenerator>(*this);
    }
    double time_advance() const override { return count < 2 ? 1 : std::numeric_limits<double>::infinity(); }
    std::vector<PortValue<int>> output() const override { return {{0, count + 1}}; }
    void internal_transition() override { ++count; }
    void external_transition(double, const std::vector<Input<int>>&) override {
        throw std::logic_error("generator received unexpected input");
    }
    void confluent_transition(const std::vector<Input<int>>&) override {
        throw std::logic_error("generator received unexpected input");
    }
    int count = 0;
};

class CloneableFaultSink final : public Atomic<int> {
public:
    explicit CloneableFaultSink(const bool& fail) : fail_(&fail) {}
    std::unique_ptr<Atomic<int>> clone() const override {
        return std::make_unique<CloneableFaultSink>(*this);
    }
    double time_advance() const override { return std::numeric_limits<double>::infinity(); }
    std::vector<PortValue<int>> output() const override { return {}; }
    void internal_transition() override { throw std::logic_error("sink has no internal event"); }
    void external_transition(double, const std::vector<Input<int>>& bag) override {
        count += static_cast<int>(bag.size());
        if (*fail_) throw std::runtime_error("injected sink failure");
    }
    void confluent_transition(const std::vector<Input<int>>&) override {
        throw std::logic_error("sink has no internal event");
    }
    int count = 0;
private:
    const bool* fail_;
};

void test_whole_step_rollback_and_retry() {
    bool fail = true;
    Simulator<int> sim;
    const auto source = sim.add(std::make_unique<CloneableGenerator>());
    const auto sink = sim.add(std::make_unique<CloneableFaultSink>(fail));
    sim.connect(source, 0, sink, 0);
    bool rejected = false;
    try { (void)sim.step_transactional(); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected && sim.now() == 0 && sim.next_time() == 1 &&
            dynamic_cast<const CloneableGenerator&>(sim.model(source)).count == 0 &&
            dynamic_cast<const CloneableFaultSink&>(sim.model(sink)).count == 0,
            "failed step must restore every atomic and the event clock");
    fail = false;
    const auto retry = sim.step_transactional();
    require(retry && retry->time == 1 && retry->emissions.size() == 1 &&
            retry->emissions[0].value == 1 &&
            dynamic_cast<const CloneableGenerator&>(sim.model(source)).count == 1 &&
            dynamic_cast<const CloneableFaultSink&>(sim.model(sink)).count == 1 &&
            sim.next_time() == 2,
            "retry must reproduce the failed event once without double transitions");

    Simulator<int> unsupported;
    (void)unsupported.add(std::make_unique<Generator>(1));
    rejected = false;
    try { (void)unsupported.step_transactional(); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected && unsupported.now() == 0 && unsupported.next_time() == 1,
            "transactional mode must reject unsupported atomics before advancing");

    fail = true;
    Simulator<int> injected;
    const auto injected_sink = injected.add(std::make_unique<CloneableFaultSink>(fail));
    injected.inject(2, injected_sink, 0, 42);
    rejected = false;
    try { (void)injected.step_transactional(); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected && injected.now() == 0 && injected.next_time() == 2 &&
            dynamic_cast<const CloneableFaultSink&>(injected.model(injected_sink)).count == 0,
            "failed external injection must remain queued without changing the sink");
    fail = false;
    const auto external_retry = injected.step_transactional();
    require(external_retry && external_retry->injections.size() == 1 &&
            external_retry->injections[0].value == 42 &&
            dynamic_cast<const CloneableFaultSink&>(injected.model(injected_sink)).count == 1 &&
            !std::isfinite(injected.next_time()),
            "external injection retry must deliver its original payload exactly once");
}

void test_multiple_inputs_and_fanout() {
    Simulator<int> sim;
    const auto source_a = sim.add(std::make_unique<Generator>(1));
    const auto source_b = sim.add(std::make_unique<Generator>(1));
    auto first = std::make_unique<Recorder>();
    auto second = std::make_unique<Recorder>();
    Recorder* first_ptr = first.get();
    Recorder* second_ptr = second.get();
    const auto sink_a = sim.add(std::move(first));
    const auto sink_b = sim.add(std::move(second));
    sim.connect(source_a, 0, sink_a, 2);
    sim.connect(source_b, 0, sink_a, 2);
    sim.connect(source_a, 0, sink_b, 3);
    const auto trace = sim.run_until(2);
    require(trace.size() == 1 && trace[0].time == 1, "simultaneous generators must make one step");
    require(first_ptr->calls == 1 && first_ptr->bag_sizes[0] == 2,
            "multiple simultaneous inputs must be delivered in one bag");
    require(first_ptr->values == std::vector<int>({1, 1}), "input bag order or values are wrong");
    require(second_ptr->calls == 1 && second_ptr->values == std::vector<int>({1}),
            "fan-out did not deliver to second recipient");
    require(first_ptr->elapsed_times[0] == 1, "external elapsed time is wrong");
}

void test_confluent_and_pretransition_output() {
    Simulator<int> sim;
    const auto generator = sim.add(std::make_unique<Generator>(1));
    auto target = std::make_unique<Confluent>();
    Confluent* target_ptr = target.get();
    const auto target_id = sim.add(std::move(target));
    sim.connect(generator, 0, target_id, 0);
    const auto trace = sim.run_until(1);
    require(trace.size() == 1, "confluent test should have one time step");
    require(trace[0].imminent == std::vector<std::size_t>({generator, target_id}),
            "both models must be imminent at the same time");
    require(trace[0].emissions.size() == 2 && trace[0].emissions[1].value == 99,
            "target output must be computed from pre-transition state");
    require(target_ptr->confluent_count == 1 && target_ptr->internal_count == 0 &&
            target_ptr->external_count == 0 && target_ptr->received == 1,
            "confluent transition must run exactly once");
}

void test_event_rescheduling() {
    Simulator<int> sim;
    auto sink = std::make_unique<Recorder>();
    Recorder* sink_ptr = sink.get();
    const auto source = sim.add(std::make_unique<Generator>(3));
    const auto target = sim.add(std::move(sink));
    sim.connect(source, 0, target, 0);
    const auto trace = sim.run_until(3);
    require(trace.size() == 3, "generator should fire three times");
    require(sink_ptr->values == std::vector<int>({1, 2, 3}), "generator event sequence is wrong");
    require(sink_ptr->elapsed_times == std::vector<double>({1, 1, 1}),
            "external elapsed time should be since last transition");
}

void test_zero_time_guard_and_invalid_advance() {
    Simulator<int> sim;
    sim.add(std::make_unique<ZeroCycle>());
    bool caught = false;
    try { sim.run_until(0, 5); }
    catch (const std::runtime_error&) { caught = true; }
    require(caught, "zero-time cycle must hit transition limit");

    Simulator<int> invalid;
    invalid.add(std::make_unique<Generator>(1, -1));
    caught = false;
    try { (void)invalid.next_time(); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "negative time advance must be rejected");
}

void test_dynamic_structure_between_steps() {
    Simulator<int> sim;
    const auto original = sim.add(std::make_unique<Generator>(3));
    auto recorder = std::make_unique<Recorder>();
    Recorder* observed = recorder.get();
    const auto sink = sim.add(std::move(recorder));
    sim.connect(original, 0, sink, 0);
    const auto first = sim.step();
    require(first && first->time == 1 && observed->values.size() == 1,
            "initial event did not reach the sink");
    sim.remove(original);
    require(!std::isfinite(sim.next_time()), "removed component must have no scheduled event");
    const auto replacement = sim.add(std::make_unique<Generator>(1, 2));
    require(replacement > original, "component IDs must not be reused");
    sim.connect(replacement, 0, sink, 0);
    const auto remaining = sim.run_until(3);
    require(remaining.size() == 1 && remaining[0].time == 3,
            "new component should schedule relative to the current boundary time");
    require(observed->values.size() == 2, "dynamic component output did not reach the sink");
}

void test_external_injection_and_confluence() {
    Simulator<int> sim;
    auto recorder = std::make_unique<Recorder>();
    Recorder* observed = recorder.get();
    const auto sink = sim.add(std::move(recorder));
    auto confluent = std::make_unique<Confluent>();
    Confluent* target = confluent.get();
    const auto target_id = sim.add(std::move(confluent));
    sim.inject(0.5, sink, 7, 11);
    sim.inject(0.5, sink, 7, 12);
    sim.inject(1, target_id, 0, 7);
    const auto trace = sim.run_until(1);
    require(trace.size() == 2 && trace[0].time == 0.5 && trace[1].time == 1,
            "external events must set the next event time");
    require(observed->calls == 1 && observed->values == std::vector<int>({11, 12}) &&
            observed->elapsed_times[0] == 0.5,
            "same-time external inputs must preserve insertion order and elapsed time");
    require(trace[0].injections.size() == 2 && trace[0].injections[0].destination == sink &&
            trace[0].injections[0].value == 11,
            "external inputs must appear in the event trace");
    require(target->confluent_count == 1 && target->received == 7 &&
            trace[1].emissions.size() == 1 && trace[1].emissions[0].value == 99,
            "external input at an internal event must cause a confluent transition");
    bool caught = false;
    try { sim.inject(0.9, sink, 0, 1); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "injection in the past must be rejected");

    Simulator<int> removed;
    const auto retired = removed.add(std::make_unique<Recorder>());
    removed.inject(2, retired, 0, 1);
    removed.remove(retired);
    require(!std::isfinite(removed.next_time()), "removed component must discard pending external inputs");
}

} // namespace

int main() {
    try {
        test_multiple_inputs_and_fanout();
        test_confluent_and_pretransition_output();
        test_event_rescheduling();
        test_zero_time_guard_and_invalid_advance();
        test_dynamic_structure_between_steps();
        test_external_injection_and_confluence();
        test_whole_step_rollback_and_retry();
        test_absolute_deadline_contract();
        std::cout << "DEVS kernel tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
