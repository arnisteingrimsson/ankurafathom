#include "ankurafathom/devs/simulator.hpp"
#include "adevs/simulator.h"

#include <iostream>
#include <algorithm>
#include <limits>
#include <list>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using AdevsAtomic = adevs::Atomic<int>;
using AdevsValue = adevs::PinValue<int>;
using NativeAtomic = ankurafathom::devs::Atomic<int>;
using NativeInput = ankurafathom::devs::Input<int>;
using NativeValue = ankurafathom::devs::PortValue<int>;

struct Observation {
    std::vector<double> times;
    std::vector<int> values;
    std::vector<std::size_t> bag_sizes;
    int confluent = 0;
};

class NativeGenerator final : public NativeAtomic {
public:
    explicit NativeGenerator(int limit, int base = 0) : limit_(limit), base_(base) {}
    double time_advance() const override {
        return count_ < limit_ ? 1 : std::numeric_limits<double>::infinity();
    }
    std::vector<NativeValue> output() const override { return {{0, base_ + count_ + 1}}; }
    void internal_transition() override { ++count_; }
    void external_transition(double, const std::vector<NativeInput>&) override { throw std::logic_error("unexpected input"); }
    void confluent_transition(const std::vector<NativeInput>&) override { throw std::logic_error("unexpected confluence"); }
private:
    int count_ = 0;
    int limit_;
    int base_;
};

class AdevsGenerator final : public AdevsAtomic {
public:
    explicit AdevsGenerator(int limit, int base = 0) : limit_(limit), base_(base) {}
    double ta() override { return count_ < limit_ ? 1 : adevs_inf<double>(); }
    void output_func(std::list<AdevsValue>& bag) override { bag.emplace_back(output_pin, base_ + count_ + 1); }
    void delta_int() override { ++count_; }
    void delta_ext(double, const std::list<AdevsValue>&) override { throw std::logic_error("unexpected input"); }
    void delta_conf(const std::list<AdevsValue>&) override { throw std::logic_error("unexpected confluence"); }
    const adevs::pin_t output_pin;
private:
    int count_ = 0;
    int limit_;
    int base_;
};

class NativeReceiver final : public NativeAtomic {
public:
    NativeReceiver(Observation& observed, bool own_event) : observed_(observed), own_event_(own_event) {}
    double time_advance() const override {
        return own_event_ && !done_ ? 1 : std::numeric_limits<double>::infinity();
    }
    std::vector<NativeValue> output() const override { return {}; }
    void internal_transition() override { done_ = true; }
    void external_transition(double elapsed, const std::vector<NativeInput>& bag) override {
        observed_.times.push_back(elapsed);
        observed_.bag_sizes.push_back(bag.size());
        for (const auto& input : bag) observed_.values.push_back(input.value);
    }
    void confluent_transition(const std::vector<NativeInput>& bag) override {
        ++observed_.confluent;
        done_ = true;
        observed_.times.push_back(0);
        observed_.bag_sizes.push_back(bag.size());
        for (const auto& input : bag) observed_.values.push_back(input.value);
    }
private:
    Observation& observed_;
    bool own_event_;
    bool done_ = false;
};

class AdevsReceiver final : public AdevsAtomic {
public:
    AdevsReceiver(Observation& observed, bool own_event) : observed_(observed), own_event_(own_event) {}
    double ta() override { return own_event_ && !done_ ? 1 : adevs_inf<double>(); }
    void output_func(std::list<AdevsValue>&) override {}
    void delta_int() override { done_ = true; }
    void delta_ext(double elapsed, const std::list<AdevsValue>& bag) override {
        observed_.times.push_back(elapsed);
        observed_.bag_sizes.push_back(bag.size());
        for (const auto& input : bag) observed_.values.push_back(input.value);
    }
    void delta_conf(const std::list<AdevsValue>& bag) override {
        ++observed_.confluent;
        done_ = true;
        observed_.times.push_back(0);
        observed_.bag_sizes.push_back(bag.size());
        for (const auto& input : bag) observed_.values.push_back(input.value);
    }
private:
    Observation& observed_;
    bool own_event_;
    bool done_ = false;
};

Observation run_native(int generator_count, bool own_event) {
    Observation result;
    ankurafathom::devs::Simulator<int> simulator;
    const auto source = simulator.add(std::make_unique<NativeGenerator>(generator_count));
    const auto target = simulator.add(std::make_unique<NativeReceiver>(result, own_event));
    simulator.connect(source, 0, target, 0);
    simulator.run_until(static_cast<double>(generator_count));
    return result;
}

Observation run_adevs(int generator_count, bool own_event) {
    Observation result;
    auto source = std::make_shared<AdevsGenerator>(generator_count);
    auto target = std::make_shared<AdevsReceiver>(result, own_event);
    auto graph = std::make_shared<adevs::Graph<int>>();
    graph->add_atomic(source);
    graph->add_atomic(target);
    graph->connect(source->output_pin, target);
    adevs::Simulator<int> simulator(graph);
    while (simulator.nextEventTime() <= generator_count) simulator.execNextEvent();
    return result;
}

void compare(int count, bool own_event) {
    const auto native = run_native(count, own_event);
    const auto upstream = run_adevs(count, own_event);
    require(native.times == upstream.times, "elapsed-time trace differs from adevs");
    require(native.values == upstream.values, "value trace differs from adevs");
    require(native.bag_sizes == upstream.bag_sizes, "input bag boundaries differ from adevs");
    require(native.confluent == upstream.confluent, "confluent transition count differs from adevs");
}

struct EchoObservation {
    std::vector<std::vector<int>> input_bags;
    std::vector<std::vector<int>> output_bags;
    int confluent = 0;
    bool operator==(const EchoObservation&) const = default;
};

class NativeEcho final : public NativeAtomic {
public:
    explicit NativeEcho(EchoObservation& observed) : observed_(observed) {}
    double time_advance() const override {
        return pending_.empty() ? std::numeric_limits<double>::infinity() : 0;
    }
    std::vector<NativeValue> output() const override {
        std::vector<NativeValue> values;
        for (int value : pending_) values.push_back({0, value - 1});
        observed_.output_bags.push_back(pending_);
        return values;
    }
    void internal_transition() override { pending_.clear(); }
    void external_transition(double, const std::vector<NativeInput>& bag) override { receive(bag); }
    void confluent_transition(const std::vector<NativeInput>& bag) override {
        ++observed_.confluent;
        internal_transition();
        receive(bag);
    }
private:
    void receive(const std::vector<NativeInput>& bag) {
        std::vector<int> values;
        for (const auto& item : bag) {
            values.push_back(item.value);
            if (item.value > 0) pending_.push_back(item.value);
        }
        std::sort(values.begin(), values.end());
        observed_.input_bags.push_back(std::move(values));
        std::sort(pending_.begin(), pending_.end());
    }
    EchoObservation& observed_;
    std::vector<int> pending_;
};

class AdevsEcho final : public AdevsAtomic {
public:
    explicit AdevsEcho(EchoObservation& observed) : observed_(observed) {}
    double ta() override { return pending_.empty() ? adevs_inf<double>() : 0; }
    void output_func(std::list<AdevsValue>& bag) override {
        for (int value : pending_) bag.emplace_back(output_pin, value - 1);
        observed_.output_bags.push_back(pending_);
    }
    void delta_int() override { pending_.clear(); }
    void delta_ext(double, const std::list<AdevsValue>& bag) override { receive(bag); }
    void delta_conf(const std::list<AdevsValue>& bag) override {
        ++observed_.confluent;
        delta_int();
        receive(bag);
    }
    const adevs::pin_t output_pin;
private:
    void receive(const std::list<AdevsValue>& bag) {
        std::vector<int> values;
        for (const auto& item : bag) {
            values.push_back(item.value);
            if (item.value > 0) pending_.push_back(item.value);
        }
        std::sort(values.begin(), values.end());
        observed_.input_bags.push_back(std::move(values));
        std::sort(pending_.begin(), pending_.end());
    }
    EchoObservation& observed_;
    std::vector<int> pending_;
};

struct FeedbackTrace {
    std::vector<double> times;
    EchoObservation a;
    EchoObservation b;
};

struct NetworkSpec {
    std::vector<std::pair<int, int>> edges;
    int second_source_target;
};

NetworkSpec network_spec(std::uint32_t seed) {
    NetworkSpec spec;
    for (int i = 0; i < 4; ++i) spec.edges.emplace_back(i, i + 1);
    spec.second_source_target = static_cast<int>(seed % 3);
    for (int source = 0; source < 5; ++source) {
        for (int destination = source + 2; destination < 5; ++destination) {
            seed = seed * 1664525U + 1013904223U;
            if ((seed >> 29) % 3 == 0) spec.edges.emplace_back(source, destination);
        }
    }
    if (seed % 2 == 0) spec.edges.emplace_back(4, static_cast<int>(seed % 3));
    return spec;
}

struct NetworkTrace {
    std::vector<double> times;
    std::vector<EchoObservation> nodes = std::vector<EchoObservation>(5);
};

NetworkTrace run_network_native(const NetworkSpec& spec) {
    NetworkTrace trace;
    ankurafathom::devs::Simulator<int> simulator;
    const auto source1 = simulator.add(std::make_unique<NativeGenerator>(1, 2));
    const auto source2 = simulator.add(std::make_unique<NativeGenerator>(1, 1));
    std::vector<std::size_t> ids;
    for (auto& observed : trace.nodes)
        ids.push_back(simulator.add(std::make_unique<NativeEcho>(observed)));
    simulator.connect(source1, 0, ids[0], 0);
    simulator.connect(source2, 0, ids[spec.second_source_target], 0);
    for (const auto [source, destination] : spec.edges)
        simulator.connect(ids[source], 0, ids[destination], 0);
    for (const auto& step : simulator.run_until(1)) trace.times.push_back(step.time);
    return trace;
}

NetworkTrace run_network_adevs(const NetworkSpec& spec) {
    NetworkTrace trace;
    auto source1 = std::make_shared<AdevsGenerator>(1, 2);
    auto source2 = std::make_shared<AdevsGenerator>(1, 1);
    auto graph = std::make_shared<adevs::Graph<int>>();
    graph->add_atomic(source1);
    graph->add_atomic(source2);
    std::vector<std::shared_ptr<AdevsEcho>> nodes;
    for (auto& observed : trace.nodes) {
        auto node = std::make_shared<AdevsEcho>(observed);
        graph->add_atomic(node);
        nodes.push_back(std::move(node));
    }
    graph->connect(source1->output_pin, nodes[0]);
    graph->connect(source2->output_pin, nodes[spec.second_source_target]);
    for (const auto [source, destination] : spec.edges)
        graph->connect(nodes[source]->output_pin, nodes[destination]);
    adevs::Simulator<int> simulator(graph);
    while (simulator.nextEventTime() <= 1) trace.times.push_back(simulator.execNextEvent());
    return trace;
}

void compare_network_family() {
    for (std::uint32_t seed = 0; seed < 64; ++seed) {
        const auto spec = network_spec(seed);
        const auto native = run_network_native(spec);
        const auto upstream = run_network_adevs(spec);
        require(native.times == upstream.times, "network-family microstep times differ from adevs");
        for (std::size_t i = 0; i < native.nodes.size(); ++i)
            require(native.nodes[i] == upstream.nodes[i], "network-family atomic trace differs from adevs");
    }
}

FeedbackTrace run_feedback_native() {
    FeedbackTrace trace;
    ankurafathom::devs::Simulator<int> simulator;
    const auto source1 = simulator.add(std::make_unique<NativeGenerator>(1, 2));
    const auto source2 = simulator.add(std::make_unique<NativeGenerator>(1, 1));
    const auto a = simulator.add(std::make_unique<NativeEcho>(trace.a));
    const auto b = simulator.add(std::make_unique<NativeEcho>(trace.b));
    simulator.connect(source1, 0, a, 0);
    simulator.connect(source2, 0, a, 0);
    simulator.connect(a, 0, b, 0);
    simulator.connect(b, 0, a, 0);
    for (const auto& step : simulator.run_until(1)) trace.times.push_back(step.time);
    return trace;
}

FeedbackTrace run_feedback_adevs() {
    FeedbackTrace trace;
    auto source1 = std::make_shared<AdevsGenerator>(1, 2);
    auto source2 = std::make_shared<AdevsGenerator>(1, 1);
    auto a = std::make_shared<AdevsEcho>(trace.a);
    auto b = std::make_shared<AdevsEcho>(trace.b);
    auto graph = std::make_shared<adevs::Graph<int>>();
    graph->add_atomic(source1);
    graph->add_atomic(source2);
    graph->add_atomic(a);
    graph->add_atomic(b);
    graph->connect(source1->output_pin, a);
    graph->connect(source2->output_pin, a);
    graph->connect(a->output_pin, b);
    graph->connect(b->output_pin, a);
    adevs::Simulator<int> simulator(graph);
    while (simulator.nextEventTime() <= 1) trace.times.push_back(simulator.execNextEvent());
    return trace;
}

void compare_feedback() {
    const auto native = run_feedback_native();
    const auto upstream = run_feedback_adevs();
    require(native.times == upstream.times, "feedback microstep times differ from adevs");
    require(native.a == upstream.a && native.b == upstream.b,
            "feedback input/output bags differ from adevs");
    require(native.times.size() == 4 && native.a.input_bags.front() == std::vector<int>({2, 3}),
            "feedback fixture did not exercise simultaneous inputs and zero-time steps");
}

Observation run_external_native(bool own_event) {
    Observation result;
    ankurafathom::devs::Simulator<int> simulator;
    const auto target = simulator.add(std::make_unique<NativeReceiver>(result, own_event));
    if (!own_event) {
        simulator.inject(0.5, target, 0, 4);
        simulator.inject(0.5, target, 0, 5);
        simulator.inject(1, target, 0, 6);
    } else {
        simulator.inject(1, target, 0, 6);
    }
    simulator.run_until(1);
    return result;
}

Observation run_external_adevs(bool own_event) {
    Observation result;
    auto target = std::make_shared<AdevsReceiver>(result, own_event);
    auto graph = std::make_shared<adevs::Graph<int>>();
    graph->add_atomic(target);
    const adevs::pin_t input_pin;
    graph->connect(input_pin, target);
    adevs::Simulator<int> simulator(graph);
    if (!own_event) {
        AdevsValue first(input_pin, 4);
        AdevsValue second(input_pin, 5);
        simulator.injectInput(first);
        simulator.injectInput(second);
        simulator.setNextTime(0.5);
        require(simulator.execNextEvent() == 0.5, "adevs external test failed at first event");
    }
    AdevsValue last(input_pin, 6);
    simulator.injectInput(last);
    if (!own_event) simulator.setNextTime(1);
    require(simulator.execNextEvent() == 1, "adevs external test failed at second event");
    return result;
}

void compare_external() {
    for (bool own_event : {false, true}) {
        const auto native = run_external_native(own_event);
        const auto upstream = run_external_adevs(own_event);
        require(native.times == upstream.times, "external-input elapsed trace differs from adevs");
        require(native.values == upstream.values, "external-input values differ from adevs");
        require(native.bag_sizes == upstream.bag_sizes, "external-input bags differ from adevs");
        require(native.confluent == upstream.confluent, "external-input confluence differs from adevs");
    }
}

} // namespace

int main() {
    try {
        compare(3, false);
        compare(1, true);
        compare_feedback();
        compare_external();
        compare_network_family();
        std::cout << "DEVS traces agree with pinned adevs cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
