#include "ankurafathom/devs/coupled.hpp"

#include <iostream>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using namespace ankurafathom::devs;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Source final : public Atomic<int> {
public:
    double time_advance() const override { return fired_ ? std::numeric_limits<double>::infinity() : 1; }
    std::vector<PortValue<int>> output() const override { return {{0, 7}}; }
    void internal_transition() override { fired_ = true; }
    void external_transition(double, const std::vector<Input<int>>&) override {
        throw std::logic_error("unexpected source input");
    }
    void confluent_transition(const std::vector<Input<int>>&) override {
        throw std::logic_error("unexpected source confluence");
    }
private:
    bool fired_ = false;
};

class Recorder final : public Atomic<int> {
public:
    double time_advance() const override { return std::numeric_limits<double>::infinity(); }
    std::vector<PortValue<int>> output() const override { return {}; }
    void internal_transition() override { throw std::logic_error("recorder cannot be imminent"); }
    void external_transition(double elapsed, const std::vector<Input<int>>& bag) override {
        elapsed_times.push_back(elapsed);
        for (const auto& input : bag) values.push_back(input.value);
    }
    void confluent_transition(const std::vector<Input<int>>&) override {
        throw std::logic_error("recorder cannot be confluent");
    }
    std::vector<double> elapsed_times;
    std::vector<int> values;
};

void test_nested_equals_flat_and_root_ports() {
    CoupledBuilder<int> builder;
    const auto first = builder.add_coupled(builder.root());
    const auto second = builder.add_coupled(first);
    const auto source = builder.add_atomic(second, std::make_unique<Source>());
    auto inside = std::make_unique<Recorder>();
    Recorder* inside_ptr = inside.get();
    const auto inside_id = builder.add_atomic(second, std::move(inside));
    auto outside = std::make_unique<Recorder>();
    Recorder* outside_ptr = outside.get();
    const auto outside_id = builder.add_atomic(builder.root(), std::move(outside));
    builder.connect(second, source, 0, builder.boundary(), 5);
    builder.connect(first, second, 5, builder.boundary(), 7);
    builder.connect(builder.root(), first, 7, outside_id, 0);
    builder.connect(builder.root(), first, 7, outside_id, 0);
    builder.connect(builder.root(), first, 7, builder.boundary(), 9);
    builder.connect(builder.root(), builder.boundary(), 4, first, 2);
    builder.connect(first, builder.boundary(), 2, second, 3);
    builder.connect(second, builder.boundary(), 3, inside_id, 1);
    auto compiled = builder.compile();
    compiled.inject(0.5, 4, 11);
    const auto nested = compiled.simulator.run_until(1);
    require(nested.size() == 2 && nested[0].time == 0.5 && nested[1].time == 1,
            "nested event times are wrong");
    require(inside_ptr->values == std::vector<int>({11}) &&
            inside_ptr->elapsed_times == std::vector<double>({0.5}),
            "nested root input did not reach the inner atomic");
    require(outside_ptr->values == std::vector<int>({7, 7}),
            "each nested coupling path must deliver one message");
    const auto boundary = compiled.boundary_outputs(nested.back());
    require(boundary.size() == 1 && boundary[0].port == 9 && boundary[0].value == 7,
            "nested output did not reach the root boundary");

    Simulator<int> flat;
    const auto flat_source = flat.add(std::make_unique<Source>());
    auto flat_inside = std::make_unique<Recorder>();
    Recorder* flat_inside_ptr = flat_inside.get();
    const auto flat_inside_id = flat.add(std::move(flat_inside));
    auto flat_outside = std::make_unique<Recorder>();
    Recorder* flat_outside_ptr = flat_outside.get();
    const auto flat_outside_id = flat.add(std::move(flat_outside));
    flat.connect(flat_source, 0, flat_outside_id, 0);
    flat.connect(flat_source, 0, flat_outside_id, 0);
    flat.inject(0.5, flat_inside_id, 1, 11);
    const auto reference = flat.run_until(1);
    require(reference.size() == nested.size() && reference[0].time == nested[0].time &&
            reference[1].time == nested[1].time,
            "flat and nested event times differ");
    require(flat_inside_ptr->values == inside_ptr->values &&
            flat_outside_ptr->values == outside_ptr->values,
            "flat and nested input values differ");
}

void test_invalid_hierarchy_and_boundary_cycle() {
    CoupledBuilder<int> invalid;
    const auto child = invalid.add_coupled(invalid.root());
    const auto source = invalid.add_atomic(child, std::make_unique<Source>());
    const auto sink = invalid.add_atomic(invalid.root(), std::make_unique<Recorder>());
    bool caught = false;
    try { invalid.connect(invalid.root(), source, 0, sink, 0); }
    catch (const std::out_of_range&) { caught = true; }
    require(caught, "couplings must use direct children or a boundary");

    CoupledBuilder<int> cyclic;
    const auto group = cyclic.add_coupled(cyclic.root());
    cyclic.connect(cyclic.root(), cyclic.boundary(), 4, group, 2);
    cyclic.connect(group, cyclic.boundary(), 2, cyclic.boundary(), 7);
    cyclic.connect(cyclic.root(), group, 7, group, 2);
    caught = false;
    try { (void)cyclic.compile(); }
    catch (const std::logic_error&) { caught = true; }
    require(caught, "pure boundary routing cycle must be rejected");
}

void test_deep_hierarchy() {
    CoupledBuilder<int> builder;
    std::vector<Component> groups{builder.root()};
    for (int depth = 0; depth < 32; ++depth)
        groups.push_back(builder.add_coupled(groups.back()));
    const auto source = builder.add_atomic(groups.back(), std::make_unique<Source>());
    builder.connect(groups.back(), source, 0, builder.boundary(), 1);
    for (std::size_t depth = groups.size() - 1; depth > 1; --depth)
        builder.connect(groups[depth - 1], groups[depth], 1, builder.boundary(), 1);
    auto sink = std::make_unique<Recorder>();
    Recorder* observed = sink.get();
    const auto sink_id = builder.add_atomic(builder.root(), std::move(sink));
    builder.connect(builder.root(), groups[1], 1, sink_id, 0);
    auto compiled = builder.compile();
    const auto trace = compiled.simulator.run_until(1);
    require(trace.size() == 1 && observed->values == std::vector<int>({7}),
            "deep boundary chain did not preserve the atomic output");
}

template<class Error, class Action> void rejects(Action action, const char* message) {
    bool caught = false;
    try { action(); } catch (const Error&) { caught = true; }
    require(caught, message);
}

class FaultReceiver final : public Atomic<int> {
public:
    explicit FaultReceiver(std::shared_ptr<bool> fail) : fail_(std::move(fail)) {}
    std::unique_ptr<Atomic<int>> clone() const override { return std::make_unique<FaultReceiver>(*this); }
    double time_advance() const override { return std::numeric_limits<double>::infinity(); }
    std::vector<PortValue<int>> output() const override { return {}; }
    void internal_transition() override { throw std::logic_error("passive receiver internal"); }
    void external_transition(double, const std::vector<Input<int>>& bag) override {
        for (const auto& input : bag) value += input.value;
        if (*fail_) throw std::runtime_error("injected failure");
    }
    void confluent_transition(const std::vector<Input<int>>&) override { throw std::logic_error("passive receiver confluence"); }
    int value = 0;
private:
    std::shared_ptr<bool> fail_;
};

class CallbackSource final : public Atomic<int> {
public:
    explicit CallbackSource(std::function<void()> callback, double delay = 1) : callback_(std::move(callback)), delay_(delay) {}
    double time_advance() const override { return fired_ ? std::numeric_limits<double>::infinity() : delay_; }
    std::vector<PortValue<int>> output() const override { callback_(); return {{0, 17}}; }
    void internal_transition() override { callback_(); fired_ = true; }
    void external_transition(double, const std::vector<Input<int>>&) override { throw std::logic_error("source input"); }
    void confluent_transition(const std::vector<Input<int>>&) override { throw std::logic_error("source confluence"); }
private:
    std::function<void()> callback_;
    double delay_;
    bool fired_ = false;
};

void test_passthrough_order_and_validation() {
    CoupledBuilder<int> builder;
    builder.connect(builder.root(), builder.boundary(), 0, builder.boundary(), 9);
    builder.connect(builder.root(), builder.boundary(), 0, builder.boundary(), 9);
    builder.connect(builder.root(), builder.boundary(), 0, builder.boundary(), 1);
    auto graph = builder.compile();
    rejects<std::out_of_range>([&] { graph.inject(0, 42, 1); }, "unknown root input accepted");
    rejects<std::invalid_argument>([&] { graph.inject(std::numeric_limits<double>::quiet_NaN(), 0, 1); }, "NaN input accepted");
    graph.inject(1, 0, 7); graph.inject(1, 0, 8);
    const auto step = graph.simulator.step();
    require(step && step->time == 1 && step->transitioned.empty() && step->emissions.empty() &&
            step->boundary_injections.size() == 2, "boundary-only event changed the atomic schedule");
    const auto outputs = graph.boundary_outputs(*step);
    require(outputs.size() == 6, "passthrough lost duplicate declared paths");
    for (std::size_t i = 0; i < outputs.size(); ++i)
        require(outputs[i].port == (i % 3 == 2 ? 1U : 9U) && outputs[i].value == (i < 3 ? 7 : 8),
                "passthrough must preserve injection and path declaration order");
    rejects<std::invalid_argument>([&] { graph.inject(0.5, 0, 1); }, "past root input accepted");
    graph.disconnect(graph.root(), graph.boundary(), 0, graph.boundary(), 9);
    graph.inject(1, 0, 11);
    const auto next = graph.simulator.step();
    require(next && next->time == 1 && next->boundary_emissions.size() == 2 &&
            graph.boundary_outputs(*step).size() == 6, "disconnect changed history or removed multiple paths");
    require(!graph.simulator.step(), "empty graph did not passivate");
}

void test_edit_rejection_and_existing_clocks() {
    CoupledBuilder<int> builder;
    const auto group = builder.add_coupled(builder.root());
    const auto source = builder.add_atomic(group, std::make_unique<Source>());
    builder.connect(group, source, 0, builder.boundary(), 0);
    builder.connect(builder.root(), group, 0, builder.boundary(), 7);
    builder.connect(builder.root(), builder.boundary(), 1, builder.boundary(), 1);
    auto graph = builder.compile();
    graph.inject(0.25, 1, 9);
    (void)graph.simulator.step();
    graph.disconnect(graph.root(), group, 0, graph.boundary(), 7);
    graph.connect(graph.root(), group, 0, graph.boundary(), 8);
    rejects<std::invalid_argument>([&] {
        graph.add_atomic(group, std::make_unique<CallbackSource>([] {}, -1));
    }, "invalid new atomic schedule was accepted");
    const auto inserted = graph.add_atomic(group, std::make_unique<Source>());
    require(inserted.id == source.id + 1, "failed insertion consumed an atomic ID");
    graph.connect(group, inserted, 0, graph.boundary(), 0);
    const auto relay = graph.add_coupled(graph.root());
    graph.connect(relay, graph.boundary(), 0, graph.boundary(), 0);
    rejects<std::logic_error>([&] { graph.connect(graph.root(), relay, 0, relay, 0); },
                              "unreachable boundary cycle accepted");
    rejects<std::out_of_range>([&] { graph.connect(graph.root(), source, 0, graph.boundary(), 0); },
                              "cross-parent edge accepted");
    rejects<std::out_of_range>([&] { graph.disconnect(group, inserted, 0, graph.boundary(), 42); },
                              "missing edge removal accepted");
    rejects<std::invalid_argument>([&] { graph.remove(graph.root()); }, "root removal accepted");
    rejects<std::logic_error>([&] { graph.simulator.remove(source.id); }, "flat edit bypassed topology ownership");
    rejects<std::logic_error>([&] { graph.simulator.replace_routing({}); }, "flat routes bypassed topology ownership");
    graph.remove(relay);
    rejects<std::out_of_range>([&] { graph.add_coupled(relay); }, "removed parent accepted a new child");
    rejects<std::out_of_range>([&] { graph.remove(relay); }, "repeated subtree removal accepted");
    const auto replacement_group = graph.add_coupled(graph.root());
    require(replacement_group.id > relay.id, "retired group ID was reused");
    const auto steps = graph.simulator.run_until(2);
    require(steps.size() == 2 && steps[0].time == 1 && steps[1].time == 1.25 &&
            steps[0].boundary_emissions[0].port == 8 && steps[1].boundary_emissions[0].port == 8,
            "edit reset an existing clock, changed insertion origin, or failed to restore routes");
}

void test_root_retry_and_removed_direct_input() {
    auto fail = std::make_shared<bool>(true);
    CoupledBuilder<int> builder;
    const auto group = builder.add_coupled(builder.root());
    const auto sink = builder.add_atomic(group, std::make_unique<FaultReceiver>(fail));
    builder.connect(builder.root(), builder.boundary(), 0, group, 0);
    builder.connect(group, builder.boundary(), 0, sink, 0);
    builder.connect(builder.root(), builder.boundary(), 0, builder.boundary(), 9);
    auto graph = builder.compile();
    graph.inject(1, 0, 7);
    rejects<std::runtime_error>([&] { (void)graph.simulator.step_transactional(); }, "fault did not propagate");
    require(graph.simulator.now() == 0 && graph.simulator.next_time() == 1 &&
            dynamic_cast<const FaultReceiver&>(graph.simulator.model(sink.id)).value == 0,
            "root injection retry did not restore model and calendar");
    graph.disconnect(graph.root(), graph.boundary(), 0, graph.boundary(), 9);
    graph.connect(graph.root(), graph.boundary(), 0, graph.boundary(), 8);
    *fail = false;
    const auto step = graph.simulator.step_transactional();
    require(step && step->boundary_emissions.size() == 1 && step->boundary_emissions[0].port == 8 &&
            step->boundary_emissions[0].value == 7 &&
            dynamic_cast<const FaultReceiver&>(graph.simulator.model(sink.id)).value == 7,
            "retry did not reroute once using committed structure");
    graph.simulator.inject(2, sink.id, 0, 100);
    graph.inject(3, 0, 11);
    graph.remove(group);
    require(graph.simulator.next_time() == 3, "removed subtree retained a direct atomic injection");
    const auto boundary_only = graph.simulator.step_transactional();
    require(boundary_only && boundary_only->transitioned.empty() && boundary_only->boundary_emissions.size() == 1 &&
            boundary_only->boundary_emissions[0].value == 11, "subtree removal discarded a surviving root route");
    graph.inject(4, 0, 12);
    graph.disconnect(graph.root(), graph.boundary(), 0, graph.boundary(), 8);
    const auto disconnected = graph.simulator.step_transactional();
    require(disconnected && disconnected->time == 4 && disconnected->boundary_emissions.empty() &&
            disconnected->boundary_injections.size() == 1, "pending disconnected root input was not consumed");
    rejects<std::out_of_range>([&] { graph.inject(5, 0, 12); }, "new injection into disconnected root accepted");
}

void test_callback_edits_rejected() {
    CoupledBuilder<int> builder;
    auto graph = builder.compile();
    int attempts = 0;
    const auto source = graph.add_atomic(graph.root(), std::make_unique<CallbackSource>([&] {
        rejects<std::logic_error>([&] { graph.connect(graph.root(), graph.boundary(), 0, graph.boundary(), 1); },
                                  "hierarchy edit from output/transition callback was accepted");
        ++attempts;
    }));
    graph.connect(graph.root(), source, 0, graph.boundary(), 5);
    const auto step = graph.simulator.step();
    require(attempts == 2 && step && step->boundary_emissions.size() == 1 && step->boundary_emissions[0].port == 5,
            "callback edit guard changed routing");
    rejects<std::out_of_range>([&] { graph.inject(1, 0, 7); }, "rejected callback edit left a root route");
}

} // namespace

int main() {
    try {
        test_nested_equals_flat_and_root_ports();
        test_invalid_hierarchy_and_boundary_cycle();
        test_deep_hierarchy();
        test_passthrough_order_and_validation();
        test_edit_rejection_and_existing_clocks();
        test_root_retry_and_removed_direct_input();
        test_callback_edits_rejected();
        std::cout << "Coupled DEVS tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
