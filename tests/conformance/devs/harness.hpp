#pragma once

#include "ankurafathom/devs/coupled.hpp"
#include "adevs/simulator.h"

#include <array>
#include <compare>
#include <deque>
#include <list>
#include <string>

namespace conformance {
namespace native = ankurafathom::devs;
inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
constexpr double infinity = std::numeric_limits<double>::infinity();
using Bag = std::vector<std::pair<std::uint32_t, int>>;
struct Event {
    std::size_t component;
    std::uint32_t port;
    int value;
    auto operator<=>(const Event&) const = default;
};
struct Transition {
    std::size_t component;
    char kind;
    double elapsed;
    Bag input;
    std::vector<int> state;
    double advance;
    auto operator<=>(const Transition&) const = default;
};
struct Frame {
    double time = 0;
    std::vector<Event> output;
    std::vector<Transition> transitions;
    Bag boundary;
    void canonicalize() {
        std::sort(output.begin(), output.end());
        std::sort(transitions.begin(), transitions.end());
        std::sort(boundary.begin(), boundary.end());
    }
    bool operator==(const Frame&) const = default;
};
using Trace = std::vector<Frame>;
struct Context { Frame frame; };
enum class Behavior { devstone, generator, processor, sink, echo, internal_first, external_first };
struct Definition {
    Behavior behavior = Behavior::devstone;
    double period = 1;
    int limit = 1;
};

// The same mathematical atomic is wrapped for two independent schedulers.
// This is deliberately shared model behavior, not shared event routing/execution.
class State {
public:
    explicit State(Definition definition) : definition_(definition), remaining_(definition.period) {}
    double advance() const {
        switch (definition_.behavior) {
        case Behavior::generator: return count_ < definition_.limit ? definition_.period : infinity;
        case Behavior::devstone: return count_ > 0 ? definition_.period : infinity;
        case Behavior::processor: return queue_.empty() ? infinity : remaining_;
        case Behavior::sink: return infinity;
        case Behavior::echo: return queue_.empty() ? infinity : 0;
        default: return stage_ == 0 ? remaining_ : stage_ == 1 ? 0 : infinity;
        }
    }
    Bag output() const {
        switch (definition_.behavior) {
        case Behavior::generator: return {{0, count_ + 1}};
        case Behavior::devstone: return {{0, 1}};
        case Behavior::processor: return {{0, queue_.front()}};
        case Behavior::sink: return {};
        case Behavior::echo: {
            Bag result;
            for (int value : queue_) result.emplace_back(0, value - 1);
            return result;
        }
        default: return {{0, probe_}};
        }
    }
    void internal() {
        switch (definition_.behavior) {
        case Behavior::generator: ++count_; break;
        case Behavior::devstone: require(count_ > 0, "negative DEVStone queue"); --count_; break;
        case Behavior::processor: queue_.pop_front(); remaining_ = definition_.period; break;
        case Behavior::sink: throw std::logic_error("passive sink became imminent");
        case Behavior::echo: queue_.clear(); break;
        default: if (stage_ == 0) probe_ *= 2; ++stage_; break;
        }
    }
    void external(double elapsed, const Bag& bag) {
        switch (definition_.behavior) {
        case Behavior::generator: throw std::logic_error("generator received input");
        case Behavior::devstone: count_ += static_cast<int>(bag.size()); break;
        case Behavior::processor:
            remaining_ = queue_.empty() ? definition_.period : remaining_ - elapsed;
            for (const auto& [port, value] : bag) { (void)port; queue_.push_back(value); }
            break;
        case Behavior::sink: count_ += static_cast<int>(bag.size()); break;
        case Behavior::echo:
            for (const auto& [port, value] : bag) { (void)port; if (value > 0) queue_.push_back(value); }
            std::sort(queue_.begin(), queue_.end());
            break;
        default:
            if (stage_ == 0) remaining_ -= elapsed;
            for (const auto& [port, value] : bag) { (void)port; probe_ += value; }
            break;
        }
    }
    void confluent(const Bag& bag) {
        if (definition_.behavior == Behavior::external_first) { external(0, bag); internal(); }
        else { internal(); external(0, bag); }
    }
    std::vector<int> snapshot() const {
        std::vector<int> result{count_, probe_, stage_};
        result.insert(result.end(), queue_.begin(), queue_.end());
        return result;
    }
private:
    Definition definition_;
    double remaining_;
    int count_ = 0;
    int probe_ = 10;
    int stage_ = 0;
    std::deque<int> queue_;
};

class NativeAtomic final : public native::Atomic<int> {
public:
    NativeAtomic(std::size_t id, Definition definition, Context& context, double origin = 0)
        : id_(id), state_(definition), context_(&context), last_(origin) {}
    std::unique_ptr<native::Atomic<int>> clone() const override { return std::make_unique<NativeAtomic>(*this); }
    double time_advance() const override { return state_.advance(); }
    std::vector<native::PortValue<int>> output() const override {
        std::vector<native::PortValue<int>> result;
        for (const auto& [port, value] : state_.output()) result.push_back({port, value});
        return result;
    }
    void internal_transition() override { state_.internal(); record('I', {}); }
    void external_transition(double elapsed, const std::vector<native::Input<int>>& bag) override {
        const auto values = unpack(bag);
        require(elapsed == context_->frame.time - last_, "native elapsed clock mismatch");
        state_.external(elapsed, values);
        record('E', values);
    }
    void confluent_transition(const std::vector<native::Input<int>>& bag) override {
        const auto values = unpack(bag);
        state_.confluent(values);
        record('C', values);
    }
private:
    static Bag unpack(const std::vector<native::Input<int>>& bag) {
        Bag values;
        for (const auto& item : bag) values.emplace_back(item.port, item.value);
        std::sort(values.begin(), values.end()); // PDEVS bags are unordered in these fixtures.
        return values;
    }
    void record(char kind, Bag bag) {
        context_->frame.transitions.push_back({id_, kind, context_->frame.time - last_,
                                               std::move(bag), state_.snapshot(), state_.advance()});
        last_ = context_->frame.time;
    }
    std::size_t id_;
    State state_;
    Context* context_;
    double last_;
};

class ReferenceAtomic final : public adevs::Atomic<int> {
public:
    ReferenceAtomic(std::size_t id, Definition definition, Context& context, double origin = 0)
        : id_(id), state_(definition), context_(&context), last_(origin) {}
    double ta() override { const double t = state_.advance(); return std::isinf(t) ? adevs_inf<double>() : t; }
    void output_func(std::list<adevs::PinValue<int>>& bag) override {
        for (const auto& [port, value] : state_.output()) {
            bag.emplace_back(out.at(port), value);
            context_->frame.output.push_back({id_, port, value});
        }
    }
    void delta_int() override { state_.internal(); record('I', {}); }
    void delta_ext(double elapsed, const std::list<adevs::PinValue<int>>& bag) override {
        const auto values = unpack(bag);
        require(elapsed == context_->frame.time - last_, "adevs elapsed clock mismatch");
        state_.external(elapsed, values);
        record('E', values);
    }
    void delta_conf(const std::list<adevs::PinValue<int>>& bag) override {
        const auto values = unpack(bag);
        state_.confluent(values);
        record('C', values);
    }
    std::array<adevs::pin_t, 2> in, out;
private:
    Bag unpack(const std::list<adevs::PinValue<int>>& bag) const {
        Bag values;
        for (const auto& item : bag) {
            const auto found = std::find(in.begin(), in.end(), item.pin);
            require(found != in.end(), "unmapped reference input pin");
            values.emplace_back(static_cast<std::uint32_t>(found - in.begin()), item.value);
        }
        std::sort(values.begin(), values.end());
        return values;
    }
    void record(char kind, Bag bag) {
        context_->frame.transitions.push_back({id_, kind, context_->frame.time - last_,
                                               std::move(bag), state_.snapshot(), state_.advance()});
        last_ = context_->frame.time;
    }
    std::size_t id_;
    State state_;
    Context* context_;
    double last_;
};

class BoundarySink final : public adevs::Atomic<int> {
public:
    BoundarySink(std::uint32_t port, Context& context) : port_(port), context_(&context) {}
    double ta() override { return adevs_inf<double>(); }
    void output_func(std::list<adevs::PinValue<int>>&) override {}
    void delta_int() override { throw std::logic_error("boundary sink internal"); }
    void delta_ext(double, const std::list<adevs::PinValue<int>>& bag) override {
        for (const auto& item : bag) context_->frame.boundary.emplace_back(port_, item.value);
    }
    void delta_conf(const std::list<adevs::PinValue<int>>&) override { throw std::logic_error("boundary confluence"); }
private:
    std::uint32_t port_;
    Context* context_;
};

using Component = native::Component;
constexpr Component boundary = native::CoupledBuilder<int>::boundary();
constexpr Component root = native::CoupledBuilder<int>::root();
struct Link { Component parent, source; std::uint32_t out; Component destination; std::uint32_t in; };
struct Input { double time; std::uint32_t port; int value; };
struct Fixture {
    std::string name;
    std::vector<Component> parents{root};
    std::vector<std::pair<Component, Definition>> atomics;
    std::vector<Link> links;
    std::vector<Input> inputs;
    Component group(Component parent) {
        parents.push_back(parent);
        return {native::ComponentKind::coupled, parents.size() - 1};
    }
    Component atomic(Component parent, Definition definition = {}) {
        atomics.emplace_back(parent, definition);
        return {native::ComponentKind::atomic, atomics.size() - 1};
    }
    void connect(Component parent, Component source, std::uint32_t out, Component destination, std::uint32_t in) {
        links.push_back({parent, source, out, destination, in});
    }
};

inline Trace run_native(const Fixture& fixture, bool nested) {
    Context context;
    native::CompiledCoupling<int> compiled;
    if (nested) {
        native::CoupledBuilder<int> builder;
        for (std::size_t i = 1; i < fixture.parents.size(); ++i) (void)builder.add_coupled(fixture.parents[i]);
        for (std::size_t i = 0; i < fixture.atomics.size(); ++i)
            (void)builder.add_atomic(fixture.atomics[i].first,
                std::make_unique<NativeAtomic>(i, fixture.atomics[i].second, context));
        for (const auto& link : fixture.links)
            builder.connect(link.parent, link.source, link.out, link.destination, link.in);
        compiled = builder.compile();
    }
    // Independently resolve boundary routes breadth-first for the explicitly flat run.
    if (!nested) {
        native::CompiledCoupling<int> flat;
        native::Routing routing;
        for (std::size_t i = 0; i < fixture.atomics.size(); ++i)
            (void)flat.simulator.add(std::make_unique<NativeAtomic>(i, fixture.atomics[i].second, context));
        using Node = std::tuple<char, std::size_t, std::uint32_t>;
        std::map<Node, std::vector<Node>> edges;
        for (const auto& link : fixture.links) {
            const auto source = link.source.kind == native::ComponentKind::boundary
                ? Node{'i', link.parent.id, link.out} : link.source.kind == native::ComponentKind::atomic
                ? Node{'a', link.source.id, link.out} : Node{'o', link.source.id, link.out};
            const auto destination = link.destination.kind == native::ComponentKind::boundary
                ? Node{'o', link.parent.id, link.in} : link.destination.kind == native::ComponentKind::atomic
                ? Node{'b', link.destination.id, link.in} : Node{'i', link.destination.id, link.in};
            edges[source].push_back(destination);
        }
        for (const auto& [source, _] : edges) {
            const auto [kind, id, port] = source;
            if (kind != 'a' && !(kind == 'i' && id == 0)) continue;
            std::deque<Node> pending{source};
            std::size_t visits = 0;
            while (!pending.empty()) {
                require(++visits < 1000000, "flat fixture routing cycle/limit");
                const auto node = pending.front(); pending.pop_front();
                const auto [target_kind, target_id, target_port] = node;
                if (target_kind == 'b') {
                    if (kind == 'a') routing.internal.push_back({id, port, target_id, target_port});
                    else routing.inputs[port].push_back({target_id, target_port});
                } else if (target_kind == 'o' && target_id == 0) {
                    if (kind == 'a') routing.outputs[{id, port}].push_back(target_port);
                    else routing.passthrough[port].push_back(target_port);
                } else if (const auto found = edges.find(node); found != edges.end()) {
                    pending.insert(pending.end(), found->second.begin(), found->second.end());
                }
            }
        }
        flat.simulator.replace_routing(std::move(routing));
        compiled = std::move(flat);
    }
    for (const auto& input : fixture.inputs) compiled.inject(input.time, input.port, input.value);
    Trace trace;
    while (std::isfinite(compiled.simulator.next_time())) {
        require(trace.size() < 100000, "native fixture transition limit");
        context.frame = Frame{}; context.frame.time = compiled.simulator.next_time();
        const auto result = compiled.simulator.step();
        for (const auto& event : result->emissions)
            context.frame.output.push_back({event.source, event.port, event.value});
        for (const auto& event : compiled.boundary_outputs(*result))
            context.frame.boundary.emplace_back(event.port, event.value);
        context.frame.canonicalize();
        trace.push_back(std::move(context.frame));
    }
    return trace;
}

inline Trace run_reference(const Fixture& fixture) {
    Context context;
    auto graph = std::make_shared<adevs::Graph<int>>();
    std::vector<std::shared_ptr<ReferenceAtomic>> atomics;
    for (std::size_t i = 0; i < fixture.atomics.size(); ++i) {
        auto model = std::make_shared<ReferenceAtomic>(i, fixture.atomics[i].second, context);
        graph->add_atomic(model);
        for (const auto& pin : model->in) graph->connect(pin, model);
        atomics.push_back(model);
    }
    struct Pins { std::array<adevs::pin_t, 2> in, out; };
    std::vector<Pins> groups(fixture.parents.size());
    for (const auto& link : fixture.links) {
        const auto source = link.source.kind == native::ComponentKind::boundary
            ? groups.at(link.parent.id).in.at(link.out) : link.source.kind == native::ComponentKind::atomic
            ? atomics.at(link.source.id)->out.at(link.out) : groups.at(link.source.id).out.at(link.out);
        const auto destination = link.destination.kind == native::ComponentKind::boundary
            ? groups.at(link.parent.id).out.at(link.in) : link.destination.kind == native::ComponentKind::atomic
            ? atomics.at(link.destination.id)->in.at(link.in) : groups.at(link.destination.id).in.at(link.in);
        graph->connect(source, destination);
    }
    for (std::uint32_t port = 0; port < 2; ++port) {
        auto sink = std::make_shared<BoundarySink>(port, context);
        graph->add_atomic(sink); graph->connect(groups[0].out[port], sink);
    }
    adevs::Simulator<int> simulator(graph);
    Trace trace;
    std::size_t next = 0;
    while (next < fixture.inputs.size() || simulator.nextEventTime() < adevs_inf<double>()) {
        require(trace.size() < 100000, "adevs fixture transition limit");
        const double time = std::min(next < fixture.inputs.size() ? fixture.inputs[next].time : infinity,
                                     simulator.nextEventTime());
        while (next < fixture.inputs.size() && fixture.inputs[next].time == time) {
            const auto& input = fixture.inputs[next++];
            adevs::PinValue<int> event(groups[0].in.at(input.port), input.value);
            simulator.injectInput(event);
        }
        simulator.setNextTime(time);
        context.frame = Frame{}; context.frame.time = time;
        require(simulator.execNextEvent() == time, "adevs changed physical time");
        context.frame.canonicalize(); trace.push_back(std::move(context.frame));
    }
    return trace;
}

inline void equal(const Trace& actual, const Trace& expected, const std::string& label) {
    require(actual.size() == expected.size(), label + ": event-step count differs (" +
            std::to_string(actual.size()) + " vs " + std::to_string(expected.size()) + ")");
    for (std::size_t i = 0; i < actual.size(); ++i)
        require(actual[i] == expected[i], label + ": trace mismatch at step " + std::to_string(i) +
                ", time " + std::to_string(expected[i].time));
}
} // namespace conformance
