#include "harness.hpp"

#include <iostream>

using namespace conformance;
namespace {
void passthrough_cases() {
    for (int variant = 0; variant < 3; ++variant) {
        Fixture fixture;
        fixture.name = "root_passthrough_" + std::to_string(variant);
        fixture.connect(root, boundary, 0, boundary, 1);
        if (variant > 0) {
            const auto a = fixture.group(root), b = fixture.group(a);
            fixture.connect(root, boundary, 0, a, 0);
            fixture.connect(a, boundary, 0, b, 0);
            fixture.connect(b, boundary, 0, boundary, 1);
            fixture.connect(a, b, 1, boundary, 1);
            fixture.connect(root, a, 1, boundary, 1);
        }
        if (variant == 2) {
            const auto model = fixture.atomic(root, {Behavior::internal_first, 1, 1});
            fixture.connect(root, boundary, 0, model, 0);
            fixture.connect(root, model, 0, boundary, 0);
            fixture.inputs = {{1, 0, 3}};
        } else fixture.inputs = {{0, 0, 4}, {0, 0, 7}, {0.5, 0, 9}};
        const auto trace = run_native(fixture, true);
        equal(trace, run_native(fixture, false), fixture.name + " flat");
        equal(trace, run_reference(fixture), fixture.name + " adevs");
        if (variant == 2) {
            require(trace.size() == 2 && trace[0].boundary == Bag{{0, 10}, {1, 3}, {1, 3}} &&
                    trace[1].boundary == Bag{{0, 23}} && trace[0].transitions[0].kind == 'C',
                    "passthrough changed confluent timing");
        } else {
            require(trace.size() == 2 && trace[0].time == 0 && trace[1].time == 0.5 &&
                    trace[0].transitions.empty() && trace[1].transitions.empty() &&
                    trace[0].boundary.size() == (variant ? 4U : 2U),
                    "empty hierarchy passthrough added atomic transitions or lost paths");
        }
    }
}

struct NativeGraph {
    Context& context;
    native::CompiledCoupling<int> graph;
    explicit NativeGraph(Context& context) : context(context) {
        native::CoupledBuilder<int> builder;
        graph = builder.compile();
    }
    Component group(Component parent) { return graph.add_coupled(parent); }
    Component atomic(Component parent, Definition definition, double origin) {
        return graph.add_atomic(parent, std::make_unique<NativeAtomic>(next_id++, definition, context, origin));
    }
    void connect(Component p, Component a, std::uint32_t out, Component b, std::uint32_t in) {
        graph.connect(p, a, out, b, in);
    }
    void disconnect(Component p, Component a, std::uint32_t out, Component b, std::uint32_t in) {
        graph.disconnect(p, a, out, b, in);
    }
    void remove(Component component) { graph.remove(component); }
    void sync() {}
    std::size_t next_id = 0;
};

// The reference keeps the hierarchy as pin edges; it never calls the native
// flattener. Parent metadata is only used to enumerate a removed subtree.
struct ReferenceGraph {
    struct Group { std::size_t parent; bool active = true; std::array<adevs::pin_t, 2> in, out; };
    struct Model { std::size_t parent; bool active; std::shared_ptr<ReferenceAtomic> atomic; };
    struct Link { Component parent, from, to; std::uint32_t out, in; };
    Context& context;
    std::shared_ptr<adevs::Graph<int>> graph = std::make_shared<adevs::Graph<int>>();
    std::vector<Group> groups;
    std::vector<Model> models;
    std::vector<Link> links;
    explicit ReferenceGraph(Context& context) : context(context) {
        groups.push_back(Group{0, true, {}, {}});
        for (std::uint32_t port = 0; port < 2; ++port) {
            auto sink = std::make_shared<BoundarySink>(port, context);
            graph->add_atomic(sink); graph->connect(groups[0].out[port], sink);
        }
    }
    Component group(Component parent) {
        groups.push_back(Group{parent.id, true, {}, {}});
        return {native::ComponentKind::coupled, groups.size() - 1};
    }
    Component atomic(Component parent, Definition definition, double origin) {
        auto model = std::make_shared<ReferenceAtomic>(models.size(), definition, context, origin);
        graph->add_atomic(model);
        for (const auto& pin : model->in) graph->connect(pin, model);
        models.push_back({parent.id, true, model});
        return {native::ComponentKind::atomic, models.size() - 1};
    }
    auto pins(const Link& link) const {
        const auto from = link.from.kind == native::ComponentKind::boundary ? groups[link.parent.id].in[link.out]
            : link.from.kind == native::ComponentKind::atomic ? models[link.from.id].atomic->out[link.out]
            : groups[link.from.id].out[link.out];
        const auto to = link.to.kind == native::ComponentKind::boundary ? groups[link.parent.id].out[link.in]
            : link.to.kind == native::ComponentKind::atomic ? models[link.to.id].atomic->in[link.in]
            : groups[link.to.id].in[link.in];
        return std::pair{from, to};
    }
    void connect(Component p, Component a, std::uint32_t out, Component b, std::uint32_t in) {
        Link link{p, a, b, out, in};
        const auto [from, to] = pins(link); graph->connect(from, to); links.push_back(link);
    }
    void erase(std::size_t index) {
        const auto [from, to] = pins(links[index]); graph->disconnect(from, to);
        links.erase(links.begin() + static_cast<std::ptrdiff_t>(index));
    }
    void disconnect(Component p, Component a, std::uint32_t out, Component b, std::uint32_t in) {
        for (std::size_t i = 0; i < links.size(); ++i) {
            const auto& link = links[i];
            if (link.parent.id == p.id && link.from.kind == a.kind && link.from.id == a.id &&
                link.to.kind == b.kind && link.to.id == b.id && link.out == out && link.in == in) {
                erase(i); return;
            }
        }
        throw std::logic_error("reference edge missing");
    }
    void remove(Component component) {
        if (component.kind == native::ComponentKind::coupled) groups[component.id].active = false;
        for (std::size_t i = 1; i < groups.size(); ++i)
            if (!groups[groups[i].parent].active) groups[i].active = false;
        for (std::size_t i = 0; i < models.size(); ++i) {
            auto& model = models[i];
            if (model.active && (!groups[model.parent].active ||
                (component.kind == native::ComponentKind::atomic && component.id == i))) {
                model.active = false; graph->remove_atomic(model.atomic);
            }
        }
        auto active = [&](Component c) {
            return c.kind == native::ComponentKind::boundary || (c.kind == native::ComponentKind::atomic
                ? models[c.id].active : groups[c.id].active);
        };
        for (std::size_t i = links.size(); i > 0; --i) {
            const auto& link = links[i - 1];
            if (!active(link.parent) || !active(link.from) || !active(link.to)) erase(i - 1);
        }
    }
};

struct FlatGraph {
    Context& context;
    ReferenceGraph metadata;
    native::CompiledCoupling<int> graph;
    explicit FlatGraph(Context& context) : context(context), metadata(context) {}
    Component group(Component parent) { return metadata.group(parent); }
    Component atomic(Component parent, Definition definition, double origin) {
        const auto component = metadata.atomic(parent, definition, origin);
        const auto id = graph.simulator.add(std::make_unique<NativeAtomic>(component.id, definition, context, origin));
        require(id == component.id, "flat insertion ID changed");
        return component;
    }
    void connect(Component p, Component a, std::uint32_t out, Component b, std::uint32_t in) {
        metadata.connect(p, a, out, b, in);
    }
    void disconnect(Component p, Component a, std::uint32_t out, Component b, std::uint32_t in) {
        metadata.disconnect(p, a, out, b, in);
    }
    void remove(Component component) {
        std::vector<std::size_t> active;
        for (std::size_t id = 0; id < metadata.models.size(); ++id)
            if (metadata.models[id].active) active.push_back(id);
        metadata.remove(component);
        for (const auto id : active)
            if (!metadata.models[id].active) graph.simulator.remove(id);
    }
    void sync() {
        // Independently expand reference pin edges into direct native routes.
        // The reference atomics here are metadata only and are never scheduled.
        std::map<adevs::pin_t, std::vector<adevs::pin_t>> edges;
        std::map<adevs::pin_t, std::pair<std::size_t, std::uint32_t>> inputs;
        for (const auto& link : metadata.links) {
            const auto [from, to] = metadata.pins(link); edges[from].push_back(to);
        }
        for (std::size_t id = 0; id < metadata.models.size(); ++id)
            if (metadata.models[id].active)
                for (std::uint32_t port = 0; port < 2; ++port)
                    inputs.emplace(metadata.models[id].atomic->in[port], std::pair{id, port});
        native::Routing routing;
        auto expand = [&](adevs::pin_t source, bool root_input, std::size_t id, std::uint32_t port) {
            std::deque<adevs::pin_t> pending{source};
            std::size_t visits = 0;
            while (!pending.empty()) {
                require(++visits < 10000, "flat dynamic route expansion limit");
                const auto pin = pending.front(); pending.pop_front();
                if (const auto found = inputs.find(pin); found != inputs.end()) {
                    if (root_input) routing.inputs[port].push_back(found->second);
                    else routing.internal.push_back({id, port, found->second.first, found->second.second});
                } else if (pin == metadata.groups[0].out[0] || pin == metadata.groups[0].out[1]) {
                    const std::uint32_t target = pin == metadata.groups[0].out[0] ? 0 : 1;
                    if (root_input) routing.passthrough[port].push_back(target);
                    else routing.outputs[{id, port}].push_back(target);
                } else if (const auto link = edges.find(pin); link != edges.end()) {
                    pending.insert(pending.end(), link->second.begin(), link->second.end());
                }
            }
        };
        for (std::uint32_t port = 0; port < 2; ++port) expand(metadata.groups[0].in[port], true, 0, port);
        for (std::size_t id = 0; id < metadata.models.size(); ++id)
            if (metadata.models[id].active)
                for (std::uint32_t port = 0; port < 2; ++port)
                    expand(metadata.models[id].atomic->out[port], false, id, port);
        graph.simulator.replace_routing(std::move(routing));
    }
};

struct Scenario {
    Component a, inner, other, generator, processor, old_sink, new_sink, replacement;
    template<class Graph> void build(Graph& graph) {
        a = graph.group(root); inner = graph.group(a); other = graph.group(root);
        generator = graph.atomic(root, {Behavior::generator, 1, 7}, 0);
        processor = graph.atomic(inner, {Behavior::processor, 2, 1}, 0);
        old_sink = graph.atomic(other, {Behavior::sink, 1, 1}, 0);
        graph.connect(root, generator, 0, a, 0);
        graph.connect(a, boundary, 0, inner, 0);
        graph.connect(inner, boundary, 0, processor, 0);
        graph.connect(inner, processor, 0, boundary, 0);
        graph.connect(a, inner, 0, boundary, 0);
        graph.connect(root, a, 0, other, 0);
        graph.connect(other, boundary, 0, old_sink, 0);
        graph.connect(root, a, 0, boundary, 0);
        graph.connect(root, boundary, 0, a, 0);
        graph.connect(root, boundary, 0, boundary, 1);
    }
    template<class Graph> void change(Graph& graph, double time) {
        if (time == 1) {
            const auto child = graph.group(other);
            new_sink = graph.atomic(child, {Behavior::sink, 1, 1}, time);
            graph.connect(other, boundary, 0, child, 0);
            graph.connect(child, boundary, 0, new_sink, 0);
            graph.remove(old_sink);
        }
        if (time == 2) {
            graph.remove(inner); // Busy processor due at three must be cancelled.
            const auto branch = graph.group(a), child = graph.group(branch);
            replacement = graph.atomic(child, {Behavior::processor, 1, 1}, time);
            graph.connect(a, boundary, 0, branch, 0);
            graph.connect(branch, boundary, 0, child, 0);
            graph.connect(child, boundary, 0, replacement, 0);
            graph.connect(child, replacement, 0, boundary, 0);
            graph.connect(branch, child, 0, boundary, 0);
            graph.connect(a, branch, 0, boundary, 0);
        }
        if (time == 3.5) {
            graph.disconnect(root, a, 0, boundary, 0);
            graph.connect(root, a, 0, boundary, 1);
            graph.disconnect(root, boundary, 0, boundary, 1);
            graph.connect(root, boundary, 0, boundary, 0);
        }
        if (time == 4) {
            graph.disconnect(root, boundary, 0, a, 0);
            graph.connect(root, boundary, 0, other, 0);
        }
        if (time == 5) {
            graph.remove(a);
            const auto relay = graph.group(root);
            graph.connect(relay, boundary, 0, boundary, 0);
            graph.connect(root, generator, 0, relay, 0);
            graph.connect(root, boundary, 0, relay, 0);
            graph.connect(root, relay, 0, boundary, 0);
            graph.connect(root, relay, 0, other, 0);
        }
        if (time == 6) graph.remove(other);
    }
};

template<class Graph> Trace native_dynamic() {
    Context context;
    Graph graph(context);
    Scenario scenario; scenario.build(graph);
    graph.sync();
    for (const auto& [time, value] : std::vector<std::pair<double, int>>{{2.5, 40}, {4.5, 50}, {6.5, 60}})
        graph.graph.inject(time, 0, value);
    Trace trace;
    std::set<double> changed;
    std::optional<native::StepResult<int>> historical;
    while (std::isfinite(graph.graph.simulator.next_time())) {
        require(trace.size() < 100, "dynamic hierarchy failed to drain");
        context.frame = Frame{}; context.frame.time = graph.graph.simulator.next_time();
        const auto step = graph.graph.simulator.step();
        for (const auto& output : step->emissions) context.frame.output.push_back({output.source, output.port, output.value});
        for (const auto& output : graph.graph.boundary_outputs(*step)) context.frame.boundary.emplace_back(output.port, output.value);
        context.frame.canonicalize(); trace.push_back(std::move(context.frame));
        if (step->time == 3.5) historical = step;
        if (changed.insert(step->time).second) { scenario.change(graph, step->time); graph.sync(); }
    }
    require(historical.has_value(), "replacement processor schedule changed");
    const auto output = graph.graph.boundary_outputs(*historical);
    require(output.size() == 1 && output[0].port == 0 && output[0].value == 40,
            "rewiring changed a historical boundary output");
    return trace;
}

Trace reference_dynamic() {
    Context context;
    ReferenceGraph graph(context);
    Scenario scenario; scenario.build(graph);
    adevs::Simulator<int> simulator(graph.graph);
    const std::vector<std::pair<double, int>> inputs{{2.5, 40}, {4.5, 50}, {6.5, 60}};
    std::size_t next = 0;
    std::set<double> changed;
    Trace trace;
    while (next < inputs.size() || simulator.nextEventTime() < adevs_inf<double>()) {
        require(trace.size() < 100, "reference dynamic hierarchy failed to drain");
        const double time = std::min(next < inputs.size() ? inputs[next].first : infinity, simulator.nextEventTime());
        context.frame = Frame{}; context.frame.time = time;
        if (next < inputs.size() && inputs[next].first == time) {
            adevs::PinValue<int> input(graph.groups[0].in[0], inputs[next++].second);
            simulator.injectInput(input);
        }
        simulator.setNextTime(time);
        simulator.computeNextOutput();
        if (changed.insert(time).second) scenario.change(graph, time);
        require(simulator.computeNextState() == time, "reference clock changed during edit");
        context.frame.canonicalize(); trace.push_back(std::move(context.frame));
    }
    return trace;
}

void dynamic_case() {
    const auto trace = native_dynamic<NativeGraph>();
    conformance::equal(trace, native_dynamic<FlatGraph>(), "nested dynamic vs independently flat edits");
    conformance::equal(trace, reference_dynamic(), "nested dynamic subtree edits");
    std::vector<std::pair<double, int>> completions;
    bool redirected = false, passthrough_only = false;
    for (const auto& frame : trace) {
        for (const auto& event : frame.output) {
            require(event.component != 1, "removed busy processor emitted");
            if (event.component == 4) completions.emplace_back(frame.time, event.value);
        }
        if (frame.time == 4.5) {
            for (const auto& event : frame.transitions)
                if (event.component == 3) redirected = event.input == Bag{{0, 3}, {0, 50}};
        }
        if (frame.time == 6.5) passthrough_only = frame.transitions.empty() && frame.boundary == Bag{{0, 60}, {0, 60}};
    }
    require(completions == std::vector<std::pair<double, int>>{{3.5, 40}, {4.5, 3}} && redirected && passthrough_only,
            "dynamic subtree hand schedule, future input rerouting, or cancellation failed");
}
} // namespace
int main() {
    try {
        passthrough_cases(); dynamic_case();
        std::cout << "Four hierarchy cases agree with pinned adevs; passthrough, subtree edits, and hand schedules pass\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
