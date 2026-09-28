#include "harness.hpp"

#include <iostream>

using namespace conformance;
namespace {
Trace native_changes(double retirement) {
    Context context;
    native::Simulator<int> simulator;
    auto add = [&](std::size_t expected, Definition definition) {
        const auto id = simulator.add(std::make_unique<NativeAtomic>(expected, definition, context, simulator.now()));
        require(id == expected, "dynamic IDs must remain stable and never be reused");
    };
    add(0, {Behavior::generator, 1, 8});
    add(1, {Behavior::processor, 2, 1});
    add(2, {Behavior::sink, 1, 1});
    simulator.connect(0, 0, 1, 0);
    simulator.connect(1, 0, 2, 0);
    std::set<double> changed;
    Trace trace;
    while (std::isfinite(simulator.next_time())) {
        require(trace.size() < 100, "dynamic native transition limit");
        context.frame = Frame{}; context.frame.time = simulator.next_time();
        const auto step = simulator.step();
        for (const auto& output : step->emissions)
            context.frame.output.push_back({output.source, output.port, output.value});
        context.frame.canonicalize(); trace.push_back(std::move(context.frame));
        const double time = step->time;
        if (!changed.insert(time).second) continue;
        if (time == 1) {
            add(3, {Behavior::generator, 0.5, 3});
            simulator.connect(3, 0, 2, 1);
        }
        if (time == retirement) {
            simulator.remove(1);
            add(4, {Behavior::processor, 1, 1});
            simulator.connect(0, 0, 4, 0);
            simulator.connect(4, 0, 2, 0);
        }
        if (time == 4) {
            simulator.disconnect(0, 0, 4, 0);
            simulator.connect(0, 0, 2, 0);
        }
        if (time == 5) {
            simulator.disconnect(0, 0, 2, 0);
            simulator.connect(0, 0, 4, 0);
            simulator.remove(3);
        }
        if (time == 6) {
            add(5, {Behavior::echo, 0, 1});
            simulator.connect(5, 0, 2, 1);
            simulator.inject(7, 5, 0, 4);
        }
        if (time == 7) simulator.remove(5); // cancel an imminent zero-time output
    }
    return trace;
}

Trace reference_changes(double retirement) {
    Context context;
    auto graph = std::make_shared<adevs::Graph<int>>();
    std::vector<std::shared_ptr<ReferenceAtomic>> models;
    auto add = [&](Definition definition, double time) {
        auto model = std::make_shared<ReferenceAtomic>(models.size(), definition, context, time);
        graph->add_atomic(model);
        for (const auto& pin : model->in) graph->connect(pin, model);
        models.push_back(model);
    };
    add({Behavior::generator, 1, 8}, 0);
    add({Behavior::processor, 2, 1}, 0);
    add({Behavior::sink, 1, 1}, 0);
    auto connect = [&](std::size_t from, std::size_t to, std::uint32_t port) {
        graph->connect(models.at(from)->out[0], models.at(to)->in[port]);
    };
    auto disconnect = [&](std::size_t from, std::size_t to, std::uint32_t port) {
        graph->disconnect(models.at(from)->out[0], models.at(to)->in[port]);
    };
    connect(0, 1, 0); connect(1, 2, 0);
    adevs::Simulator<int> simulator(graph);
    std::set<double> changed;
    Trace trace;
    while (simulator.nextEventTime() < adevs_inf<double>()) {
        require(trace.size() < 100, "dynamic adevs transition limit");
        const double time = simulator.nextEventTime();
        context.frame = Frame{}; context.frame.time = time;
        if (time == 7 && !changed.contains(7)) {
            adevs::PinValue<int> input(models[5]->in[0], 4);
            simulator.injectInput(input);
        }
        simulator.computeNextOutput();
        // adevs queues these operations and commits them after this step's
        // transitions; native applies the same edits immediately after step().
        if (changed.insert(time).second) {
            if (time == 1) { add({Behavior::generator, 0.5, 3}, time); connect(3, 2, 1); }
            if (time == retirement) {
                disconnect(0, 1, 0); disconnect(1, 2, 0);
                graph->remove_atomic(models[1]);
                add({Behavior::processor, 1, 1}, time); connect(0, 4, 0); connect(4, 2, 0);
            }
            if (time == 4) { disconnect(0, 4, 0); connect(0, 2, 0); }
            if (time == 5) {
                disconnect(0, 2, 0); connect(0, 4, 0);
                disconnect(3, 2, 1); graph->remove_atomic(models[3]);
            }
            if (time == 6) { add({Behavior::echo, 0, 1}, time); connect(5, 2, 1); }
            if (time == 7) { disconnect(5, 2, 1); graph->remove_atomic(models[5]); }
        }
        require(simulator.computeNextState() == time, "dynamic adevs clock changed");
        context.frame.canonicalize(); trace.push_back(std::move(context.frame));
    }
    return trace;
}

void test_dynamic_differential() {
    for (double retirement : {2.0, 3.0}) {
        const auto actual = native_changes(retirement);
        equal(actual, reference_changes(retirement), "dynamic replacement at " + std::to_string(retirement));
        std::vector<std::pair<double, int>> added_source;
        std::size_t retired_outputs = 0;
        bool two_inputs = false;
        for (const auto& frame : actual) {
            for (const auto& event : frame.output) {
                if (event.component == 3) added_source.emplace_back(frame.time, event.value);
                if (event.component == 1) ++retired_outputs;
                require(event.component != 5, "removed zero-time output escaped cancellation");
            }
            for (const auto& event : frame.transitions) {
                if (event.component == 1) require(frame.time <= retirement, "removed component transitioned");
                if (frame.time == 5 && event.component == 2 && event.input == Bag{{0, 4}, {0, 5}})
                    two_inputs = true;
            }
        }
        require(added_source == std::vector<std::pair<double, int>>{{1.5, 1}, {2, 2}, {2.5, 3}} &&
                retired_outputs == (retirement == 3 ? 1U : 0U) && two_inputs,
                "dynamic hand oracle: insertion clock, retirement boundary, or reconnection failed");
    }
}

void test_disconnect_multiplicity_and_cancelled_injections() {
    Context context;
    native::Simulator<int> simulator;
    const auto generator = simulator.add(std::make_unique<NativeAtomic>(0, Definition{Behavior::generator, 1, 3}, context));
    const auto sink = simulator.add(std::make_unique<NativeAtomic>(1, Definition{Behavior::sink, 1, 1}, context));
    simulator.connect(generator, 0, sink, 0);
    simulator.connect(generator, 0, sink, 0);
    simulator.disconnect(generator, 0, sink, 0);
    context.frame.time = 1;
    (void)simulator.step();
    require(context.frame.transitions.back().input == Bag{{0, 1}}, "disconnect must remove one path");
    simulator.disconnect(generator, 0, sink, 0);
    bool rejected = false;
    try { simulator.disconnect(generator, 0, sink, 0); }
    catch (const std::out_of_range&) { rejected = true; }
    require(rejected, "missing coupling removal must be diagnosed");
    simulator.inject(2.5, sink, 0, 100);
    simulator.remove(sink);
    rejected = false;
    try { simulator.disconnect(generator, 0, sink, 0); }
    catch (const std::out_of_range&) { rejected = true; }
    require(rejected, "removed endpoint must reject coupling mutation");
    const auto replacement = simulator.add(std::make_unique<NativeAtomic>(2, Definition{Behavior::sink, 1, 1}, context, 1));
    require(replacement == 2, "replacement reused a retired ID");
    simulator.connect(generator, 0, replacement, 0);
    while (std::isfinite(simulator.next_time())) {
        context.frame = Frame{}; context.frame.time = simulator.next_time();
        require(context.frame.time != 2.5, "retired target's pending injection was not discarded");
        (void)simulator.step();
    }
}
class OrderedEmitter final : public native::Atomic<int> {
public:
    explicit OrderedEmitter(std::vector<native::PortValue<int>> values) : values_(std::move(values)) {}
    double time_advance() const override { return remaining_ ? 1 : infinity; }
    std::vector<native::PortValue<int>> output() const override { return values_; }
    void internal_transition() override { --remaining_; }
    void external_transition(double, const std::vector<native::Input<int>>&) override {
        throw std::logic_error("emitter cannot receive input");
    }
    void confluent_transition(const std::vector<native::Input<int>>&) override {
        throw std::logic_error("emitter cannot receive confluent input");
    }
private:
    std::vector<native::PortValue<int>> values_;
    int remaining_ = 2;
};

class OrderedReceiver final : public native::Atomic<int> {
public:
    explicit OrderedReceiver(native::Simulator<int>& simulator) : simulator_(simulator) {}
    double time_advance() const override { return infinity; }
    std::vector<native::PortValue<int>> output() const override { return {}; }
    void internal_transition() override { throw std::logic_error("passive receiver became imminent"); }
    void external_transition(double, const std::vector<native::Input<int>>& bag) override {
        std::vector<std::tuple<std::size_t, std::uint32_t, int>> values;
        for (const auto& item : bag) values.emplace_back(item.source, item.port, item.value);
        received.push_back(std::move(values));
        bool rejected = false;
        try { simulator_.disconnect(0, 7, 2, 2); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected, "disconnect during a transition must be rejected");
    }
    void confluent_transition(const std::vector<native::Input<int>>&) override {
        throw std::logic_error("passive receiver became confluent");
    }
    std::vector<std::vector<std::tuple<std::size_t, std::uint32_t, int>>> received;
private:
    native::Simulator<int>& simulator_;
};

void test_native_order_and_mutation_guard() {
    native::Simulator<int> simulator;
    simulator.add(std::make_unique<OrderedEmitter>(std::vector<native::PortValue<int>>{{7, 11}, {3, 12}}));
    simulator.add(std::make_unique<OrderedEmitter>(std::vector<native::PortValue<int>>{{0, 21}}));
    auto receiver = std::make_unique<OrderedReceiver>(simulator);
    const auto* observed = receiver.get();
    simulator.add(std::move(receiver));
    // Reverse declaration order must not change destination-port ordering.
    simulator.connect(1, 0, 2, 0);
    simulator.connect(0, 7, 2, 9);
    simulator.connect(0, 3, 2, 1);
    simulator.connect(0, 7, 2, 2);
    simulator.inject(1, 2, 8, 90);
    simulator.inject(1, 2, 8, 91);
    for (double time : {1.0, 2.0}) {
        const auto step = simulator.step();
        require(step && step->time == time && step->emissions.size() == 3,
                "native ordering fixture schedule changed");
        std::vector<Event> output;
        for (const auto& item : step->emissions) output.push_back({item.source, item.port, item.value});
        require(output == std::vector<Event>{{0, 7, 11}, {0, 3, 12}, {1, 0, 21}},
                "native output order must preserve component IDs and atomic output-vector order");
    }
    using Received = std::vector<std::tuple<std::size_t, std::uint32_t, int>>;
    const Received routed{{0, 2, 11}, {0, 9, 11}, {0, 1, 12}, {1, 0, 21}};
    Received injected{{native::Simulator<int>::external_source, 8, 90},
                      {native::Simulator<int>::external_source, 8, 91}};
    injected.insert(injected.end(), routed.begin(), routed.end());
    require(observed->received == std::vector<Received>{injected, routed},
            "input order or transition mutation guard changed routing");
}
} // namespace

int main() {
    try {
        test_dynamic_differential();
        test_disconnect_multiplicity_and_cancelled_injections();
        test_native_order_and_mutation_guard();
        std::cout << "dynamic boundary changes agree with pinned adevs; duplicate-path removal and injection cancellation pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
